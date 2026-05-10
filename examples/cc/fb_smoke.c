/*
 * fb_smoke.c — smoke test for oriscterm's framebuffer service
 * (FRAMEBUFFER_INDEX = 7).
 *
 * Two flavours of round-trip:
 *
 *   - Per-byte OSB / OLBU on the framebuffer's remote ref —
 *     auto-triggers OBJ_WRITE_REQ / OBJ_READ_REQ wire round-trips,
 *     one per instruction.  Original α-stage path.
 *
 *   - Bulk ObjStoreBytes (#0x109) / ObjFetchBytes (#0x108) — same
 *     wire packets but with arbitrary widths in a single RTT.
 *     Phase 59: the WM's eventual glyph renderer needs this to
 *     push 8×16 = 128-byte font cells in one round-trip instead
 *     of 128.
 *
 * Boot environment (set up by test_framebuffer.sh's --service args):
 *
 *     O3 = our data segment (preserved by task_init in O15)
 *     O4 = our self-service
 *     O8 = oriscdir mailbox sub-cap (BOOT_PARENT_SLOT after
 *          task_init; we promote it to DIR_SLOT so dir.c finds the
 *          directory without going through SUP_OP_GET_DIR — we have
 *          no parent supervisor)
 *
 * Test sequence:
 *
 *     1. task_init + promote BOOT_PARENT to DIR_SLOT.
 *     2. dir_walk("/sys/term/0/framebuffer") → OREFLD result into O5.
 *     3. OSB four bytes (0x42, 0x43, 0x44, 0x45) at offsets 0..3.
 *     4. OLBU each back; verify they match.
 *     5. Print PASS or FAIL.
 *
 * OR hygiene
 * ----------
 * Same as wm_smoke: every wire op (dir_walk, OSB, OLBU) overlays
 * O2..O4 from the response.  task_init parks the boot stack in O11
 * and the boot data in O15; we restore O2/O3 from those before any
 * print_str / print_int.
 */

#include "liborisc.h"

static void
restore_or_state(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}

#define WP(s)      do { restore_or_state(); print_str(s); } while (0)
#define WP_INT(n)  do { restore_or_state(); print_int(n); } while (0)

static void
fail(const char *stage, int got, int want)
{
	restore_or_state();
	print_str("FAIL: ");
	print_str(stage);
	print_str(" got=");
	print_int(got);
	print_str(" want=");
	print_int(want);
	print_str("\n");
}

static void
promote_boot_parent_to_dir_slot(void)
{
	asm volatile(
		"orefld o1, 544(o12)\n"     /* BOOT_PARENT_SLOT */
		"orefst o1, 584(o12)"       /* DIR_SLOT */
		: : : "r1"
	);
}

