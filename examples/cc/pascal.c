/*
 * pascal.c — first 10 rows of Pascal's triangle.
 *
 * Computes each row in place by walking the previous row from
 * right to left, summing each pair into row[i]. Exercises
 * stack-resident arrays of int with computed indices.
 */

extern void print_str(const char *s);
extern void print_int(int n);

int
main(void)
{
	int row[16];
	int n, i;

	row[0] = 1;
	for (n = 0; n < 10; n++) {
		/* Print the current row. */
		for (i = 0; i <= n; i++) {
			print_int(row[i]);
			print_str(" ");
		}
		print_str("\n");

		/* Compute next row in place: walk right-to-left so each
		 * sum reads the un-updated neighbor. */
		row[n + 1] = 1;
		for (i = n; i > 0; i = i - 1) {
			row[i] = row[i] + row[i - 1];
		}
	}
	return 0;
}
