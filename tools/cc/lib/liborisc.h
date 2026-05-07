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

/* ---- argv.c — program arguments + cwd handed in by the launcher ----- *
 *
 * `program_args()` returns a pointer to a NUL-terminated string with
 * everything the shell typed after the program path. Programs that
 * want individual args split it themselves.
 *
 * `program_cwd()` returns the launcher's working directory at the
 * moment of spawn. Programs that take user-typed paths should
 * prepend cwd to relative arguments — the libc has no per-task cwd
 * concept, so hostfsd resolves every path against its jail root and
 * "edit hello.c" from a non-root cwd would otherwise miss.
 *
 * Both return "" if the launcher passed nothing for that field
 * (orx_run always maps a buffer with both fields, even when empty). */

const char *program_args(void);
const char *program_cwd(void);

/* ---- io.c — console output ------------------------------------- */

void print_str(const char *s);
void print_char(char c);
void print_int(int n);
void print_hex(unsigned int n);

/* ---- clock.c — system clock primitives (Vol VI §8) ------------ */

unsigned int read_cycles(void);        /* CPU cycle counter (#0x301) */
unsigned int time_now_us(void);        /* μs since boot, low 32 bits
                                        * (#0x400 — high 32 bits live
                                        * in the side-channel which
                                        * isn't implemented yet) */
unsigned int clock_resolution(void);   /* ticks/sec of time_now_us
                                        * (#0x410); always 1_000_000 */

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
 *     O9  = term mailbox       (allocated by term_init; receives
 *                                key events the terminal SENDs; do
 *                                NOT clobber after init — term_getkey
 *                                polls it)
 *     O11 = boot stack ref     (parked by term_init)
 *     O14 = boot self-svc      (parked by term_init)
 *     O15 = boot data ref      (parked by term_init)
 *
 * Call term_init() once at program start; after that, term_print*
 * writes to the Tk terminal window (NOT host stdout — for that
 * keep using the print_* family from io.c) and term_getkey blocks
 * on the keyboard queue. */

void term_init(void);
void term_print_only_init(void);                   /* parks boot ORs, no kbd subscribe */
void term_shutdown(void);                          /* unsubscribe from kbd before exit */
void term_print(const char *s);
void term_print_n(const char *buf, int count);     /* explicit length, async */
void term_print_n_sync(const char *buf, int count);/* sync — blocks until the
                                                    * receiver acks; safe to
                                                    * reuse the buffer after
                                                    * return. See term.c. */
void term_print_char(char c);
void term_print_int(int n);
void term_print_hex(unsigned int n);

/* ---- grid.c — oriscterm grid (idx 3) client ----------------------
 *
 * Paint byte sequences at character-cell positions (col, row) on
 * the graphics canvas, and clear the whole canvas. The grid
 * service carries both paint (col/row >= 0) and clear (col=row=-1
 * sentinel) on a single ref — handy because hf_init lays claim to
 * O8 at runtime, leaving a separate vector slot crowded out.
 *
 *     O7  = oriscterm grid     (--service N=3@9 in slot 3)
 *
 * term_init or term_print_only_init MUST run first — they park
 * the boot OPRs grid.c reuses. */

void grid_print(int col, int row, const char *s);
void grid_print_n(int col, int row, const char *buf, int count);
void grid_clear(void);

/* term_getkey: blocks until the next keystroke arrives. Returns
 * the codepoint (ASCII for printable, ≥0x100 for special — see
 * KEY_* in tools/devices/oriscterm). Modifier mask written to
 * *out_mods (NULL OK to ignore). */
int  term_getkey(int *out_mods);

/* Special-key codepoints (mirrored from oriscterm). */
#define TK_BACKSPACE 0x108
#define TK_TAB       0x109
#define TK_RETURN    0x10D
/* Synthetic event the terminal sends to the newly-focused subscriber
 * each time F1 cycles focus — a "you have the keyboard now, repaint
 * if you care" hint. Programs that draw something to the screen and
 * render at the top of their main loop (the editor, future window
 * managers) get a free redraw on focus-in just by ignoring the key
 * code: term_getkey returns, no handler matches, the next iteration's
 * render fires. Mods are always 0. */
#define TK_FOCUS_IN  0x10E
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

