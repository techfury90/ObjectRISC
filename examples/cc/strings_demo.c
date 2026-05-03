/*
 * strings_demo.c — exercise the new liborisc string functions.
 *
 * Demonstrates that calls into the libc archive resolve correctly
 * (orld pulls in just the .oro members that satisfy the references
 * — string.oro for strlen/strcmp/strcpy/memset, io.oro for the
 * print helpers).
 */

#include "liborisc.h"

const char greeting[] = "hello, libc";

int
main(void)
{
	char buf[32];

	/* Format a small banner using strcpy + strlen + memset. */
	memset(buf, '=', 11);
	buf[11] = '\n';
	buf[12] = '\0';
	print_str(buf);

	strcpy(buf, greeting);
	print_str(buf);
	print_str("\n");

	memset(buf, '=', 11);
	buf[11] = '\n';
	buf[12] = '\0';
	print_str(buf);

	/* strlen + print_int. */
	print_str("strlen(\"hello, libc\") = ");
	print_int((int)strlen(greeting));
	print_str("\n");

	/* strcmp: print 1 / 0 / -1 outcomes. Need an int variable since
	 * print_int wants an int and the strcmp normalisation fits. */
	print_str("strcmp self        = ");
	print_int(strcmp(greeting, greeting));
	print_str("\n");

	print_str("strcmp self vs ZZZ = ");
	print_int(strcmp(greeting, "ZZZ"));
	print_str("  ('h' > 'Z' in ASCII, so positive)\n");

	/* atoi + print_hex. */
	print_str("atoi(\"-42\")        = ");
	print_int(atoi("-42"));
	print_str("\n");

	print_str("print_hex(0xC0FFEE) = ");
	print_hex(0xC0FFEE);
	print_str("\n");

	return 0;
}
