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
/* Phase 50: filesystem mutation. Both return 0 on success or a
 * negative errno: -3 = ENOENT, -4 = EACCES, -7 = EEXIST (mkdir on
 * existing entry; unlink on directory). hf_mkdir doesn't create
 * intermediate parent directories — that's POSIX `mkdir`, not
 * `mkdir -p`; if you need the latter, walk the path and call us
 * once per component. */
int hf_mkdir(const char *path);
int hf_unlink(const char *path);

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

/* term_clear — wipe the text pane. Phase 48: login.orx fires this
 * before each welcome banner so the new session starts on a blank
 * canvas. Implemented by sending a single 0x0C (form feed) byte
 * through the normal console-write path — oriscterm and
 * fake_terminal both interpret it as a clear directive (same
 * shape as `\b` for backspace). Pair with grid_clear() to wipe
 * the canvas pane too. */
void term_clear(void);

/* term_resubscribe — re-attach a previously term_shutdown'd
 * keyboard subscription, reusing the existing mailbox in O9.
 * Phase 48: login.orx pairs term_shutdown / sup_spawn(shell) /
 * task_wait / term_resubscribe so the shell takes keyboard focus
 * during its session and login takes it back when the shell
 * exits. Avoids the boot-OR-resave hazard of calling term_init
 * a second time. */
void term_resubscribe(void);

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

/* ---- menu.c — reusable modal pop-up menu ------------------------
 *
 * menu_run draws a vertical list of `n` items at grid cell (col,
 * row), then blocks until the user picks one or cancels.  Mirrors
 * the WM desktop root menu's model: mouse motion highlights, left-
 * click selects, click-outside or Esc cancels; arrow keys + Enter
 * also work.  The selected row is marked "> " (grid text carries no
 * color, so no inverse-video bar).
 *
 * `items` is a flat buffer of n NUL-terminated strings end to end,
 * e.g.  static const char items[] = "Red\0Green\0Blue\0Quit";
 * (the array-of-pointers shape trips a pcc-orisc codegen bug — see
 * menu.c).
 *
 * Returns the chosen index [0, n), or -1 on cancel.  Does not
 * restore the cells it drew over — repaint after it returns.
 *
 * Needs term_init (keyboard + grid).  For mouse control also do
 * pointer_init_from_dir_result + pointer_subscribe; without a
 * subscription the menu is keyboard-only. */
int  menu_run(int col, int row, const char *items, int n);

/* ---- vec.c — WM-mediated vector graphics client (Phase 59 / WM γ.11) ----
 *
 * Pixel-space line / rect / oval rasterisation through the window
 * manager.  Wire op + two packed (x<<16)|y / (w<<16)|h words; the
 * WM's per-window VECTOR queue dispatches each op into the
 * framebuffer at native 1280×768 (FB_W × FB_H — see oriscwm.c).
 *
 * Boot prerequisites:
 *   1. task_init() has run.
 *   2. wm_init() returned 0.
 *   3. wm_new_window(WIN_TYPE_CONSOLE) succeeded.
 *   4. wm_bind_surface(wid, WSURF_VECTOR) succeeded — the resolved
 *      cap lands in DIR_RESULT_SLOT (offset 616) and the caller
 *      MUST adopt it before the first vec_*() call.
 *      vec_init_from_dir_result() is the convenience helper that
 *      adopts it into the libc VECTOR handle (obj.h).
 *
 * All vec_*() return 0 on success, -1 if no surface is bound
 * (the VECTOR handle is still null).  Coordinates are signed 16-bit (clamped /
 * clipped by the WM); colors are palette indices 0..8 (matching
 * oriscterm's VEC_PALETTE).
 *
 * `vec_clear` is currently a no-op on the WM side (pending a
 * per-window backing store; see oriscwm.c).  The libc still emits
 * the SEND so future WM versions don't need a client recompile. */

/* Vector op codes (must match oriscterm's VEC_* and oriscwm's
 * forward_vector_write dispatch). */
