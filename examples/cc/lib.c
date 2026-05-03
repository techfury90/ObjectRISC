/*
 * lib.c — minimal Object RISC C library helpers.
 *
 * Compiled separately and linked alongside each example program by
 * run_c.sh. Provides the utilities every demo needs without each one
 * re-implementing them. The pcc port has no real libc yet; this file
 * is the seed of one.
 */

extern int console_write(const char *buf, int count);

void
print_str(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	console_write(s, n);
}

void
print_int(int n)
{
	char buf[16];
	int i = 15;
	int neg = 0;

	if (n < 0) { neg = 1; n = -n; }
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
	if (neg) {
		buf[i] = '-';
		i = i - 1;
	}
	console_write(buf + i + 1, 15 - i);
}
