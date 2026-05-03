/*
 * fizzbuzz.c — the canonical interview question.
 *
 * Exercises if/else-if chains, modulo, mixed string/integer output.
 */

extern void print_str(const char *s);
extern void print_int(int n);

int
main(void)
{
	int i;

	for (i = 1; i <= 20; i++) {
		if (i % 15 == 0)      print_str("FizzBuzz");
		else if (i % 3 == 0)  print_str("Fizz");
		else if (i % 5 == 0)  print_str("Buzz");
		else                  print_int(i);
		print_str("\n");
	}
	return 0;
}
