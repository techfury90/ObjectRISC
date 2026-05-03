/*
 * io.c — Object RISC libc: console I/O.
 *
 * All output goes through `console_write`, which the small bridge in
 * tools/cc/arch/orisc/console_io.s lowers to firmware ConsoleWrite.
 * The bridge picks the right object reference based on the buffer's
 * VA range — fine for stack and data, not for OR-typed buffers; for
 * those, call orisc_console_write directly with an `__or` source.
 */

#include "liborisc.h"

extern int console_write(const char *buf, int count);

void
print_str(const char *s)
{
	int n = 0;
	while (s[n]) n++;
	console_write(s, n);
}

void
print_char(char c)
{
	char buf[1];
	buf[0] = c;
	console_write(buf, 1);
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

void
print_hex(unsigned int n)
{
	/* Always 8 hex digits, "0x" prefixed. Uniform width keeps
	 * register / address dumps aligned, which is what hex is for. */
	char buf[10];
	int i;
	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++) {
		int nibble = (n >> ((7 - i) * 4)) & 0xF;
		buf[2 + i] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
	}
	console_write(buf, 10);
}
