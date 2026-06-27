/*
 * termfw.c — Object RISC Terminal Firmware (M1: self-test + splash).
 *
 * The boot image a terminal CPU's ROM hands control to: it allocates the
 * physical framebuffer, runs a VRAM self-test (solid fills with pixel
 * read-back), and — on pass — prints the firmware banner in the embedded
 * fixed-width Lucida Typewriter font, exactly as a late-80s workstation would
 * before downloading its system software.
 *
 * M1 is standalone: it depends on NOTHING but a framebuffer (no supervisor,
 * no WM, no directory/hostfsd).  M2 will replace the final `return 0` with an
 * orx_run of the supervisor; M3 brings the WM up reusing THIS framebuffer.
 *
 * Mechanics mirror two proven references:
 *   - examples/cc/fb_local_smoke.c — the ObjAllocFramebuffer / ObjFetchBytes
 *     idioms + the pcc-orisc input-clobber dance (stage "r" inputs into safe
 *     high registers before the asm body stomps r4-r7).
 *   - oriscwm.c blit_glyphs_winfb — the WM's lutRS failsafe text path: O2=O15
 *     boot data, R6 = (blob - DATA_VA), EXTENDED ObjBlitGlyphs (R5 bit31).
 *     lutRS_blob is a self-describing WMF1 font (8x16, codepoints 32-126), so
 *     the extended path parses it natively — no magic offset into the blob.
 */

#include "liborisc.h"

/* wm_fonts.h also defines font_lutRS as a wm_font_t (oriscwm.c's font
 * descriptor, normally defined before the include there).  termfw only uses
 * the raw lutRS_blob, but the struct must still compile — mirror the typedef. */
typedef struct {
	const unsigned int *blob;
	int cell_w, cell_h, base, n_glyphs, flags, obj_slot;
} wm_font_t;
#include "wm_fonts.h"           /* lutRS_blob (WMF1 8x16, cp 32-126) */

#define FB_W 1280               /* WM native framebuffer (liborisc.h) */
#define FB_H 768
#define FB_BYTES (FB_W * FB_H)  /* 1 byte (palette index) per pixel */

/* Per-pattern self-test delay; overridable so the headless test runs fast
 * (6 * 800ms would be ~5s) via -DDELAY_US=2000. */
#ifndef DELAY_US
#define DELAY_US 800000U
#endif

/* How long the final banner (or a memtest-fail message) stays on screen before
 * M1 exits.  Without it the last frame flashes by — there's no following
 * per-pattern delay to hold it.  (M2's supervisor handoff keeps the screen up,
 * so this becomes moot there.)  Overridden short in the headless test. */
#ifndef BANNER_HOLD_US
#define BANNER_HOLD_US 3000000U
#endif

#define DATA_VA      0x00040000U   /* O15 boot-data base (orx.c) */
#define STACK_BOTTOM 0x001f0000U   /* O11 boot-stack base */

/* Splash palette indices (VEC_PALETTE_HEX, simorisc) — legible on the gray. */
#define COL_BG  10                 /* BG1 #cccccc */
#define COL_FG  14                 /* black */

/* --- OR hygiene: asm/primitives clobber O2/O3; restore before any print --- */
static void
restore_or_state(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}
#define WP(s) do { restore_or_state(); print_str(s); } while (0)

/* === Framebuffer ops — all read the FB ref parked in O5 (set in main after
 * ObjAllocFramebuffer).  None of these primitives WRITE an object register, so
 * O5 survives across them and across these calls (the fb_local_smoke pattern).
 * === */

/* ObjFillRect #0x10D — fill (x,y,w,h) of the FB with palette index `idx`. */
static int
fb_fill(int x, int y, int w, int h, int idx)
{
	int status;
	asm volatile(
		"addu r10, %1, r0\n"        /* stage inputs into the disjoint r10..r12 */
		"addu r11, %2, r0\n"        /* range FIRST (pcc places inputs in r2..r7,*/
		"addu r12, %3, r0\n"        /* which the body stomps) */
		"omov o1, o5\n"             /* O1 = framebuffer */
		"addu r4, r10, r0\n"        /* R4 = (x<<16)|y */
		"addu r5, r11, r0\n"        /* R5 = (w<<16)|h */
		"addu r6, r12, r0\n"        /* R6 = palette idx */
		"call #0x10D\n"
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		: "r"(((x & 0xFFFF) << 16) | (y & 0xFFFF)),
		  "r"(((w & 0xFFFF) << 16) | (h & 0xFFFF)),
		  "r"(idx & 0xFF)
		: "r1", "r2", "r4", "r5", "r6", "r10", "r11", "r12"
	);
	return status;
}

