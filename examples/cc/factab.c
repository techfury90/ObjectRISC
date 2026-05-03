/*
 * factab.c — print 1! through 7! using a recursive factorial.
 *
 * Exercises recursion (factorial), loops with conditionals,
 * pointer arithmetic via the shared lib helpers, integer
 * arithmetic — across two compilation units linked together
 * (see examples/cc/lib.c for print_str / print_int).
 */

extern void print_str(const char *s);
extern void print_int(int n);

static int
factorial(int n)
{
	if (n <= 1) return 1;
	return n * factorial(n - 1);
}

int
main(void)
{
	int i;

	for (i = 1; i <= 7; i++) {
		print_int(i);
		print_str("! = ");
		print_int(factorial(i));
		print_str("\n");
	}
	return 0;
}
