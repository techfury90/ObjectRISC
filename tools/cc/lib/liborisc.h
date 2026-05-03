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

/* ---- host_io.c — host filesystem access via the hostfsd device --
 *
 * Programs using these MUST follow the OR-hygiene contract:
 *   O10 = hostfsd service ref  (set by --service in the runner)
 *   O11 = boot stack ref       (program parks o2 here at startup)
 *   O14 = boot self-svc        (program parks o4 here at startup)
 *   O15 = boot data ref        (program parks o3 here at startup)
 *
 * Each hf_* call SENDs to hostfsd and blocks on the local receive
 * queue for one response. hf_init must be called once before the
 * other hf_* functions. */

#define HF_O_RDONLY  0
#define HF_O_WRONLY  1
#define HF_O_RDWR    2
#define HF_O_CREAT   4
#define HF_O_TRUNC   8

int hf_init(void);
int hf_open(const char *path, int flags);
int hf_close(int fd);
int hf_read(int fd, char *buf, int count);          /* buf MUST be on the stack */
int hf_write(int fd, const char *buf, int count);   /* buf may be stack or data */

#endif /* LIBORISC_H */