#define VEC_OP_LINE         0x00
#define VEC_OP_RECT_FILL    0x01
#define VEC_OP_RECT_OUTLINE 0x02
#define VEC_OP_OVAL_FILL    0x03
#define VEC_OP_OVAL_OUTLINE 0x04
#define VEC_OP_CLEAR        0x05
#define VEC_OP_SET_COLOR    0x06

int  vec_init_from_dir_result(void);                 /* adopt DIR_RESULT_SLOT cap into the VECTOR handle */
int  vec_line(int x1, int y1, int x2, int y2);
int  vec_rect_fill(int x, int y, int w, int h);
int  vec_rect_outline(int x, int y, int w, int h);
int  vec_oval_fill(int x, int y, int w, int h);
int  vec_oval_outline(int x, int y, int w, int h);
int  vec_clear(void);
int  vec_set_color(int palette_idx);

/* ---- raster.c — WM-mediated raster blit (Phase 59 / WM γ.12) ----
 *
 * Blit a w×h palette-indexed pixel buffer into the framebuffer at
 * (x, y).  Pixels are 1 byte each, row-major, packed (w bytes per
 * row, no padding).  The WM ObjFetchBytes one row at a time from
 * the caller's source ref into a WM-side scratch buffer, then
 * ObjStoreBytes the row into the framebuffer.  2 wire RTTs per
 * row — for h rows total = 2h RTTs.
 *
 * Boot prerequisites mirror vec.c: task_init, wm_init,
 * wm_new_window, wm_bind_surface(WSURF_RASTER), then
 * raster_init_from_dir_result() to seed WM_RASTER_CAP_SLOT.
 *
 * Wire SEND payload:
 *   O1 = raster cap (read from WM_RASTER_CAP_SLOT)
 *   O2 = source pixel buffer ref (caller chooses; raster_blit picks
 *        boot-stack vs boot-data based on the buffer's VA)
 *   R4 = RST_OP_*
 *   R5 = (x << 16) | y
 *   R6 = (w << 16) | h
 *   R7 = byte offset within source where pixel data starts
 *
 * Returns 0 on success, -1 if WM_RASTER_CAP_SLOT is null. */

#define RST_OP_BLIT   0x00
#define RST_OP_CLEAR  0x01

/* Pack two 16-bit halves into one 32-bit word, matching the WM's
 * wire format for (x, y) and (w, h) pairs.  Used at raster_blit
 * call sites because pcc-orisc passes only 4 args in registers and
 * trips on 5-arg function calls — rolling x/y and w/h into single
 * ints keeps the call shape compatible. */
#define WM_PACK_XY(x, y) (((((int)(x)) & 0xFFFF) << 16) | (((int)(y)) & 0xFFFF))
#define WM_PACK_WH(w, h) (((((int)(w)) & 0xFFFF) << 16) | (((int)(h)) & 0xFFFF))

int  raster_init_from_dir_result(void);
int  raster_blit(int packed_xy, int packed_wh, const unsigned char *pixels);
int  raster_clear(void);

/* ---- pointer.c — WM-mediated pointer events (Phase 59 / WM γ.13) ---
 *
 * The WM subscribes to /sys/term/0/pointer at boot and forwards
 * events to a single client subscriber slot.  Clients
 * wm_bind_surface(WSURF_POINTER), copy DIR_RESULT_SLOT into
 * WM_POINTER_CAP_SLOT, then SEND through it once with O2 = a reply
 * ref where they want events delivered.  v1 supports a single
 * subscriber per WM; multi-subscriber + per-window focus routing are
 * post-multi-window work.
 *
 * Event payload (the WM's onward SEND to the subscriber):
 *   R4 = PTR_EVT_* (motion / down / up)
 *   R5 = packed (x << 16) | y    — canvas-space coordinates
 *   R6 = button (PTR_BTN_* — only meaningful for DOWN / UP)
 *   R7 = button-state mask (1 << button per pressed button)
 *
 * Subscribers typically allocate a TAG_SERVICE mailbox + queue,
 * derive an R|W sub-cap, and pass that as the reply ref.  Same
 * pattern term.c uses for keyboard. */

