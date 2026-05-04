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
int hf_opendir(const char *path);                   /* read returns "name\nname\n..." */
int hf_close(int fd);
int hf_read(int fd, char *buf, int count);          /* buf MUST be on the stack */
int hf_write(int fd, const char *buf, int count);   /* buf may be stack or data */

/* ---- term.c — oriscterm interaction (console + keyboard) ------- *
 *
 * Wraps the wire protocols for the console (idx 1) and keyboard
 * (idx 2) services. Programs MUST follow the OR-hygiene contract:
 *
 *     O5  = oriscterm console  (--service order)
 *     O6  = oriscterm keyboard (--service order)
 *     O11 = boot stack ref     (parked by term_init)
 *     O14 = boot self-svc      (parked by term_init)
 *     O15 = boot data ref      (parked by term_init)
 *
 * Call term_init() once at program start; after that, term_print*
 * writes to the Tk terminal window (NOT host stdout — for that
 * keep using the print_* family from io.c) and term_getkey blocks
 * on the keyboard queue. */

void term_init(void);
void term_print(const char *s);
void term_print_n(const char *buf, int count);   /* explicit length */
void term_print_char(char c);
void term_print_int(int n);
void term_print_hex(unsigned int n);

/* term_getkey: blocks until the next keystroke arrives. Returns
 * the codepoint (ASCII for printable, ≥0x100 for special — see
 * KEY_* in tools/devices/oriscterm). Modifier mask written to
 * *out_mods (NULL OK to ignore). */
int  term_getkey(int *out_mods);

/* Special-key codepoints (mirrored from oriscterm). */
#define TK_BACKSPACE 0x108
#define TK_TAB       0x109
#define TK_RETURN    0x10D
#define TK_ESCAPE    0x11B
#define TK_DELETE    0x17F
#define TK_UP        0x180
#define TK_DOWN      0x181
#define TK_LEFT      0x182
#define TK_RIGHT     0x183

/* Modifier mask bits (mirrored from oriscterm). */
#define TK_MOD_SHIFT 0x01
#define TK_MOD_CTRL  0x02
#define TK_MOD_ALT   0x04
#define TK_MOD_META  0x08

/* ---- linkboot.c — spawn programs via linkbootd ----------------- *
 *
 * Boot ABI for shells using these helpers (caller arranges via
 * --service order):
 *
 *     O7  = linkbootd service ref  (--service N=1@9 in slot 3)
 *     O11 = boot stack ref         (parked by term_init)
 *     O14 = boot self-svc          (parked by term_init)
 *     O15 = boot data ref          (parked by term_init)
 *
 * lb_init() must be called once at startup, after term_init. It
 * allocates an internal mailbox object and attaches a queue, used
 * to receive spawn-result messages without conflicting with the
 * keyboard/hostfsd queue. */

int lb_init(void);

/* Ask linkbootd to load and run `path`. Blocks until the guest
 * exits (or fails to load). Returns the guest's exit code on
 * success, 255 on a load failure, or -1 on a poll failure. */
int lb_spawn(const char *path);

#endif /* LIBORISC_H */
