/*
 * count.c — prints 1 through 10 to demonstrate that print_int +
 * loops + the data-segment mapping all work in a spawned child.
 */

#include "liborisc.h"

int
main(void)
{
	int i;
	for (i = 1; i <= 10; i++) {
		print_int(i);
		print_str("\n");
	}
	return 0;
}