#define PTR_EVT_MOTION  0x00
#define PTR_EVT_DOWN    0x01
#define PTR_EVT_UP      0x02

#define PTR_BTN_LEFT   1
#define PTR_BTN_MIDDLE 2
#define PTR_BTN_RIGHT  3

/* Subscribe / receive flow:
 *   1. wm_init + wm_new_window + wm_bind_surface(wid, WSURF_POINTER).
 *   2. pointer_init_from_dir_result() — DIR_RESULT_SLOT →
 *      WM_POINTER_CAP_SLOT.
 *   3. pointer_subscribe() — allocate a TAG_SERVICE mailbox into
 *      O10, ReceiveQueueAttach (depth 16), derive an R|S sub-cap,
 *      and SEND it to the WM-mediated pointer cap.
 *   4. pointer_getevent(&evt_type, &packed_xy, &button, &btn_state)
 *      polls the O10 mailbox with timeout=0; returns 0 if an event
 *      was delivered, -1 otherwise.
 * Call pointer_unsubscribe() to clear the WM's subscriber slot
 * before exit. */
int  pointer_init_from_dir_result(void);
int  pointer_subscribe(void);
int  pointer_unsubscribe(void);
int  pointer_getevent(int *evt_type, int *packed_xy,
                       int *button, int *btn_state);
/* pointer_subscribed: 1 if pointer_subscribe has run and the event
 * mailbox is live, 0 otherwise. Lets helpers (menu_run) decide
 * whether the mouse is available before polling it. */
int  pointer_subscribed(void);

/* term_getkey: blocks until the next keystroke arrives. Returns
 * the codepoint (ASCII for printable, ≥0x100 for special — see
 * KEY_* in tools/devices/oriscterm). Modifier mask written to
 * *out_mods (NULL OK to ignore). */
int  term_getkey(int *out_mods);

/* term_pollkey: non-blocking peek. Returns 0 and fills *out_code /
 * *out_mods (either may be NULL) if a keystroke is waiting, -1 if
 * none. For event loops that interleave keyboard with pointer/timer
 * work (mouse_paint, menu_run). term_init must have run. */
int  term_pollkey(int *out_code, int *out_mods);

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
#define TK_HOME      0x184
#define TK_END       0x185
#define TK_PAGE_UP   0x186
#define TK_PAGE_DOWN 0x187
#define TK_F1        0x190    /* F1..F12 = 0x190..0x19B */

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
/* Phase 51: terminal_idx propagation. Each task tracks the index
 * of the terminal the user is sitting at (independent of which CPU
 * the task happens to run on). sup_spawn reads this and packs it
 * into the spawn-request's R7 so the receiving supervisor can dir-
 * walk /sys/term/<N>/* and inject the right console/keyboard/grid
 * into the child's OPRs (Phase 49 pass-through machinery).
 *
 * task_init computes the initial value from `_orisc_init_r4` (set
 * by crt0 from R4-at-entry, which was TaskCreate's init_r4). The
 * encoding is `terminal_idx + 1` with 0 = "no terminal" (-1
 * internally). Top-level boots (the supervisor) come up with
 * my_terminal_idx = -1 and need to set it explicitly with
 * task_set_my_terminal_idx — typically procid at boot. */
int    task_my_terminal_idx(void);
void   task_set_my_terminal_idx(int idx);
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
/* Phase 51: per-spawn override for the child's terminal_idx (the
 * value that ends up in the child's task_my_terminal_idx() at
 * boot). Supervisors set this around an orx_spawn for a relayed
 * pass-through request; everyone else leaves it alone (the default
 * propagates the parent's own terminal_idx, which is the right
 * inheritance for a shell running `run cmd` directly). */
void   orx_set_child_terminal_idx(int idx);
void   orx_clear_child_terminal_idx(void);

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