int
main(void)
{
	task_init();
	promote_boot_parent_to_dir_slot();

	WP("fb_smoke: starting\n");

	/* Step 1: walk for the framebuffer cap. */
	int kind;
	char rem[16];
	int rc = dir_walk("/sys/term/0/framebuffer", &kind, rem, sizeof(rem));
	if (rc != 0)              { fail("dir_walk", rc, 0); return 1; }
	if (kind != DIR_KIND_LEAF) { fail("kind", kind, DIR_KIND_LEAF); return 1; }
	WP("fb_smoke: dir_walk OK\n");

	/* Move the resolved framebuffer ref from DIR_RESULT_SLOT into O5
	 * so OSB/OLBU can address it via the OPR-relative encoding. */
	asm volatile("orefld o5, 616(o12)");

	/* Step 2: write a four-byte pattern at offsets 0..3.  OSB writes
	 * 1 byte; for a remote ref this triggers OBJ_WRITE_REQ → blocks
	 * → resumes on OBJ_WRITE_RESP.  oriscterm's _handle_obj_write_req
	 * receives, updates self.framebuffer, replies. */
	asm volatile("addiu r4, r0, 0x42\nosb r4, 0(o5)" ::: "r4");
	asm volatile("addiu r4, r0, 0x43\nosb r4, 1(o5)" ::: "r4");
	asm volatile("addiu r4, r0, 0x44\nosb r4, 2(o5)" ::: "r4");
	asm volatile("addiu r4, r0, 0x45\nosb r4, 3(o5)" ::: "r4");
	restore_or_state();
	WP("fb_smoke: write OK\n");

	/* Step 3: read back and verify.  OLBU loads 1 byte unsigned. */
	int b0, b1, b2, b3;
	asm volatile("olbu %0, 0(o5)" : "=r"(b0));
	asm volatile("olbu %0, 1(o5)" : "=r"(b1));
	asm volatile("olbu %0, 2(o5)" : "=r"(b2));
	asm volatile("olbu %0, 3(o5)" : "=r"(b3));
	restore_or_state();

	if (b0 != 0x42) { fail("byte 0", b0, 0x42); return 2; }
	if (b1 != 0x43) { fail("byte 1", b1, 0x43); return 2; }
	if (b2 != 0x44) { fail("byte 2", b2, 0x44); return 2; }
	if (b3 != 0x45) { fail("byte 3", b3, 0x45); return 2; }

	WP("fb_smoke: read-back OK (b0=");
	WP_INT(b0);
	WP(" b1=");
	WP_INT(b1);
	WP(" b2=");
	WP_INT(b2);
	WP(" b3=");
	WP_INT(b3);
	WP(")\n");

	/* Step 4: ObjStoreBytes round-trip.  Build a 16-byte pattern
	 * on the stack, push to framebuffer at offset 100, fetch back
	 * into a separate stack buffer, verify each byte matches.
	 *
	 * Stack VAs run [STACK_BOTTOM, STACK_TOP) where STACK_BOTTOM
	 * = 0x001f0000.  C local arrays land here at runtime; we
	 * compute their offset within the boot stack object (O11) by
	 * subtracting STACK_BOTTOM. */
	#define STACK_BOTTOM 0x001f0000

	unsigned char src_buf[16];
	int i;
	for (i = 0; i < 16; i++) src_buf[i] = (unsigned char)(0x80 + i);

	int src_offset_i = (int)((unsigned int)(unsigned long)src_buf - STACK_BOTTOM);

	/* ObjStoreBytes: O1 = stack source (boot stack ref in O11),
	 * O2 = framebuffer remote dest, R4 = src_off, R5 = dst_off,
	 * R6 = count.  Returns R2 = status, R3 = bytes copied. */
	int store_status, store_count;
	asm volatile(
		"omov  o1, o11\n"
		"omov  o2, o5\n"
		"addu  r4, %2, r0\n"
		"addiu r5, r0, 100\n"
		"addiu r6, r0, 16\n"
		"call  #0x109\n"            /* ObjStoreBytes */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(store_status), "=r"(store_count)
		: "r"(src_offset_i)
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);
	restore_or_state();
	if (store_status != 0)  { fail("ObjStoreBytes status", store_status, 0); return 4; }
	if (store_count != 16)  { fail("ObjStoreBytes count", store_count, 16); return 4; }
	WP("fb_smoke: ObjStoreBytes 16-byte push OK\n");

	/* ObjFetchBytes: pull the same bytes back into a fresh stack
	 * buffer, verify the pattern round-trips. */
	unsigned char dst_buf[16];
	for (i = 0; i < 16; i++) dst_buf[i] = 0;   /* clear */

	int dst_offset_i = (int)((unsigned int)(unsigned long)dst_buf - STACK_BOTTOM);

	int fetch_status, fetch_count;
	asm volatile(
		"omov  o1, o5\n"            /* source = framebuffer */
		"omov  o2, o11\n"           /* dest = boot stack */
		"addiu r4, r0, 100\n"
		"addu  r5, %2, r0\n"
		"addiu r6, r0, 16\n"
		"call  #0x108\n"            /* ObjFetchBytes */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(fetch_status), "=r"(fetch_count)
		: "r"(dst_offset_i)
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);
	restore_or_state();
	if (fetch_status != 0) { fail("ObjFetchBytes status", fetch_status, 0); return 5; }
	if (fetch_count != 16) { fail("ObjFetchBytes count", fetch_count, 16); return 5; }

	for (i = 0; i < 16; i++) {
		if (dst_buf[i] != (unsigned char)(0x80 + i)) {
			fail("ObjStoreBytes round-trip byte mismatch",
			     dst_buf[i], 0x80 + i);
			return 5;
		}
	}
	WP("fb_smoke: ObjStoreBytes round-trip OK\n");

	WP("fb_smoke: PASS\n");
	return 0;
}
