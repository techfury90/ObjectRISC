/*
 * exit42.c — exits with a non-zero code so you can see the shell's
 * `[exited 42]` reporter at work.
 */

#include "liborisc.h"

int
main(void)
{
	print_str("about to exit 42\n");
	return 42;
}