/* SUP_TARGET_LOCAL — sentinel meaning "stay on the local CPU; do
 * not round-robin." Used when the caller explicitly wants the
 * spawn to happen on the supervisor's own CPU. Phase 51 also uses
 * this on the wire as the relay-pinning marker (a relayed packet
 * arrives at the receiver with target_pid = LOCAL, signalling
 * "spawn here, no further relay").
 *
 * SUP_TARGET_ANY — Phase 51 sentinel meaning "any CPU is fine;
 * round-robin OK." Plain sup_spawn() uses this so a Phase-51-aware
 * supervisor can spread load across CPUs. The receiver dir-walks
 * /sys/term/<requester>/* (carried in R7) and injects the
 * requester's terminal services into the child's OPRs, so
 * term_print/term_getkey route to the right oriscterm regardless
 * of which CPU the spawn lands on.
 *
 * Both values are above the 0..254 PROCID range so they never
 * collide with a literal target. */
#define SUP_TARGET_LOCAL 0xFF
#define SUP_TARGET_ANY   0xFE

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

/* sup_have_supervisor — non-zero iff the BOOT_PARENT_SLOT carries a
 * non-null supervisor sub-cap (i.e. this program was launched via
 * sup_spawn / orx_spawn under a supervisor). Phase 48: shell.exit
 * branches on this to choose between yield-forever (login race
 * mitigation) and a clean return (no-supervisor test path). */
int  sup_have_supervisor(void);

/* sup_list_tasks — Phase 52: cross-CPU `ps`. Sends an op=5
 * SUP_OP_LIST_TASKS to the supervisor whose R+S sub-cap is in O1 at
 * call time (caller OREFLDs from a dir_walk result; see
 * shell.c::cmd_ps for the pattern). The supervisor replies with a
 * human-readable text listing of its libc task table, one task per
 * line ("[N] STATE NAME [ (exit C)]\n"). We MapObject the bytes
 * R-only, copy up to `max` bytes into `dst`, and Unmap.
 *
 * Returns:
 *     >= 0  byte length copied into dst (0 if the supervisor's task
 *           table is empty)
 *     < 0   error: -1 = recipient ref in O1 was null, other negative
 *                  values mirror firmware status codes
 *
 * The caller addresses a SPECIFIC supervisor (no relay) — for `ps`
 * we walk /sys/cpu/<N>/supervisor for each candidate procid and
 * SEND directly to each one's mailbox. */
int  sup_list_tasks(char *dst, int max);

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

/* dir_subscribe — Phase 54: ask oriscdir to SEND to the notify_cap
 * currently in O1 whenever the tree mutates at or under `path`. The
 * notify_op (1..255) lands in R3 of every notification; pick a value
 * distinct from your other dispatch ops so your poll loop can route.
 *
 * Caller MUST OREFLD the notify_cap into O1 immediately before
 * calling — same convention as dir_register's ref-to-register. The
 * notify_cap is typically a R+S sub-cap of the same mailbox the
 * caller's main poll already reads, so notifications interleave
 * naturally with regular requests.
 *
 * Returns 0 on success, negative on error. */
int dir_subscribe(const char *path, int notify_op);

/* ----- Window-manager client (oriscwm) — milestone 3 -----------------
 *
 * libc wrappers for the oriscwm wire protocol.  See
 * ouroboros/oriscwm.c for the full op writeup; tools/cc/lib/wm.c
 * implements the SEND-and-poll round-trips against the per-program
 * reply mailbox (shared with sup.c / dir.c).
 *
 * Boot prerequisites: task_init() has run, DIR_SLOT is reachable
 * (either pre-populated by a parent — supervisors propagate this
 * through orx_spawn — or BOOT_PARENT_SLOT is wired so dir.c's
 * lazy bootstrap can populate it).
 *
 * Window types (must match ouroboros/oriscwm.c). */
