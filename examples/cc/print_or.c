/*
 * print_or.c — print "Hello, world!" using only C and inline asm.
 *
 * No console_io.s bridge: this program controls the OR file
 * directly via `register __or T * __asm__("oN")` declarations,
 * then issues the ConsoleWrite firmware primitive through inline
 * asm. The OR setup is real C; only the firmware-call sequence
 * itself is asm.
 *
 * Compare hello.c — that one compiles to identical output but
 * relies on tools/cc/arch/orisc/console_io.s to bridge between C
 * calling convention and the OR file.
 */

const char hello[] = "Hello, world!\n";

static int
firmware_console_write(int offset, int count)
{
	int status;

	/* Inline asm sets R4 = offset, R5 = count, invokes
	 * firmware ConsoleWrite (primitive 0x320), then captures
	 * R2 (status) into the named output operand. The source
	 * object reference must already be in O1 — set by the
	 * caller with a `register __or void * __asm__("o1")`. */
	asm("addu r4, %1, r0\n"
	    "addu r5, %2, r0\n"
	    "call #0x320\n"
	    "nop\n"
	    "addu %0, r2, r0\n"
	    : "=r"(status)
	    : "r"(offset), "r"(count));
	return status;
}

int
main(void)
{
	register __or void *o1_src   __asm__("o1");
	register __or void *o3_data  __asm__("o3");

	o1_src = o3_data;                      /* omov o1, o3 */
	firmware_console_write(0, 14);
	return 0;
}
