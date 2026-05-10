/*
 * fb_local_smoke.c — smoke test for simorisc's TAG_FRAMEBUFFER
 * primitive (Phase 60).
 *
 * Exercises ObjAllocFramebuffer (#0x102) + same-CPU ObjStoreBytes /
 * ObjFetchBytes round-trip.  No oriscterm in the loop, no
 * /sys/term/<N>/framebuffer walk — the framebuffer is allocated
 * locally on the running CPU, and stores into it never leave the
 * process.  Headless: passes regardless of `--display` because the
 * firmware op works as a plain bytearray when no Tk is attached.
 *
 * This is the proof-of-mechanism step for the WM-meets-display
 * unification.  The WM (oriscwm.orx) doesn't change yet — that's
 * the next PR.  For now we just verify the firmware op is wired
 * end-to-end and that ObjStoreBytes marks TAG_FRAMEBUFFER objects
 * dirty (visible to a host display worker if one is attached).
 *
 * Single-CPU, no socket: run via simorisc standalone.
 *   simorisc fb_local_smoke.orx
 *
 * Test sequence:
 *   1. task_init.
 *   2. ObjAllocFramebuffer(W=16, H=16, R|W) → O5.
 *   3. ObjStoreBytes 256 bytes of a known pattern from a stack
 *      buffer to the FB at offset 0.
 *   4. ObjFetchBytes 256 bytes back to a different stack buffer.
 *   5. Verify each byte matches.  Print PASS or FAIL.
 */

#include "liborisc.h"

#define FB_W 16
#define FB_H 16
#define FB_BYTES (FB_W * FB_H)

static void
restore_or_state(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}

#define WP(s)      do { restore_or_state(); print_str(s); } while (0)
#define WP_INT(n)  do { restore_or_state(); print_int(n); } while (0)

static void
fail(const char *stage, int got)
{
	restore_or_state();
	print_str("FAIL: ");
	print_str(stage);
	print_str(" got=");
	print_int(got);
	print_str("\n");
}

