/*
 * factab.c — print 1! through 7! using a recursive factorial.
 *
 * Exercises a fairly broad slice of the C compiler:
 *   - recursion (factorial)
 *   - loops with conditionals
 *   - static helper functions
 *   - stack-allocated arrays (the print_int digit buffer)
 *   - pointer arithmetic (`buf + i + 1`)
 *   - string literals (`"! = "`, `"\n"`)
 *   - integer division and modulo
 *
 * Build via examples/cc/run_factab_c.sh.
 */

extern int console_write(const char *buf, int count);

static void
print_str(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	console_write(s, n);
}

static void
print_int(int n)
{
	char buf[16];
	int i = 15;

	if (n == 0) {
		buf[15] = '0';
		i = 14;
	} else {
		while (n > 0) {
			buf[i] = '0' + (n % 10);
			n = n / 10;
			i = i - 1;
		}
	}
	console_write(buf + i + 1, 15 - i);
}

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