/* ObjFetchBytes #0x108 — read `count` bytes from the FB at `src_off` into the
 * stack buffer `dst` (must be on the boot stack). */
static int
fb_fetch(int src_off, unsigned char *dst, int count)
{
	int dst_off = (int)((unsigned int)dst - STACK_BOTTOM);
	int status;
	asm volatile(
		"addu r10, %1, r0\n"        /* stage src_off */
		"addu r11, %2, r0\n"        /* stage dst_off */
		"addu r12, %3, r0\n"        /* stage count */
		"omov o1, o5\n"             /* O1 = framebuffer (src) */
		"omov o2, o11\n"            /* O2 = boot stack (dst) */
		"addu r4, r10, r0\n"        /* R4 = src_off */
		"addu r5, r11, r0\n"        /* R5 = dst_off */
		"addu r6, r12, r0\n"        /* R6 = count */
		"call #0x108\n"
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		: "r"(src_off), "r"(dst_off), "r"(count)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r10", "r11", "r12"
	);
	return status;
}

/* ObjBlitGlyphs #0x10C (EXTENDED) — draw NUL-terminated `text` at absolute
 * pixel (px,py) in fg/bg, opaque.  Font + text both live in boot data (O15);
 * R6/R7 are their byte offsets from DATA_VA.  Mirrors the WM's failsafe path. */
static int
fb_text(int px, int py, const char *text, int fg, int bg)
{
	int n = 0;
	while (text[n]) n++;
	if (n == 0) return 0;
	if (n > 0x3FFF) n = 0x3FFF;

	int pxy   = ((px & 0xFFFF) << 16) | (py & 0xFFFF);
	int shape = (int)(0x80000000u                  /* EXTENDED */
	                | ((unsigned)(n  & 0x3FFF) << 16)
	                | ((unsigned)(fg & 0xFF)   << 8)
	                |  (unsigned)(bg & 0xFF));
	int foff  = (int)((unsigned int)lutRS_blob - DATA_VA);
	int toff  = (int)((unsigned int)text       - DATA_VA);
	int status;
	asm volatile(
		"addu r10, %1, r0\n"        /* stage pxy   */
		"addu r11, %2, r0\n"        /* stage shape */
		"addu r12, %3, r0\n"        /* stage foff  */
		"addu r13, %4, r0\n"        /* stage toff  */
		"omov o1, o5\n"             /* O1 = framebuffer */
		"omov o2, o15\n"            /* O2 = boot data (font) */
		"omov o3, o15\n"            /* O3 = boot data (text) */
		"addu r4, r10, r0\n"        /* R4 = pixel (px<<16)|py */
		"addu r5, r11, r0\n"        /* R5 = extended shape */
		"addu r6, r12, r0\n"        /* R6 = font off */
		"addu r7, r13, r0\n"        /* R7 = text off */
		"addu r8, r0, r0\n"         /* R8 = clip lo = 0 (no clip) */
		"addu r9, r0, r0\n"         /* R9 = clip hi = 0 */
		"call #0x10C\n"
		"nop\n"
		"addu %0, r2, r0"
		: "=r"(status)
		: "r"(pxy), "r"(shape), "r"(foff), "r"(toff)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9",
		  "r10", "r11", "r12", "r13"
	);
	return status;
}

/* Busy-wait `us` microseconds.  TimeNow (#0x400) touches no object registers,
 * so O5 survives the wait; unsigned subtraction is wrap-safe. */
static void
delay_us(unsigned int us)
{
	unsigned int t0 = time_now_us();
	while (time_now_us() - t0 < us)
		;
}

/* Read back a small span at `off` and confirm every byte equals `idx`.
 * Samples regions rather than the whole 983 KB FB.  Returns 1 if uniform. */
#define SAMPLE 64
static int
verify_uniform(int off, int idx)
{
	unsigned char buf[SAMPLE];
	int i;
	if (fb_fetch(off, buf, SAMPLE) != 0)
		return 0;
	for (i = 0; i < SAMPLE; i++)
		if (buf[i] != (unsigned char)idx)
			return 0;
	return 1;
}

