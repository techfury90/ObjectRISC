/*
 * kbd_echo.c — subscribe to the terminal's keyboard service and echo
 * each keystroke (codepoint + modifiers) to host stdout. Press ESC
 * (codepoint 0x11B in the terminal's portable encoding) to exit.
 *
 * Boot environment expected (set up by the runner script):
 *     O3 = our data segment (preserved as O15 below — see "OR hygiene")
 *     O4 = our self-service (full caps; queue receives key events here)
 *     O5 = terminal console object  (text output)        — unused here
 *     O6 = terminal keyboard object (subscribe target)
 *
 * OR hygiene
 * ----------
 * Three registers need active save/restore around the poll loop —
 * print_int and print_char both use stack buffers, which console_io's
 * VA heuristic reaches via O2 (the stack object), not O3:
 *
 *   O2 — stack ref. console_write picks this for any buffer in the
 *        stack VA range, so print_char / print_int fail silently
 *        without it.
 *   O3 — data ref. console_write picks this for data-segment
 *        buffers (string literals), so print_str needs it.
 *   O4 — our self-service, the target of ReceiveQueuePoll itself.
 *        Without it the *next* poll's `omov o1, o4` sees null and
 *        returns EFAULT silently.
 *
 * ReceiveQueuePoll's overlay sets O1..O4 from the wire payload
 * (always [sub_ref, 0, 0, 0] for our key events), so all three of
 * O2/O3/O4 get nuked on every successful poll. We park them once
 * in O13/O14/O15 at startup and copy back after each poll.
 *
 * Wire protocol — see tools/devices/README.md for the canonical writeup.
 *
 *   subscribe:    SEND to O6 with O2 = derived(R|S) self-ref
 *   key event:    terminal SENDs to that ref with R4 = codepoint,
 *                 R5 = modifier mask
 */

#include "liborisc.h"

/* Keep these in sync with the KEY_ / MOD_ constants in
 * tools/devices/oriscterm. */
#define KEY_ESCAPE    0x11B
#define KEY_BACKSPACE 0x108
#define KEY_RETURN    0x10D
#define KEY_TAB       0x109
#define KEY_LEFT      0x182
#define KEY_RIGHT     0x183
#define KEY_UP        0x180
#define KEY_DOWN      0x181

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_META  0x08

int
main(void)
{
	register void *__or o2_stack       __asm__("o2");
	register void *__or o3_data        __asm__("o3");
	register void *__or o4_self        __asm__("o4");
	register void *__or o6_kbd         __asm__("o6");
	register void *__or o7_subref      __asm__("o7");
	register void *__or o13_stack_save __asm__("o13");
	register void *__or o14_self_save  __asm__("o14");
	register void *__or o15_data_save  __asm__("o15");
	int status;
	int code, mods;

	/* Stash the boot-time stack, data, and self-service refs — see
	 * "OR hygiene" in the file header. */
	o13_stack_save = o2_stack;
	o14_self_save  = o4_self;
	o15_data_save  = o3_data;

	/* Step 1. Derive an R|S self-ref to hand the terminal as our
	 * subscription cap. */
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, 9\n"        /* R|S = 0x09 */
		"call  #0x103\n"           /* ObjDerive */
		"nop\n"
		"omov  %0, o1"
		: "=r"(o7_subref)
		:
		: "r1", "r2", "r4"
	);

	/* Step 2. ReceiveQueueAttach on our self-service so key events
	 * land in a queue we can poll synchronously. */
	asm volatile(
		"omov  o1, o4\n"
		"addiu r4, r0, 16\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	/* attach doesn't touch ORs — no restoration needed. */
	if (status != 0) {
		print_str("attach failed: status=");
		print_int(status);
		print_str("\n");
		return 1;
	}

	/* Step 3. Subscribe — SEND to the terminal's keyboard service
	 * with our derived self-ref in O2. */
	asm volatile(
		"omov  o1, %0\n"
		"omov  o2, %1\n"
		"onull o3\n"
		"addiu r4, r0, 0\n"
		"addiu r5, r0, 0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "r"(o6_kbd), "r"(o7_subref)
		: "r1", "r4", "r5", "r6", "r7"
	);
	o2_stack = o13_stack_save;
	o3_data  = o15_data_save;

	print_str("kbd_echo ready — focus the terminal and type "
	          "(ESC to quit)\n");

	while (1) {
		/* Poll the queue. Sender's R4..R7 land in our R3..R6;
		 * sender's O1..O4 overlay our O1..O4. */
		asm volatile(
			"omov  o1, o4\n"
			"addiu r4, r0, -1\n"   /* infinite timeout */
			"call  #0x204\n"
			"nop\n"
			"addu  %0, r2, r0\n"
			"addu  %1, r3, r0\n"
			"addu  %2, r4, r0"
			: "=r"(status), "=r"(code), "=r"(mods)
			:
			: "r1", "r2", "r3", "r4"
		);
		o2_stack = o13_stack_save;  /* restore for stack-buffer prints */
		o3_data  = o15_data_save;   /* restore for data-buffer prints */
		o4_self  = o14_self_save;   /* restore for the next poll */
		if (status != 0) {
			print_str("poll failed: status=");
			print_int(status);
			print_str("\n");
			return 2;
		}

		if (code == KEY_ESCAPE) {
			print_str("ESC — exiting\n");
			break;
		}

		print_str("key=");
		print_int(code);
		if (code >= 32 && code < 127) {
			print_str(" '");
			print_char(code);
			print_str("'");
		} else if (code == KEY_BACKSPACE) print_str(" [BackSpace]");
		else if (code == KEY_RETURN)      print_str(" [Return]");
		else if (code == KEY_TAB)         print_str(" [Tab]");
		else if (code == KEY_UP)          print_str(" [Up]");
		else if (code == KEY_DOWN)        print_str(" [Down]");
		else if (code == KEY_LEFT)        print_str(" [Left]");
		else if (code == KEY_RIGHT)       print_str(" [Right]");
		if (mods) {
			print_str(" mods=");
			if (mods & MOD_SHIFT) print_str("S");
			if (mods & MOD_CTRL)  print_str("C");
			if (mods & MOD_ALT)   print_str("A");
			if (mods & MOD_META)  print_str("M");
		}
		print_str("\n");
	}

	/* Step 4. Unsubscribe — SEND with O2 = null. */
	asm volatile(
		"omov  o1, %0\n"
		"onull o2\n"
		"onull o3\n"
		"addiu r4, r0, 0\n"
		"addiu r5, r0, 0\n"
		"addiu r6, r0, 0\n"
		"addiu r7, r0, 0\n"
		"send  o1"
		:
		: "r"(o6_kbd)
		: "r1", "r4", "r5", "r6", "r7"
	);

	return 0;
}