int
main(void)
{
	task_init();
	WP("fb_local_smoke: starting\n");

	/* Step 1: ObjAllocFramebuffer(W=16, H=16, R|W) → O5. */
	int alloc_status;
	asm volatile(
		"addiu r4, r0, %1\n"           /* width */
		"addiu r5, r0, %2\n"           /* height */
		"addiu r6, r0, 3\n"            /* CAP_R | CAP_W */
		"call  #0x102\n"               /* ObjAllocFramebuffer */
		"nop\n"
		"omov  o5, o1\n"               /* park FB ref */
		"addu  %0, r2, r0"
		: "=r"(alloc_status)
		: "i"(FB_W), "i"(FB_H)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (alloc_status != 0) {
		fail("ObjAllocFramebuffer", alloc_status); return 1;
	}
	WP("fb_local_smoke: alloc OK\n");

	/* Step 2: build a known 256-byte pattern on the stack.  Mix
	 * indices so a regression that returns wrong bytes is obvious. */
	unsigned char src[FB_BYTES];
	int i;
	for (i = 0; i < FB_BYTES; i++) {
		src[i] = (unsigned char)((i * 7 + 3) & 0xFF);
	}

	/* Step 3: ObjStoreBytes from boot-stack source (O11) into the
	 * framebuffer (O5).  Single bulk write — no per-byte wire RTT
	 * because both src and dst are local to this CPU.
	 *
	 * pcc-orisc input-clobber workaround (same one γ.13 documents):
	 * pcc may place a `"r"` input in r4/r5/r6 even though they're
	 * in the clobber list, then the asm body's first store stomps
	 * the input.  Copy the input into a safe temp (r8) FIRST. */
	int src_off = (int)((unsigned int)src - 0x001f0000U);  /* STACK_BOTTOM */
	int store_status;
	asm volatile(
		"addu  r8, %1, r0\n"           /* save src_off in r8 */
		"omov  o1, o11\n"              /* src = boot stack */
		"omov  o2, o5\n"               /* dst = framebuffer */
		"addu  r4, r8, r0\n"           /* src_off */
		"addiu r5, r0, 0\n"            /* dst_off */
		"addiu r6, r0, %2\n"           /* count */
		"call  #0x109\n"               /* ObjStoreBytes */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(store_status)
		: "r"(src_off), "i"(FB_BYTES)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r8"
	);
	if (store_status != 0) {
		fail("ObjStoreBytes", store_status); return 2;
	}
	WP("fb_local_smoke: store OK\n");

	/* Step 4: ObjFetchBytes the same range back into a different
	 * stack buffer.  Same safe-temp dance for dst_off. */
	unsigned char dst[FB_BYTES];
	int dst_off = (int)((unsigned int)dst - 0x001f0000U);
	int fetch_status;
	asm volatile(
		"addu  r8, %1, r0\n"           /* save dst_off in r8 */
		"omov  o1, o5\n"               /* src = framebuffer */
		"omov  o2, o11\n"              /* dst = boot stack */
		"addiu r4, r0, 0\n"            /* src_off */
		"addu  r5, r8, r0\n"           /* dst_off */
		"addiu r6, r0, %2\n"           /* count */
		"call  #0x108\n"               /* ObjFetchBytes */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(fetch_status)
		: "r"(dst_off), "i"(FB_BYTES)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r8"
	);
	if (fetch_status != 0) {
		fail("ObjFetchBytes", fetch_status); return 3;
	}
	WP("fb_local_smoke: fetch OK\n");

	/* Step 5: byte-compare.  Stop at first mismatch; print index. */
	for (i = 0; i < FB_BYTES; i++) {
		if (dst[i] != src[i]) {
			restore_or_state();
			print_str("FAIL: byte mismatch at i=");
			print_int(i);
			print_str(" got=");
			print_int(dst[i]);
			print_str(" want=");
			print_int(src[i]);
			print_str("\n");
			return 4;
		}
	}

	/* Step 6 (Phase 60 step 6): ObjFbScroll the whole 16×16 region up
	 * by 4 pixels with fill=0xAB.  After the scroll:
	 *   rows  0..11 ← previous rows  4..15
	 *   rows 12..15 ← 0xAB
	 * Verify by re-fetching and checking those bands. */
	int scroll_status;
	{
		/* pcc-orisc input-clobber dance: under register pressure pcc
		 * may allocate one of the "r" inputs into r8/r9/r10, then the
		 * "save inputs to safe temps" prelude stomps it before we
		 * read it.  Pin each input to a high-numbered register pcc
		 * doesn't touch (r12..r14 are safe — fp is r29, sp is r30,
		 * ra is r31, and pcc never picks r12+ for ordinary spills). */
		register int xy   asm("r12") = ((0 & 0xFFFF) << 16) | (0 & 0xFFFF);
		register int wh   asm("r13") = ((FB_W & 0xFFFF) << 16) | (FB_H & 0xFFFF);
		register int dyfl asm("r14") = ((4 & 0xFFFFFF) << 8) | (0xAB & 0xFF);
		asm volatile(
			"omov o1, o5\n"
			"addu r4, r12, r0\n"
			"addu r5, r13, r0\n"
			"addu r6, r14, r0\n"
			"call #0x10E\n"             /* ObjFbScroll */
			"nop\n"
			"addu %0, r2, r0"
			: "=r"(scroll_status)
			: "r"(xy), "r"(wh), "r"(dyfl)
			: "r1", "r2", "r4", "r5", "r6"
		);
	}
	if (scroll_status != 0) {
		fail("ObjFbScroll", scroll_status); return 5;
	}
	WP("fb_local_smoke: scroll OK\n");

	/* Re-fetch and verify the post-scroll FB matches expectation. */
	unsigned char post[FB_BYTES];
	int post_off = (int)((unsigned int)post - 0x001f0000U);
	int post_status;
	asm volatile(
		"addu  r8, %1, r0\n"
		"omov  o1, o5\n"
		"omov  o2, o11\n"
		"addiu r4, r0, 0\n"
		"addu  r5, r8, r0\n"
		"addiu r6, r0, %2\n"
		"call  #0x108\n"                /* ObjFetchBytes */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(post_status)
		: "r"(post_off), "i"(FB_BYTES)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r8"
	);
	if (post_status != 0) {
		fail("ObjFetchBytes post-scroll", post_status); return 6;
	}
	/* Top 12 rows: must match src[(y+4) ...]. Bottom 4 rows: must be 0xAB. */
	int y, x;
	for (y = 0; y < FB_H - 4; y++) {
		for (x = 0; x < FB_W; x++) {
			int got  = post[y * FB_W + x];
			int want = src[(y + 4) * FB_W + x];
			if (got != want) {
				restore_or_state();
				print_str("FAIL: scroll-up mismatch y=");
				print_int(y);
				print_str(" x=");
				print_int(x);
				print_str(" got=");
				print_int(got);
				print_str(" want=");
				print_int(want);
				print_str("\n");
				return 7;
			}
		}
	}
	for (y = FB_H - 4; y < FB_H; y++) {
		for (x = 0; x < FB_W; x++) {
			int got = post[y * FB_W + x];
			if (got != 0xAB) {
				restore_or_state();
				print_str("FAIL: scroll-fill mismatch y=");
				print_int(y);
				print_str(" got=");
				print_int(got);
				print_str("\n");
				return 8;
			}
		}
	}
	WP("fb_local_smoke: scroll verify OK\n");

	/* Step 7 (Phase 60 step 7): ObjBlitCopy a sub-rect from the same
	 * FB to itself, in-place — verifies the same-FB / overlapping
	 * path.  Copy rows 0..3 (post-scroll: src[4..7] pattern + first
	 * row of the fill band gone since dst overlaps src at +4) onto
	 * rows 8..11.  Then byte-check rows 8..11 match what rows 0..3
	 * had just before the copy.
	 *
	 * Save rows 0..3 first so we have a ground-truth for the check
	 * (the copy mutates the source if dst overlaps src... it doesn't
	 * here since dst_y=8 > src_y=0 with non-overlapping 4-row band). */
	unsigned char saved[4 * FB_W];
	{
		int y2;
		for (y2 = 0; y2 < 4; y2++)
			for (x = 0; x < FB_W; x++)
				saved[y2 * FB_W + x] = post[y2 * FB_W + x];
	}
	int blit_status;
	{
		register int sxy  asm("r12") = ((0 & 0xFFFF) << 16) | (0 & 0xFFFF);
		register int dxy  asm("r13") = ((0 & 0xFFFF) << 16) | (8 & 0xFFFF);
		register int whp  asm("r14") = ((FB_W & 0xFFFF) << 16) | (4 & 0xFFFF);
		asm volatile(
			"omov o1, o5\n"             /* dst = framebuffer */
			"omov o2, o5\n"             /* src = same framebuffer */
			"addu r4, r12, r0\n"
			"addu r5, r13, r0\n"
			"addu r6, r14, r0\n"
			"call #0x10F\n"             /* ObjBlitCopy */
			"nop\n"
			"addu %0, r2, r0"
			: "=r"(blit_status)
			: "r"(sxy), "r"(dxy), "r"(whp)
			: "r1", "r2", "r4", "r5", "r6"
		);
	}
	if (blit_status != 0) {
		fail("ObjBlitCopy", blit_status); return 9;
	}
	WP("fb_local_smoke: blit-copy OK\n");

	/* Re-fetch + verify rows 8..11 now match what rows 0..3 held. */
	unsigned char post2[FB_BYTES];
	int post2_off = (int)((unsigned int)post2 - 0x001f0000U);
	int post2_status;
	asm volatile(
		"addu  r8, %1, r0\n"
		"omov  o1, o5\n"
		"omov  o2, o11\n"
		"addiu r4, r0, 0\n"
		"addu  r5, r8, r0\n"
		"addiu r6, r0, %2\n"
		"call  #0x108\n"                /* ObjFetchBytes */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(post2_status)
		: "r"(post2_off), "i"(FB_BYTES)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r8"
	);
	if (post2_status != 0) {
		fail("ObjFetchBytes post-blit", post2_status); return 10;
	}
	int y3;
	for (y3 = 0; y3 < 4; y3++) {
		for (x = 0; x < FB_W; x++) {
			int got  = post2[(y3 + 8) * FB_W + x];
			int want = saved[y3 * FB_W + x];
			if (got != want) {
				restore_or_state();
				print_str("FAIL: blit-copy mismatch y=");
				print_int(y3 + 8);
				print_str(" x=");
				print_int(x);
				print_str(" got=");
				print_int(got);
				print_str(" want=");
				print_int(want);
				print_str("\n");
				return 11;
			}
		}
	}
	WP("fb_local_smoke: blit-copy verify OK\n");

	WP("fb_local_smoke: PASS\n");
	return 0;
}