int
main(void)
{
	int i;

	task_init();
	WP("termfw: starting\n");

	/* Allocate the physical (display-backed) framebuffer; park its ref in O5. */
	int alloc_status;
	asm volatile(
		"addiu r4, r0, %1\n"        /* width */
		"addiu r5, r0, %2\n"        /* height */
		"addiu r6, r0, 3\n"         /* CAP_R | CAP_W */
		"addiu r7, r0, 0\n"         /* flags = 0: display-backed (the screen) */
		"call  #0x10A\n"            /* ObjAllocFramebuffer */
		"nop\n"
		"omov  o5, o1\n"            /* park FB ref in O5 */
		"addu  %0, r2, r0"
		: "=r"(alloc_status)
		: "i"(FB_W), "i"(FB_H)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (alloc_status != 0) {
		WP("FAIL: ObjAllocFramebuffer\n");
		return 1;
	}

	/* VRAM self-test: solid full-screen fills, read-back verified at the top
	 * and bottom of memory, ~DELAY_US apart. */
	static const int patterns[6] = { 9, 5, 1, 11, 13, 0 };
	for (i = 0; i < 6; i++) {
		int idx = patterns[i];
		fb_fill(0, 0, FB_W, FB_H, idx);

		int ok = verify_uniform(0, idx)
		      && verify_uniform(FB_BYTES - SAMPLE, idx);
#ifdef FORCE_MEMTEST_FAIL
		if (i == 0) ok = 0;        /* test hook: exercise the fail path */
#endif
		if (!ok) {
			fb_fill(0, 0, FB_W, FB_H, COL_BG);
			fb_text(80, 360,
			        "Memory test failed. Contact your service "
			        "representative for assistance.", COL_FG, COL_BG);
			WP("FAIL: memtest\n");
			delay_us(BANNER_HOLD_US);   /* leave the failure message up */
			return 2;
		}
		delay_us(DELAY_US);
	}

	/* Pass: clear to the desktop gray and show the firmware banner.  The blit
	 * status is checked so a bad font offset (the chief text-path risk) surfaces
	 * as a FAIL rather than a silently blank screen. */
	fb_fill(0, 0, FB_W, FB_H, COL_BG);
	if (fb_text(80, 348, "Object RISC Terminal Firmware 1.0",
	            COL_FG, COL_BG) != 0
	 || fb_text(80, 372, "Self-test passed. Downloading system software...",
	            COL_FG, COL_BG) != 0) {
		WP("FAIL: ObjBlitGlyphs\n");
		return 3;
	}

	WP("termfw: self-test PASS\n");

#ifdef STOP_AFTER_SPLASH
	delay_us(BANNER_HOLD_US);       /* M1 standalone: hold the banner, then exit */
	return 0;
#else
	/* ---- M2: hand off to the co-resident supervisor ------------------------
	 * The directory cap (boot O8) is the ONLY wired input; everything else
	 * derives from it.  (Validated via the M2 spike.)  The banner stays up
	 * meanwhile — the supervisor draws nothing until the WM comes up (M3). */

	/* Drop our framebuffer from O5 BEFORE spawning.  A child inherits the
	 * parent's O5, and a non-null O5 makes the supervisor think it has a console
	 * terminal — it then SENDs to the framebuffer (which lacks the S cap) and
	 * traps.  At M2 the supervisor has no terminal (the FB→WM handoff is M3); the
	 * banner stays on screen regardless, since the FB object itself persists. */
	asm volatile("onull o5");

	/* Promote the boot directory into DIR_SLOT (for our own dir_walks) AND
	 * forward it into ORX_SLOT_CHILD_O8 so the supervisor task harvests
	 * O8 = directory (its boot ABI) rather than a spawn-service sub-cap. */
	asm volatile(
		"orefld o1, 544(o12)\n"     /* BOOT_PARENT_SLOT = directory */
		"orefst o1, 584(o12)\n"     /* -> DIR_SLOT */
		"orefst o1, 560(o12)"       /* -> ORX_SLOT_CHILD_O8 */
		: : : "r1"
	);

	/* Derive hostfsd from the directory: walk /sys/hostfsd/0 -> O10, then
	 * hf_init adopts it (orx_spawn reads the .orx header through it). */
	{
		int kind;
		char rem[16];
		if (dir_walk("/sys/hostfsd/0", &kind, rem, sizeof(rem)) >= 0)
			asm volatile("orefld o10, 616(o12)");   /* DIR_RESULT_SLOT -> O10 */
	}
	if (hf_init() != 0) { WP("FAIL: hf_init\n"); return 4; }
	orx_init();                     /* map the args-parent region + orx state */

	/* Wait for oriscdir's DEFERRED /programs mount (applied when hostfsd
	 * self-registers), then load + run the supervisor as a SAME-CPU task and
	 * idle-yield so it gets scheduled. */
	{
		int kind = 0, attempt;
		char rem[16];
		for (attempt = 0; attempt < 400; attempt++) {
			if (dir_walk("/programs", &kind, rem, sizeof(rem)) >= 0
			    && kind == DIR_KIND_MOUNT)
				break;
			task_yield();
		}
	}
	{
		task_t sup = orx_spawn("/programs/supervisor.orx", "", "/");
		if (sup < 0) { WP("FAIL: supervisor load\n"); return 5; }
	}
	WP("termfw: system software running\n");
	for (;;)
		task_yield();               /* firmware idles; supervisor is co-resident */
#endif
}
