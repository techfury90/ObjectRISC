/*
 * fb_smoke.c — smoke test for oriscterm's framebuffer service
 * (FRAMEBUFFER_INDEX = 7, milestone-3-α).
 *
 * Walks /sys/term/0/framebuffer to get the framebuffer cap, writes a
 * byte pattern via OSB, reads it back via OLBU, and verifies each
 * byte round-trips.  OSB / OLBU on a remote ref auto-trigger
 * OBJ_WRITE_REQ / OBJ_READ_REQ wire round-trips to oriscterm, which
 * is what this test exercises.
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

	/* Step 4: bounds check — write past end of framebuffer should
	 * fail.  oriscterm's bounds enforcement returns RESP_BOUNDS,
	 * which simorisc translates to a CPU-side fault.  We can't
	 * easily catch the fault from C, so skip this check for now;
	 * the bounds check is exercised by oriscterm's own logging. */

	WP("fb_smoke: PASS\n");
	return 0;
}
