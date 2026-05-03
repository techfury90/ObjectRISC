/*
 * hello.c — first C program for Object RISC.
 *
 * Compiles with the orisc pcc port (under tools/cc/arch/orisc/) and
 * links against crt0.s (which provides the entry point and calls
 * TaskExit) and console_io.s (a small bridge to the firmware
 * ConsoleWrite primitive — needed until the C compiler grows the
 * `__or` qualifier and OR-file patterns to call firmware directly).
 *
 * Build via examples/cc/run_hello_c.sh.
 */

extern int console_write(const char *buf, int count);

const char hello[] = "Hello, world!\n";

int
main(void)
{
	console_write(hello, sizeof(hello) - 1);
	return 0;
}
