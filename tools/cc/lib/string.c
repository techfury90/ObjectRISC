/*
 * string.c — Object RISC libc: string and memory primitives.
 *
 * Plain C, no architectural tricks. These are the textbook
 * one-liner implementations; if performance ever matters, hand-tuned
 * asm versions can replace them in the archive without touching
 * callers (the linker pulls in whichever .oro defines the symbol).
 */

#include "liborisc.h"

unsigned int
strlen(const char *s)
{
	const char *p = s;
	while (*p) p++;
	return (unsigned int)(p - s);
}

int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *
strcpy(char *dst, const char *src)
{
	char *out = dst;
	while ((*dst++ = *src++) != 0) ;
	return out;
}

void *
memcpy(void *dst, const void *src, unsigned int n)
{
	char *d = (char *)dst;
	const char *s = (const char *)src;
	while (n--) *d++ = *s++;
	return dst;
}

void *
memset(void *dst, int c, unsigned int n)
{
	char *d = (char *)dst;
	while (n--) *d++ = (char)c;
	return dst;
}

int
memcmp(const void *a, const void *b, unsigned int n)
{
	const unsigned char *x = (const unsigned char *)a;
	const unsigned char *y = (const unsigned char *)b;
	while (n--) {
		if (*x != *y) return (int)*x - (int)*y;
		x++; y++;
	}
	return 0;
}

int
atoi(const char *s)
{
	int n = 0;
	int neg = 0;
	while (*s == ' ' || *s == '\t' || *s == '\n') s++;
	if (*s == '-') { neg = 1; s++; }
	else if (*s == '+') { s++; }
	while (*s >= '0' && *s <= '9') {
		n = n * 10 + (*s - '0');
		s++;
	}
	return neg ? -n : n;
}
