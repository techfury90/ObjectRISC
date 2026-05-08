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

/* SUP_TARGET_LOCAL — the sentinel target_pid value passed to
 * sup_spawn_at meaning "spawn on whatever CPU my supervisor is
 * on" (the same behaviour plain sup_spawn gives). Distinct from
 * any literal PROCID (PROCIDs are 0..254). */
#define SUP_TARGET_LOCAL 0xFF

/* sup_spawn_at — Phase 45e: like sup_spawn, but with explicit
 * target-CPU placement. target_pid is either SUP_TARGET_LOCAL
 * (delegates to the caller's local supervisor; identical to
 * sup_spawn) or a literal PROCID. The local supervisor relays
 * to its peer (PEER_SUP_SLOT) when target_pid != self.procid;
 * the peer spawns locally and replies directly to the caller's
 * reply mailbox. The returned task_t carries a ref whose home is
 * the spawning CPU — task_wait/task_query/task_kill on it route
 * correctly via the Phase 45d remote Task primitives. */
task_t sup_spawn_at(int target_pid, const char *path,
                    const char *args, const char *cwd);

/* sup_shutdown — fire-and-forget op=2 SEND telling the supervisor
 * "I'm about to TaskExit." The supervisor exits its main loop and
 * TaskExits in turn, tearing down the CPU. No-op when the program
 * wasn't launched under a supervisor. Call this RIGHT before
 * returning from main() — once you've TaskExited it's too late. */
void sup_shutdown(void);

/* ----- Directory service (Phase 45f) ----------------------------------
 *
 * `oriscdir` is a small daemon that holds a hierarchical name → ref
 * tree. Programs can:
 *   - register a leaf (a name → OR ref binding)
 *   - mount a service (a name → service ref + path-prefix
 *     binding; walks descending past the mount return early with
 *     the service ref + the remaining path, so e.g. `/programs/`
 *     can route to a hostfsd jail at "/build/programs")
 *   - walk a path to resolve it
 *   - list a directory's children
 *
 * The directory's mailbox is bootstrapped via the boot ABI: oriscrun
 * synthesizes a sub-cap of oriscdir's primary mailbox into each
 * CPU's boot O8, which task_init harvests into BOOT_PARENT_SLOT.
 * Supervisors (whose parent IS the directory) copy this directly
 * into DIR_SLOT at boot. Other programs (shells, etc.) get their
 * directory ref by querying their parent supervisor lazily on the
 * first dir_walk call (see dir.c's dir_init).
 *
 * Node kinds returned by dir_walk: */
#define DIR_KIND_NOT_FOUND  0
#define DIR_KIND_DIR        1
#define DIR_KIND_LEAF       2
#define DIR_KIND_MOUNT      3

/* Walk `path` against the directory tree.
 *
 * On success returns the remainder length (>=0): 0 for DIR/LEAF,
 *   >0 for MOUNT with a non-empty remainder.
 * On error returns negative: -1 EINVAL, -2 ENOENT, -3 EEXISTS,
 *   -4 ENOTDIR, -5 ETOOBIG, -6 EIO.
 *
 * In all success cases *kind_out is filled with DIR_KIND_DIR /
 * LEAF / MOUNT. For LEAF and MOUNT, O1 holds the resolved ref —
 * caller MUST `omov` it out before any other libc call clobbers
 * it. (For DIR, O1 is left null.) For MOUNT, remainder_buf is
 * filled with the prefix-and-leftover path bytes (NUL-terminated
 * within remainder_cap). For other kinds remainder_buf is left
 * untouched. */
int dir_walk(const char *path, int *kind_out,
             char *remainder_buf, int remainder_cap);

/* Register the ref currently in O1 as a leaf at `path`. Fails with
 * EEXISTS if the path already has a non-empty node. */
int dir_register(const char *path);

/* Mount: register the service ref currently in O1 at `path`, with
 * `prefix` as the root within the mounted service (passed back to
 * walk callers as part of their remainder). */
int dir_mount(const char *path, const char *prefix);

/* List children of `path` (must be a DIR — for MOUNTs the caller
 * resolves via dir_walk and talks to the mount's service directly).
 * Children are written into `buf` as NUL-separated UTF-8 names,
 * directories/mounts get a trailing '/'. Returns the entry count
 * on success (the byte length is recoverable as `strlen(buf) +
 * trailing-NUL-terminator runs`), negative on error. */
int dir_list(const char *path, char *buf, int cap);

#endif /* LIBORISC_H */