#define WIN_TYPE_CONSOLE    1
#define WIN_TYPE_GRAPHICAL  2

/* Surface kinds (mirror oriscterm's service indices). */
#define WSURF_CONSOLE   1
#define WSURF_KEYBOARD  2
#define WSURF_GRID      3
#define WSURF_VECTOR    4
#define WSURF_RASTER    5
#define WSURF_POINTER   6

/* Error codes from wm_*.  Negative-status convention. */
#define WIN_E_INVAL    (-1)
#define WIN_E_NOENT    (-2)     /* window not found, or no oriscwm running */
#define WIN_E_IO       (-6)     /* internal / no oriscdir to bootstrap */
#define WIN_E_NOSPC    (-7)
#define WIN_E_NOTIMPL  (-8)

/* wm_init — lazy: adopt the WM service (via dir_walk on "/sys/wm/0")
 * into wm.c's obj.h handle.  Returns 0 OK, -6 if no directory available, -2 if
 * /sys/wm/0 doesn't resolve (no oriscwm in this system).  Idempotent;
 * subsequent calls fast-return.
 *
 * Programs that want the "use-WM-if-present, fall-back-otherwise"
 * pattern call wm_init and check the status: 0 means proceed with
 * wm_*, anything else means use direct boot-OPR surfaces. */
int wm_init(void);

/* wm_new_window — request a window of `type`.
 *
 * Caller MUST place the owner-task ref in O1 before calling.  The
 * WM stashes it for task_query polling: when the owner task
 * EXITed, the WM auto-destroys the window.  Pass O1 = null to opt
 * out of auto-destroy.
 *
 * On success: *out_wid is set to the window id (1..MAX_WINDOWS),
 * and *out_w_cells / *out_h_cells are filled with the cell-grid
 * dimensions (pixel dimensions aren't surfaced in this API yet —
 * caller can add an API extension if/when graphical clients need
 * them).  Pass NULL out-pointers to skip. */
int wm_new_window(int type, int *out_wid,
                  int *out_w_cells, int *out_h_cells);

/* wm_bind_surface — resolve surface `kind` of window `wid`.
 *
 * On success the resolved surface cap is parked in DIR_RESULT_SLOT
 * (offset 616 in O12 — the same generic last-resolved-ref scratch
 * dir_walk uses).  Caller follows up with `orefld oN, 616(o12)`
 * inline asm to land it in the desired OPR. */
int wm_bind_surface(int wid, int kind);

/* wm_destroy_window — release a window. */
int wm_destroy_window(int wid);

/* wm_open_session — allocate a fresh WM-mediated window for the
 * caller, replacing the inherited parent-window CONSOLE / KEYBOARD /
 * GRID caps (O5 / O6 / O7) with caps for the new window.  After this
 * returns, term_print / term_read / grid_write target the new
 * window rather than the parent's; print_str / firmware ConsoleWrite
 * (which writes to the simorisc process's stdout) is unaffected.
 *
 * `title` may be NULL to skip setting a title bar string.  On
 * success *out_wid (if non-NULL) holds the new window id; the caller
 * should wm_destroy_window(wid) before returning from main so the
 * WM doesn't leak the window object.  Returns negative on any of
 * wm_init / wm_new_window / wm_bind_surface failing.
 *
 * Phase 60 step 12. */
int wm_open_session(const char *title, int *out_wid);

/* wm_set_title — set the text displayed in `wid`'s title bar.
 * The string is copied into a stack-local buffer before SEND, so
 * callers can pass any nul-terminated source.  Pass `wid = 0` to
 * target the first live window.  Returns 0 on success; negative
 * values mean the WM rejected the request (invalid wid / no such
 * window / fetch failed). */
int wm_set_title(int wid, const char *title);

/* wm_subscribe_events — register a notify cap for window-lifecycle
 * events (resize, focus, close-request).  Stub on the WM side
 * today — accepted and stored, no events fire yet — but the wire
 * shape is committed.
 *
 * Caller MUST OREFLD the notify cap into O1 immediately before
 * calling (same convention as dir_subscribe).  notify_op is the
 * value (1..255) the WM puts in R3 of every notification SEND so
 * the subscriber can multiplex multiple subscriptions on a single
 * mailbox. */
