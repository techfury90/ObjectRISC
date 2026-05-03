/*
 * liborisc.h — Object RISC libc declarations.
 *
 * Prototypes for the functions provided by liborisc.ora. Pull in
 * with `#include "liborisc.h"`; the linker resolves each call by
 * pulling the relevant member out of the archive.
 *
 * For the OR-file macros (oref_eq, oref_isnull, OL/OS through `__or`
 * pointers, etc.) see the separate `orisc.h` in
 * tools/cc/arch/orisc/. They're orthogonal — most programs want both.
 */

#ifndef LIBORISC_H
#define LIBORISC_H

/* ---- io.c — console output ------------------------------------- */

void print_str(const char *s);
void print_char(char c);
void print_int(int n);
void print_hex(unsigned int n);

/* ---- string.c — string and memory primitives ------------------- */

unsigned int strlen(const char *s);
int          strcmp(const char *a, const char *b);
char        *strcpy(char *dst, const char *src);
void        *memcpy(void *dst, const void *src, unsigned int n);
void        *memset(void *dst, int c, unsigned int n);
int          memcmp(const void *a, const void *b, unsigned int n);
int          atoi(const char *s);

#endif /* LIBORISC_H */
