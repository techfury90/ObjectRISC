/*
 * print_via_or_arg.c — callee-side `__or` parameter demo.
 *
 * The new wrinkle this exercises: a pure-C function whose
 * parameter is `__or`-qualified, and whose body uses that
 * parameter without an explicit `register __or T *p
 * __asm__("oN")` binding. The compiler is responsible for
 * keeping the value in the OR file across the body.
 *
 * print_line takes `void *__or src` (arrives in O1 by the
 * caller-side convention) and passes it through to
 * orisc_console_write (which also takes its first arg in O1).
 * The body's reference to `src` resolves directly to the OR
 * slot via the SINREG path in bfcode — no tempnode, no
 * cross-class coalesce error.
 */

extern int orisc_console_write(void *__or src, int offset, int count);

static void
print_line(void *__or src, int len)
{
	orisc_console_write(src, 0, len);
}

const char banner[] = "callee-side __or works!\n";

int
main(void)
{
	register void *__or o3_data __asm__("o3");

	print_line(o3_data, 24);
	return 0;
}