/* ---- task.c — task management (Vol VI §4) ----------------------- *
 *
 * Multi-child API: each call takes a `task_t` handle that names a
 * slot in a libc-managed OREF storage table holding up to
 * TASK_MAX_CONCURRENT child task refs at once.
 *
 *     task_t kid_a = task_spawn(child_a, 7);
 *     task_t kid_b = task_spawn(child_b, 11);
 *     int    code_a = task_wait(kid_a);
 *     int    code_b = task_wait(kid_b);
 *     task_free(kid_a);
 *     task_free(kid_b);
 *
 * Boot ABI for task-using programs:
 *
 *     O11 = boot stack ref            (parked by task_init)
 *     O12 = task table (objstore ref) (allocated by task_init)
 *     O13 = parent's boot code ref    (parked by task_init)
 *     O15 = boot data ref             (parked by task_init)
 *
 * task_init() must be called once at program start, BEFORE main
 * clobbers O1. Children inherit the parent's OPRs verbatim, so
 * service refs (O5..O10), boot saves (O11/O15), etc. are all
 * visible to the child without redoing the init dances.
 *
 * Returns:
 *   task_spawn:  >= 0 = slot handle on success;
 *                -1   = table full;
 *                -EERR (negative firmware errno) on primitive failure.
 *   task_wait:   >= 0 = child's exit code (0..255);
 *                -EERR on failure.
 *   task_free:   0    = OK;
 *                -1   = bad handle;
 *                EERR (positive firmware errno) on primitive failure.
 */

#define TASK_MAX_CONCURRENT 16

typedef int task_t;

void   task_init(void);                            /* allocate the task table */
task_t task_spawn(void (*entry)(int), int arg);    /* fork; return slot handle */
int    task_wait(task_t t);                        /* block on t; return exit code */
int    task_kill(task_t t, int code);              /* mark t EXITED with code */
int    task_free(task_t t);                        /* reap exited child */
void   task_yield(void);                           /* surrender quantum */
void   task_exit(int code);                        /* terminate caller (no return) */

/* Lower-level handles for callers (orx.c, future loaders) that drive
 * TaskCreate themselves and only need the libc to manage the table
 * slot + TaskResume for them. */
task_t task_register_o1(void);                     /* OREFST O1 → next free slot */
int    task_resume(task_t t);                      /* OREFLD slot, call TaskResume */

/* Non-blocking inspection (Vol VI #0x008 TaskQuery). state values
 * mirror simorisc's TASK_STATE_*; exit_code is meaningful only when
 * state == TASK_STATE_EXITED. */
#define TASK_STATE_NEW        0
#define TASK_STATE_RUNNABLE   1
#define TASK_STATE_RUNNING    2
#define TASK_STATE_SUSPENDED  3
#define TASK_STATE_BLOCKED    4
#define TASK_STATE_EXITED     5

struct task_info {
	int state;
	int processor;
	int exit_code;
};

int          task_query(task_t t, struct task_info *out);
unsigned int task_active_mask(void);               /* in-use slot bitmap */

/* Install a generic timer-interrupt handler that yields to the
 * next runnable task every `quantum` cycles. Lets the caller stay
 * responsive even when a child task is CPU-bound. Caller must be
 * supervisor-mode (uses InstallTrapHandler #0x520). */
void task_install_preempt_timer(unsigned int quantum);

/* ---- orx.c — load and run a .orx executable as a child task ----- *
 *
 * The supervisor escape from "spawn programs only via linkbootd on a
 * separate CPU." orx_run reads a .orx from the host filesystem,
 * ObjAllocs code/data/stack objects, copies the file's text and data
 * sections into them via temp VA mappings, then TaskCreates a child
 * to run the entry point. Synchronous: blocks until the child exits.
 *
 * Depends on hf_init() having been called. Does NOT depend on
 * task_init() — orx_run manages task creation directly.
 *
 * Returns the guest's exit code (0..255) on success, or one of:
 *     -1  hf_open failed
 *     -2  short header read or bad magic
 *     -3  header validation failure
 *     -4  ObjAlloc / MapObject / read failed during load
 *     -5  TaskCreate / TaskResume / TaskWait failed
 */

int    orx_init(void);                                 /* optional boot-time arg-buffer pre-alloc */
int    orx_run  (const char *path, const char *args, const char *cwd);  /* sync */
task_t orx_spawn(const char *path, const char *args, const char *cwd);  /* async */
int    orx_unload(task_t t);                           /* wait + deferred-free + reap */

/* ---- sup.c — supervisor RPC client ----------------------------- *
 *
 * `sup_spawn` is the supervised cousin of `orx_spawn`: instead of
 * loading the .orx and TaskCreating directly, it SENDs a spawn
 * request to the local supervisor (the program ouroboros/supervisor.c)
 * and waits for the resulting task ref to come back. The supervisor
 * sub-cap lives in O12 + SUP_SLOT, parked there by `task_init` from
 * the boot ABI's O8. Programs that weren't launched by a supervisor
 * get -1 from sup_spawn and should fall back to orx_spawn (or
 * accept that spawning isn't available in their context).
 *
 * Phase 45a — locally-routed only. Phase 45b lifts this to N CPUs
 * via per-CPU supervisors discovered through the wire protocol. */

task_t sup_spawn(const char *path, const char *args, const char *cwd);

#endif /* LIBORISC_H */
