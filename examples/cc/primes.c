/*
 * primes.c — list every prime under 50, then count them.
 *
 * Exercises trial-division primality, bitwise AND, longer loops,
 * conditional return, and string-then-int output.
 */

extern void print_str(const char *s);
extern void print_int(int n);

static int
is_prime(int n)
{
	int d;

	if (n < 2)         return 0;
	if (n == 2)        return 1;
	if ((n & 1) == 0)  return 0;
	for (d = 3; d * d <= n; d = d + 2) {
		if (n % d == 0) return 0;
	}
	return 1;
}

int
main(void)
{
	int n;
	int count = 0;

	print_str("primes < 50:");
	for (n = 2; n < 50; n++) {
		if (is_prime(n)) {
			print_str(" ");
			print_int(n);
			count = count + 1;
		}
	}
	print_str("\ncount = ");
	print_int(count);
	print_str("\n");
	return count;
}