int wm_subscribe_events(int wid, int notify_op);

/* wm_get_geometry — read back the cell-grid + pixel extents of a
 * window.  Pass `wid = 0` to query the first live window; useful for
 * leader-spawned children that inherited a CONSOLE/GRID cap from the
 * supervisor and need to know the cell dims of the surface they're
 * rendering into without having called wm_new_window themselves.
 *
 * `out` MUST point to a 4-int array; on success it's filled with
 * the *usable* (inside-the-border) extents:
 *     out[0] = w_cells,  out[1] = h_cells,
 *     out[2] = w_px,     out[3] = h_px
 * The 4-array form avoids the pcc-orisc 5-arg-call codegen bug; for
 * a more conventional API a wrapper macro / inline can pull each
 * field out into named locals at the call site.
 *
 * Returns 0 on success; WM_NO_* if the WM is unreachable; -1/-2 if
 * the wid is invalid or no live window exists. */
int wm_get_geometry(int wid, int *out);

/* Convenience indices into the wm_get_geometry out[] array. */
#define WM_GEOM_W_CELLS  0
#define WM_GEOM_H_CELLS  1
#define WM_GEOM_W_PX     2
#define WM_GEOM_H_PX     3

/* ----- VFS helpers (Phase 45g) ----------------------------------------
 *
 * `vfs.c` is the path-aware front door programs should prefer over
 * the bare `hf_*` calls. Each operation walks the directory tree
 * (via `dir_walk`) to translate the user-visible path into the
 * remainder a backend service understands, then dispatches to the
 * underlying service. For Phase 45g the only backend is hostfsd —
 * `vfs_open` / `vfs_opendir` / `vfs_close` / `vfs_read` / `vfs_write`
 * forward to the corresponding `hf_*` against the program's boot
 * O10 (which `hf_init` has already subscribed). Multi-backend
 * dispatch (using the resolved service ref directly as the SEND
 * recipient) is deferred to a later phase.
 *
 * Required pre-conditions for callers:
 *   - `task_init()` has been called (so DIR_RESULT_SLOT etc. exist)
 *   - `hf_init()` has been called (so O10 + reply mailbox are ready)
 *   - DIR_SLOT is populated (supervisor populates from BOOT_PARENT_SLOT
 *     at boot; other programs lazily SEND op=4 SUP_OP_GET_DIR to their
 *     parent supervisor on the first dir_*() call)
 *
 * Path semantics:
 *   - vfs_open / vfs_opendir succeed only on MOUNT-resolved paths.
 *     A pure DIR (e.g. `/sys/cpu/0`) has no underlying file backend,
 *     so opening it returns -1; use `vfs_list` to enumerate children.
 *   - LEAF paths (services registered via `dir_register`, e.g.
 *     `/sys/cpu/0/supervisor`) are not file-readable; open returns -1.
 *   - vfs_list works on BOTH DIR and MOUNT — for DIR it dispatches to
 *     `dir_list`, for MOUNT it streams the backend's `hf_opendir`
 *     output into `buf` until short-read or capacity exhaustion. */
int vfs_walk_kind(const char *path, int *kind_out);
int vfs_open(const char *path, int flags);
int vfs_opendir(const char *path);
int vfs_close(int fd);
int vfs_read(int fd, char *buf, int count);
int vfs_write(int fd, const char *buf, int count);
int vfs_list(const char *path, char *buf, int cap);
/* Phase 50: filesystem mutation through the VFS. DIR-resolved paths
 * (the /sys subtree owned by oriscdir) reject — those entries belong
 * to self-registering services. MOUNT-resolved paths and the no-
 * directory fallback both end at hf_mkdir / hf_unlink. */
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);

#endif /* LIBORISC_H */
