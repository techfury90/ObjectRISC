/*
 * oriscwm.c — Object RISC window manager (.orx version, milestone 2).
 *
 * Replaces the milestone-1 Python prototype at tools/devices/oriscwm
 * with a CPU-side implementation that lives in Object RISC userspace
 * the same way supervisor.c does.  Two structural wins from the
 * translation:
 *
 *   - OP_REGISTER_SURFACE goes away.  The Python daemon needed a
 *     trusted client to push surface caps to it (Python can't
 *     ObjAlloc bytes objects on a CPU, so it can't issue OP_WALK
 *     on oriscdir).  The .orx WM walks /sys/term/0/{console,keyboard}
 *     itself at boot via dir_walk, the same path the supervisor uses
 *     for its own boot caps.
 *
 *   - task_query-based auto-destroy.  The owner-task ref is
 *     captured at OP_NEW_WINDOW time (clients pass it in O2) and
 *     stashed per-window in O12 scratch slots; the dispatch loop's
 *     idle pulse calls scan_owner_exits, which task_queries each
 *     live window's owner and frees the slot when the owner has
 *     EXITed.  Graceful degradation: clients that pass non-task
 *     refs (the smoke test does, since wm_smoke is a single-task
 *     program with no convenient way to reference itself) get
 *     task_query failures and are simply skipped — manual
 *     OP_DESTROY_WINDOW stays the cleanup path for those.
 *
 * The wire protocol is also revised for CPU-friendly dispatch
 * (the milestone-1 per-window-handle services don't fit on a CPU
 * because ReceiveQueueAttach is per-object and ReceiveQueuePoll
 * dequeues from one queue at a time).  All ops now SEND to the
 * single WM service and carry a window_id in R4:
 *
 *   WM_OP_NEW_WINDOW       R4 = 0       R5 = window_type
 *                          Reply: R3=status, R4=geom_a, R5=geom_b,
 *                                 R6=window_id
 *
 *   WM_OP_BIND_SURFACE     R4 = wid     R5 = surface_kind
 *                          Reply: R3=status, O2=surface cap
 *
 *   WM_OP_DESTROY_WINDOW   R4 = wid
 *                          Reply: R3=status
 *
 *   WM_OP_SUBSCRIBE_EVENTS R4 = wid     R5 = notify_op (1..255)
 *                          O4 = notify_cap
 *                          Reply: R3=status
 *
 * Window types:
 *   WIN_CONSOLE   = 1   (console + keyboard)
 *   WIN_GRAPHICAL = 2   (E_NOTIMPL — same as milestone 1; surfaces
 *                        beyond console/keyboard aren't published in
 *                        oriscdir yet, so the WM has nothing to bind)
 *
 * Surface kinds (mirror oriscterm's service indices):
 *   WSURF_CONSOLE  = 1
 *   WSURF_KEYBOARD = 2
 *   WSURF_GRID     = 3   (registered for protocol completeness; the
 *   WSURF_VECTOR   = 4    WM doesn't acquire these in milestone 2 —
 *   WSURF_RASTER   = 5    they aren't published at /sys/term/0/...
 *   WSURF_POINTER  = 6    so OP_BIND_SURFACE returns E_NOENT for them
 *                         on a CONSOLE window, E_NOTIMPL on graphical)
 *
 * Errors (negative R3):
 *   -1  E_INVAL      bad arguments / unknown op / wrong surface for type
 *   -2  E_NOENT      window not found, or surface unregistered
 *   -6  E_IO         internal error (typically wire / dir_walk failure)
 *   -7  E_NOSPC      no more windows allocatable (N=1 hardcoded for
 *                    CONSOLE in milestone 2; future milestones lift it)
 *   -8  E_NOTIMPL    feature not implemented (e.g. GRAPHICAL)
 *
 * Boot ABI inherited from oriscrun / simorisc:
 *   O3 = data segment              (preserved by task_init in O15)
 *   O8 = oriscdir mailbox sub-cap  (BOOT_PARENT_SLOT — wired by
 *                                   the launcher's --service "DIR=1@9")
 *
 * Self-registration: at boot the WM publishes a R+S sub-cap of its
 * service mailbox at /sys/wm/0 in oriscdir.  Clients walking that
 * path find us.
 *
 * Owner-task ref convention: clients pass their owning task ref in
 * O2 of OP_NEW_WINDOW.  The WM stashes the ref per-window and
 * task_queries it from scan_owner_exits during each idle pulse;
 * windows whose owner has EXITed get reclaimed automatically.
 */

#include "liborisc.h"

/* === Wire protocol constants (must match wm_smoke.c). ================== */

#define WM_OP_NEW_WINDOW         1
#define WM_OP_BIND_SURFACE       2
#define WM_OP_DESTROY_WINDOW     3
#define WM_OP_SUBSCRIBE_EVENTS   4

#define WIN_TYPE_CONSOLE   1
#define WIN_TYPE_GRAPHICAL 2

#define WSURF_CONSOLE  1
#define WSURF_KEYBOARD 2
#define WSURF_GRID     3
#define WSURF_VECTOR   4
#define WSURF_RASTER   5
#define WSURF_POINTER  6

#define E_INVAL    (-1)
#define E_NOENT    (-2)
#define E_IO       (-6)
#define E_NOSPC    (-7)
#define E_NOTIMPL  (-8)

/* Default window geometry — mirrors the milestone-1 Python prototype.
 * Future milestones will query oriscterm for actual dimensions. */
#define DEFAULT_W_PX     1200
#define DEFAULT_H_PX     600
#define DEFAULT_W_CELLS  80
#define DEFAULT_H_CELLS  24

/* ObjAlloc tags / cap bits. */
#define TAG_SERVICE 0x4103
#define CAP_R 0x01
#define CAP_W 0x02
#define CAP_S 0x08
#define CAP_V 0x10
#define CAP_C 0x40

/* === Slot offsets in O12 ================================================
 *
 * The libc ALLOC_BYTES allocation has a TABLE_BYTES (128) header for
 * task slots followed by ORX_STATE_BYTES (552) of orx-spawn / dir /
 * sup state.  The orx-manifest sub-region (offsets 152..535) is dead
 * weight for the WM since we never run orx_spawn.  We reuse it for
 * WM-specific stash:
 *
 *     152  WM_SURF_CONSOLE_SLOT     ref to /sys/term/0/console
 *     160  WM_SURF_KEYBOARD_SLOT    ref to /sys/term/0/keyboard
 *     168  WM_REPLY_SCRATCH_SLOT    derived reply sub-cap of our mailbox
 *                                   (used at self-register and
 *                                   ObjDerive call sites)
 *     176  WM_SCRATCH_SLOT          per-request reply_cap stash
 *     184..312  WM_OWNER_BASE       per-window owner task refs
 *                                   (16 windows × 8 bytes = 128)
 *     312..440  WM_CONSOLE_BASE     per-window CONSOLE service refs.
 *                                   The WM ObjAllocs a TAG_SERVICE
 *                                   per CONSOLE window so console
 *                                   writes from clients land in a
 *                                   per-window queue we can multiplex
 *                                   on (see the round-robin polling
 *                                   in main).  Phase 58 / WM β.
 *     440  WM_SURF_FRAMEBUFFER_SLOT  ref to /sys/term/0/framebuffer
 *                                    (Phase 59 / WM γ — the pixel
 *                                    surface the glyph renderer
 *                                    paints into).
 *     448  WM_FORWARD_SRC_SLOT      transient stash for the source
 *                                   bytes ref of an incoming console
 *                                   write, used during the
 *                                   ObjFetchBytes-then-forward dance
 *                                   in forward_console_write.
 *     456  WM_FORWARD_REPLY_SLOT    transient stash for the client's
 *                                   reply_cap on a console write.
 *
 * task.c reserves up to offset 672 (ORX_SLOT_O7_SAVE), and the orx-
 * manifest area runs 152..535 — we land safely inside it.  The WM
 * never invokes orx_spawn so the area's nominal use is moot.
 */

#define BOOT_PARENT_SLOT_OFFSET     544
#define DIR_SLOT_OFFSET             584

#define WM_SURF_CONSOLE_SLOT_OFFSET     152
#define WM_SURF_KEYBOARD_SLOT_OFFSET    160
#define WM_REPLY_SCRATCH_SLOT_OFFSET    168
#define WM_SCRATCH_SLOT_OFFSET          176
#define WM_OWNER_BASE_OFFSET            184
#define WM_CONSOLE_BASE_OFFSET          312
#define WM_SURF_FRAMEBUFFER_SLOT_OFFSET 440
#define WM_FORWARD_SRC_SLOT_OFFSET      448
#define WM_FORWARD_REPLY_SLOT_OFFSET    456

/* === Glyph rendering ==================================================
 *
 * The framebuffer is row-major, one byte per pixel (palette index;
 * see oriscterm's VEC_PALETTE).  We treat it as an N_COLS × N_ROWS
 * cell grid where each cell is CELL_W × CELL_H pixels.  Per-window
 * cursor tracking lives in window_cur_col[]/window_cur_row[].
 *
 * Pixel format / dimensions are hardcoded to 640×384 — matches the
 * milestone-α framebuffer size oriscterm allocates for an 80×24 cell
 * pane at Menlo-14 metrics on macOS.  If the underlying canvas is
 * larger we ignore the extra pixels; if smaller, ObjStoreBytes
 * returns RESP_BOUNDS and the row write is silently dropped (the WM
 * keeps running, glyphs that don't fit are just invisible).
 *
 * Forward-compat note: when the γ-stage migration moves the WM to
 * the terminal-CPU with a memory-mapped framebuffer, these
 * dimensions become queryable from the local framebuffer object's
 * size — but for the wire-mediated phase, hardcoding is good
 * enough. */

#define CELL_W   8
#define CELL_H   16
#define N_COLS   80
#define N_ROWS   24
#define FB_W    (CELL_W * N_COLS)   /* 640 */
#define FB_H    (CELL_H * N_ROWS)   /* 384 */

/* Palette indices (matching VEC_PALETTE in tools/devices/oriscterm). */
#define WM_BG_COLOR  0    /* dark navy background */
#define WM_FG_COLOR  1    /* light gray foreground */

/* Stack VA layout (CONTRACT.md §2).  Used for stack-relative offsets
 * passed to ObjFetchBytes / ObjStoreBytes — the boot stack ref lives
 * in O11 after task_init, and we compute byte offsets by subtracting
 * STACK_BOTTOM from a stack-local buffer's VA. */
#define STACK_BOTTOM 0x001f0000

/* 8×16 monospace font, 95 printable ASCII chars (codepoints 32..126),
 * 16 bytes per char (one row per byte, MSB = leftmost pixel).
 * Generated by tools/gen_wm_font.py from Menlo at 11pt with a
 * luminance threshold; re-run that tool to refresh.
 *
 * Glyphs that fall outside [32, 126] render as a blank cell.  '\n'
 * advances the cursor without rendering; other control chars are
 * silently dropped. */
static const unsigned char font_8x16[95][16] = {
    /*  \x20 */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '!' */ { 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00 },
    /*   '"' */ { 0x00, 0x00, 0x00, 0x00, 0x48, 0x48, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '#' */ { 0x00, 0x00, 0x00, 0x00, 0x14, 0x2c, 0x7e, 0x28, 0x28, 0xfc, 0x50, 0x50, 0x00, 0x00, 0x00, 0x00 },
    /*   '$' */ { 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x50, 0x50, 0x38, 0x14, 0x14, 0x78, 0x10, 0x00, 0x00, 0x00 },
    /*   '%' */ { 0x00, 0x00, 0x00, 0x00, 0x60, 0x90, 0x90, 0x6c, 0x30, 0xcc, 0x16, 0x1c, 0x00, 0x00, 0x00, 0x00 },
    /*   '&' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x40, 0x60, 0x60, 0xd6, 0x9c, 0xcc, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   "'" */ { 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '(' */ { 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x30, 0x20, 0x20, 0x20, 0x30, 0x10, 0x10, 0x00, 0x00, 0x00 },
    /*   ')' */ { 0x00, 0x00, 0x00, 0x20, 0x20, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x30, 0x20, 0x00, 0x00, 0x00 },
    /*   '*' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xd4, 0x38, 0x78, 0xd4, 0x10, 0x00, 0x00, 0x00, 0x00 },
    /*   '+' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0x7c, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   ',' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x30, 0x20, 0x00, 0x00 },
    /*   '-' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '.' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00 },
    /*   '/' */ { 0x00, 0x00, 0x00, 0x00, 0x0c, 0x08, 0x18, 0x10, 0x10, 0x20, 0x20, 0x40, 0x40, 0x00, 0x00, 0x00 },
    /*   '0' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x4c, 0x4c, 0x54, 0x64, 0x64, 0x6c, 0x38, 0x00, 0x00, 0x00, 0x00 },
    /*   '1' */ { 0x00, 0x00, 0x00, 0x00, 0x30, 0x50, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   '2' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x0c, 0x0c, 0x08, 0x18, 0x30, 0x60, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   '3' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x0c, 0x0c, 0x38, 0x08, 0x04, 0x0c, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   '4' */ { 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x28, 0x68, 0x48, 0xfc, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00 },
    /*   '5' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x40, 0x40, 0x78, 0x0c, 0x0c, 0x0c, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   '6' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x60, 0x40, 0x78, 0x4c, 0x44, 0x4c, 0x38, 0x00, 0x00, 0x00, 0x00 },
    /*   '7' */ { 0x00, 0x00, 0x00, 0x00, 0x7c, 0x0c, 0x08, 0x18, 0x10, 0x10, 0x30, 0x20, 0x00, 0x00, 0x00, 0x00 },
    /*   '8' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x4c, 0x4c, 0x38, 0x4c, 0x44, 0x4c, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   '9' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x4c, 0x44, 0x4c, 0x3c, 0x04, 0x08, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   ':' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00 },
    /*   ';' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00, 0x00, 0x30, 0x30, 0x30, 0x20, 0x00, 0x00 },
    /*   '<' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x38, 0xc0, 0x70, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '=' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x00, 0xfc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '>' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x38, 0x0c, 0x38, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '?' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x0c, 0x0c, 0x18, 0x10, 0x10, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00 },
    /*   '@' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x44, 0xdc, 0xa4, 0xa4, 0xa4, 0xdc, 0x60, 0x3c, 0x00, 0x00 },
    /*   'A' */ { 0x00, 0x00, 0x00, 0x00, 0x30, 0x38, 0x28, 0x68, 0x48, 0x7c, 0x44, 0xc4, 0x00, 0x00, 0x00, 0x00 },
    /*   'B' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x4c, 0x4c, 0x78, 0x44, 0x44, 0x44, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   'C' */ { 0x00, 0x00, 0x00, 0x00, 0x3c, 0x60, 0x40, 0x40, 0x40, 0x40, 0x60, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    /*   'D' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x4c, 0x44, 0x44, 0x44, 0x44, 0x4c, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   'E' */ { 0x00, 0x00, 0x00, 0x00, 0x7c, 0x40, 0x40, 0x7c, 0x40, 0x40, 0x40, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   'F' */ { 0x00, 0x00, 0x00, 0x00, 0x7c, 0x40, 0x40, 0x7c, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00 },
    /*   'G' */ { 0x00, 0x00, 0x00, 0x00, 0x3c, 0x60, 0x40, 0xc0, 0xcc, 0x44, 0x64, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    /*   'H' */ { 0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x44, 0x7c, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00, 0x00, 0x00 },
    /*   'I' */ { 0x00, 0x00, 0x00, 0x00, 0x7c, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   'J' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x70, 0x00, 0x00, 0x00, 0x00 },
    /*   'K' */ { 0x00, 0x00, 0x00, 0x00, 0x44, 0x48, 0x50, 0x70, 0x50, 0x48, 0x4c, 0x44, 0x00, 0x00, 0x00, 0x00 },
    /*   'L' */ { 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   'M' */ { 0x00, 0x00, 0x00, 0x00, 0xcc, 0xcc, 0xfc, 0xb4, 0xb4, 0x84, 0x84, 0x84, 0x00, 0x00, 0x00, 0x00 },
    /*   'N' */ { 0x00, 0x00, 0x00, 0x00, 0xc4, 0xc4, 0xe4, 0xa4, 0x94, 0x9c, 0x8c, 0x8c, 0x00, 0x00, 0x00, 0x00 },
    /*   'O' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x4c, 0x44, 0x44, 0x44, 0x44, 0x4c, 0x38, 0x00, 0x00, 0x00, 0x00 },
    /*   'P' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x44, 0x44, 0x4c, 0x78, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00 },
    /*   'Q' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x4c, 0x44, 0x44, 0x44, 0x44, 0x4c, 0x38, 0x08, 0x00, 0x00, 0x00 },
    /*   'R' */ { 0x00, 0x00, 0x00, 0x00, 0x78, 0x4c, 0x4c, 0x48, 0x78, 0x48, 0x44, 0x44, 0x00, 0x00, 0x00, 0x00 },
    /*   'S' */ { 0x00, 0x00, 0x00, 0x00, 0x38, 0x40, 0x40, 0x70, 0x1c, 0x04, 0x0c, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   'T' */ { 0x00, 0x00, 0x00, 0x00, 0xfc, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    /*   'U' */ { 0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x4c, 0x38, 0x00, 0x00, 0x00, 0x00 },
    /*   'V' */ { 0x00, 0x00, 0x00, 0x00, 0xc4, 0x44, 0x4c, 0x68, 0x28, 0x28, 0x38, 0x30, 0x00, 0x00, 0x00, 0x00 },
    /*   'W' */ { 0x00, 0x00, 0x00, 0x00, 0x86, 0x86, 0x94, 0xf4, 0x7c, 0x6c, 0x6c, 0x4c, 0x00, 0x00, 0x00, 0x00 },
    /*   'X' */ { 0x00, 0x00, 0x00, 0x00, 0x44, 0x6c, 0x38, 0x30, 0x38, 0x28, 0x4c, 0xc4, 0x00, 0x00, 0x00, 0x00 },
    /*   'Y' */ { 0x00, 0x00, 0x00, 0x00, 0xc4, 0x4c, 0x28, 0x30, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    /*   'Z' */ { 0x00, 0x00, 0x00, 0x00, 0x7c, 0x0c, 0x08, 0x10, 0x30, 0x20, 0x40, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   '[' */ { 0x00, 0x00, 0x00, 0x18, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x18, 0x00, 0x00, 0x00 },
    /*  '\\' */ { 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x20, 0x20, 0x10, 0x10, 0x18, 0x08, 0x0c, 0x00, 0x00, 0x00 },
    /*   ']' */ { 0x00, 0x00, 0x00, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x30, 0x00, 0x00, 0x00 },
    /*   '^' */ { 0x00, 0x00, 0x00, 0x00, 0x30, 0x68, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   '_' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x00 },
    /*   '`' */ { 0x00, 0x00, 0x00, 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /*   'a' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x0c, 0x3c, 0x44, 0x4c, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   'b' */ { 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x78, 0x6c, 0x44, 0x44, 0x6c, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   'c' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x60, 0x40, 0x40, 0x60, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    /*   'd' */ { 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c, 0x3c, 0x4c, 0x4c, 0x4c, 0x4c, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    /*   'e' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x44, 0x7c, 0x40, 0x40, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    /*   'f' */ { 0x00, 0x00, 0x00, 0x00, 0x1c, 0x10, 0x7c, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00 },
    /*   'g' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x4c, 0x4c, 0x4c, 0x4c, 0x3c, 0x0c, 0x78, 0x00, 0x00 },
    /*   'h' */ { 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x78, 0x4c, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00, 0x00, 0x00 },
    /*   'i' */ { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x30, 0x10, 0x10, 0x10, 0x10, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   'j' */ { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x30, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x70, 0x00, 0x00 },
    /*   'k' */ { 0x00, 0x00, 0x00, 0x00, 0x40, 0x40, 0x4c, 0x58, 0x70, 0x78, 0x48, 0x44, 0x00, 0x00, 0x00, 0x00 },
    /*   'l' */ { 0x00, 0x00, 0x00, 0x00, 0x70, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1c, 0x00, 0x00, 0x00, 0x00 },
    /*   'm' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xd4, 0xd4, 0xd4, 0xd4, 0xd4, 0x00, 0x00, 0x00, 0x00 },
    /*   'n' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x4c, 0x44, 0x44, 0x44, 0x44, 0x00, 0x00, 0x00, 0x00 },
    /*   'o' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x4c, 0x44, 0x44, 0x4c, 0x38, 0x00, 0x00, 0x00, 0x00 },
    /*   'p' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x44, 0x44, 0x44, 0x6c, 0x78, 0x40, 0x40, 0x00, 0x00 },
    /*   'q' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x4c, 0x4c, 0x4c, 0x4c, 0x3c, 0x04, 0x04, 0x00, 0x00 },
    /*   'r' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x30, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00 },
    /*   's' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x40, 0x70, 0x18, 0x0c, 0x78, 0x00, 0x00, 0x00, 0x00 },
    /*   't' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x7c, 0x20, 0x20, 0x20, 0x30, 0x1c, 0x00, 0x00, 0x00, 0x00 },
    /*   'u' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x44, 0x44, 0x44, 0x4c, 0x3c, 0x00, 0x00, 0x00, 0x00 },
    /*   'v' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x4c, 0x68, 0x28, 0x38, 0x30, 0x00, 0x00, 0x00, 0x00 },
    /*   'w' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x86, 0x84, 0xd4, 0x74, 0x6c, 0x68, 0x00, 0x00, 0x00, 0x00 },
    /*   'x' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4c, 0x28, 0x30, 0x30, 0x68, 0x44, 0x00, 0x00, 0x00, 0x00 },
    /*   'y' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x4c, 0x68, 0x28, 0x38, 0x10, 0x30, 0x60, 0x00, 0x00 },
    /*   'z' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x08, 0x10, 0x30, 0x60, 0x7c, 0x00, 0x00, 0x00, 0x00 },
    /*   '{' */ { 0x00, 0x00, 0x00, 0x1c, 0x10, 0x10, 0x10, 0x10, 0x60, 0x10, 0x10, 0x10, 0x1c, 0x00, 0x00, 0x00 },
    /*   '|' */ { 0x00, 0x00, 0x00, 0x00, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x00 },
    /*   '}' */ { 0x00, 0x00, 0x00, 0x60, 0x10, 0x10, 0x10, 0x10, 0x1c, 0x10, 0x10, 0x10, 0x60, 0x00, 0x00, 0x00 },
    /*   '~' */ { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x94, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

#define MAX_WINDOWS 16

/* === Per-window state in normal C globals ============================= */

/* window_id 0 is "invalid"; valid ids are 1..MAX_WINDOWS, mapped
 * to the [id-1] entry of these arrays. */
static int    window_type[MAX_WINDOWS];        /* WIN_TYPE_* (0 = free) */
static int    window_subscribe_op[MAX_WINDOWS]; /* notify_op (0 = none) */
static int    window_cur_col[MAX_WINDOWS];      /* cursor col within cell grid */
static int    window_cur_row[MAX_WINDOWS];      /* cursor row within cell grid */

/* The owner task ref and subscriber notify_cap live in OPR slots
 * (WM_OWNER_BASE_OFFSET + id*8 and WM_SUBSCRIBE_BASE_OFFSET + id*8)
 * so we can OREFST/OREFLD them.  We can't store cap refs in C
 * globals because the C language can't represent them. */

/* === Boot-OR restore ================================================== */

/* console_write picks O2 (stack) for stack-VA buffers and O3 (data)
 * for data-VA buffers; both get clobbered by ReceiveQueuePoll's
 * _deliver_queue_msg on every dispatch.  task_init parks boot stack
 * in O11 and boot data in O15.  Restore both before any print_str /
 * print_int. */
static void
wm_restore_boot_or(void)
{
	asm volatile("omov o2, o11\nomov o3, o15");
}

#define WM_PRINT(s)      do { wm_restore_boot_or(); print_str(s); } while (0)
#define WM_PRINT_INT(n)  do { wm_restore_boot_or(); print_int(n); } while (0)

/* === Service mailbox setup ============================================ */

/* Allocate a 16-byte TAG_SERVICE object, attach a queue, park the
 * full ref in O9.  Mirrors supervisor.c::allocate_service_mailbox. */
static int
allocate_service_mailbox(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"           /* TAG_SERVICE */
		"addiu r6, r0, %2\n"           /* R|W|S|V|C */
		"call  #0x100\n"               /* ObjAlloc → O1 */
		"nop\n"
		"omov  o9, o1\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, 8\n"            /* depth = 8 */
		"call  #0x203\n"               /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* === Surface-cap acquisition ========================================== */

/* dir_walk wraps OBJ_WALK and parks the resolved ref in
 * DIR_RESULT_SLOT (offset 616).  pcc-orisc requires the immediate
 * to OREFST be a fixed offset (not a register-computed one), so each
 * per-slot helper bakes its offset in via the asm "i" constraint. */
static int
walk_console_to_slot(void)
{
	int kind;
	char rem[16];
	int rc = dir_walk("/sys/term/0/console", &kind, rem, sizeof(rem));
	if (rc != 0) return rc;
	if (kind != DIR_KIND_LEAF) return -1;
	asm volatile(
		"orefld o1, 616(o12)\n"
		"orefst o1, %0(o12)"
		:
		: "i"(WM_SURF_CONSOLE_SLOT_OFFSET)
		: "r1"
	);
	return 0;
}

static int
walk_keyboard_to_slot(void)
{
	int kind;
	char rem[16];
	int rc = dir_walk("/sys/term/0/keyboard", &kind, rem, sizeof(rem));
	if (rc != 0) return rc;
	if (kind != DIR_KIND_LEAF) return -1;
	asm volatile(
		"orefld o1, 616(o12)\n"
		"orefst o1, %0(o12)"
		:
		: "i"(WM_SURF_KEYBOARD_SLOT_OFFSET)
		: "r1"
	);
	return 0;
}

static int
walk_framebuffer_to_slot(void)
{
	int kind;
	char rem[16];
	int rc = dir_walk("/sys/term/0/framebuffer", &kind, rem, sizeof(rem));
	if (rc != 0) return rc;
	if (kind != DIR_KIND_LEAF) return -1;
	asm volatile(
		"orefld o1, 616(o12)\n"
		"orefst o1, %0(o12)"
		:
		: "i"(WM_SURF_FRAMEBUFFER_SLOT_OFFSET)
		: "r1"
	);
	return 0;
}

/* === Self-register at /sys/wm/0 ======================================= */

/* dir_register publishes whatever's in O1 at the given path.  We
 * derive a R+S sub-cap of our mailbox first, then call dir_register. */
static int
self_register(void)
{
	int derive_status, register_status;
	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, 9\n"            /* R | S */
		"call  #0x103\n"               /* ObjDerive → O1 */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(derive_status)
		:
		: "r1", "r2", "r4"
	);
	if (derive_status != 0) return derive_status;

	register_status = dir_register("/sys/wm/0");
	return register_status;
}

/* === Reply helper ===================================================== */

/* SEND a reply back to a previously-stashed reply_cap.  The WM_
 * SCRATCH_SLOT holds the sender's O3 (reply_cap) from the dequeued
 * message.  The four R-payload slots and the O2 ref are explicit
 * arguments.  Restores boot ORs after, so subsequent print_str works. */
static void
wm_reply(int status, int r5, int r6, int r7)
{
	/* Park R-payload words in pcc-friendly variables first.  Then
	 * load reply_cap into O1 and SEND. */
	asm volatile(
		"orefld o1, %0(o12)\n"         /* O1 = stashed reply_cap */
		:
		: "i"(WM_SCRATCH_SLOT_OFFSET)
		: "r1"
	);
	asm volatile(
		"onull o3\n"
		"addu  r4, %0, r0\n"
		"addu  r5, %1, r0\n"
		"addu  r6, %2, r0\n"
		"addu  r7, %3, r0\n"
		"send  o1\n"
		:
		: "r"(status), "r"(r5), "r"(r6), "r"(r7)
		: "r4", "r5", "r6", "r7"
	);
	wm_restore_boot_or();
}

/* SEND a reply that ALSO carries a ref in O2 (used by OP_BIND_SURFACE
 * to return the surface cap, and by future ops that pass refs back).
 * The ref to return must already be in O14 when this is called. */
static void
wm_reply_with_ref_o14(int status)
{
	asm volatile(
		"orefld o1, %0(o12)\n"
		"omov   o2, o14\n"             /* return ref */
		"onull  o3\n"
		"addu   r4, %1, r0\n"
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(WM_SCRATCH_SLOT_OFFSET), "r"(status)
		: "r1", "r4", "r5", "r6", "r7"
	);
	wm_restore_boot_or();
}

/* === Stash incoming reply_cap ========================================= */

/* On a freshly dequeued SEND_DELIVER, O3 holds the sender's reply_cap
 * (their O3 was carried verbatim into our O3 by _deliver_queue_msg).
 * Stash it into WM_SCRATCH_SLOT before any subsequent asm clobbers O3
 * — wm_reply OREFLDs from the slot. */
static void
stash_reply_cap_o3(void)
{
	asm volatile("orefst o3, %0(o12)" :: "i"(WM_SCRATCH_SLOT_OFFSET));
}

/* === Per-window slot helpers ========================================== */

/* The WM_OWNER and WM_SUBSCRIBE slot bases hold MAX_WINDOWS refs each.
 * Slot offset = base + (window_id - 1) * 8.  pcc rejects orefst with
 * a computed offset, so we synthesize one via dynamic addressing —
 * load O12, add the offset, OREFST the ref into the computed pointer.
 *
 * Trick: use OREFST through a temporary slot.  We OREFST O1 into a
 * fixed scratch, then read the 8 bytes via lw and store into the
 * computed offset.  Cap refs are 8 bytes, naturally aligned, so this
 * is safe.
 *
 * Even simpler: a switch-statement dispatcher per window_id, like
 * supervisor.c::reply_to_requester does for task_t → ref translation.
 * MAX_WINDOWS is 16, so the switch has 16 cases.  Compact and pcc-
 * friendly. */

static void
stash_owner_o2(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o2, 184(o12)"); break;
	case  2: asm volatile("orefst o2, 192(o12)"); break;
	case  3: asm volatile("orefst o2, 200(o12)"); break;
	case  4: asm volatile("orefst o2, 208(o12)"); break;
	case  5: asm volatile("orefst o2, 216(o12)"); break;
	case  6: asm volatile("orefst o2, 224(o12)"); break;
	case  7: asm volatile("orefst o2, 232(o12)"); break;
	case  8: asm volatile("orefst o2, 240(o12)"); break;
	case  9: asm volatile("orefst o2, 248(o12)"); break;
	case 10: asm volatile("orefst o2, 256(o12)"); break;
	case 11: asm volatile("orefst o2, 264(o12)"); break;
	case 12: asm volatile("orefst o2, 272(o12)"); break;
	case 13: asm volatile("orefst o2, 280(o12)"); break;
	case 14: asm volatile("orefst o2, 288(o12)"); break;
	case 15: asm volatile("orefst o2, 296(o12)"); break;
	case 16: asm volatile("orefst o2, 304(o12)"); break;
	default: break;
	}
}

/* Load owner ref for `wid` into O14.  After this, callers can either
 * SEND on it or task_query it via libc's task_query — see
 * scan_owner_exits below. */
static void
load_owner_to_o14(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o14, 184(o12)"); break;
	case  2: asm volatile("orefld o14, 192(o12)"); break;
	case  3: asm volatile("orefld o14, 200(o12)"); break;
	case  4: asm volatile("orefld o14, 208(o12)"); break;
	case  5: asm volatile("orefld o14, 216(o12)"); break;
	case  6: asm volatile("orefld o14, 224(o12)"); break;
	case  7: asm volatile("orefld o14, 232(o12)"); break;
	case  8: asm volatile("orefld o14, 240(o12)"); break;
	case  9: asm volatile("orefld o14, 248(o12)"); break;
	case 10: asm volatile("orefld o14, 256(o12)"); break;
	case 11: asm volatile("orefld o14, 264(o12)"); break;
	case 12: asm volatile("orefld o14, 272(o12)"); break;
	case 13: asm volatile("orefld o14, 280(o12)"); break;
	case 14: asm volatile("orefld o14, 288(o12)"); break;
	case 15: asm volatile("orefld o14, 296(o12)"); break;
	case 16: asm volatile("orefld o14, 304(o12)"); break;
	default: asm volatile("onull o14"); break;
	}
}

/* === Per-window CONSOLE service slot helpers ==========================
 *
 * Phase 58: WM allocates a TAG_SERVICE per CONSOLE window so client
 * console writes land at a per-window queue we can multiplex via the
 * round-robin polling in main.  The full cap (R|W|S|V|C, what
 * ObjAlloc returned) lives in WM_CONSOLE_BASE+wid*8 — used for our
 * own ReceiveQueueAttach + ReceiveQueuePoll.  Clients receive an
 * R|S sub-cap (derived on bind_surface).
 *
 * Same per-wid switch shape as the WM_OWNER_BASE helpers above. */

static void
stash_console_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o1, 312(o12)"); break;
	case  2: asm volatile("orefst o1, 320(o12)"); break;
	case  3: asm volatile("orefst o1, 328(o12)"); break;
	case  4: asm volatile("orefst o1, 336(o12)"); break;
	case  5: asm volatile("orefst o1, 344(o12)"); break;
	case  6: asm volatile("orefst o1, 352(o12)"); break;
	case  7: asm volatile("orefst o1, 360(o12)"); break;
	case  8: asm volatile("orefst o1, 368(o12)"); break;
	case  9: asm volatile("orefst o1, 376(o12)"); break;
	case 10: asm volatile("orefst o1, 384(o12)"); break;
	case 11: asm volatile("orefst o1, 392(o12)"); break;
	case 12: asm volatile("orefst o1, 400(o12)"); break;
	case 13: asm volatile("orefst o1, 408(o12)"); break;
	case 14: asm volatile("orefst o1, 416(o12)"); break;
	case 15: asm volatile("orefst o1, 424(o12)"); break;
	case 16: asm volatile("orefst o1, 432(o12)"); break;
	default: break;
	}
}

/* Load the per-window CONSOLE service ref into O1 (used as the
 * ReceiveQueuePoll target and as the ObjDerive source when handing
 * a sub-cap to a client). */
static void
load_console_to_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 312(o12)"); break;
	case  2: asm volatile("orefld o1, 320(o12)"); break;
	case  3: asm volatile("orefld o1, 328(o12)"); break;
	case  4: asm volatile("orefld o1, 336(o12)"); break;
	case  5: asm volatile("orefld o1, 344(o12)"); break;
	case  6: asm volatile("orefld o1, 352(o12)"); break;
	case  7: asm volatile("orefld o1, 360(o12)"); break;
	case  8: asm volatile("orefld o1, 368(o12)"); break;
	case  9: asm volatile("orefld o1, 376(o12)"); break;
	case 10: asm volatile("orefld o1, 384(o12)"); break;
	case 11: asm volatile("orefld o1, 392(o12)"); break;
	case 12: asm volatile("orefld o1, 400(o12)"); break;
	case 13: asm volatile("orefld o1, 408(o12)"); break;
	case 14: asm volatile("orefld o1, 416(o12)"); break;
	case 15: asm volatile("orefld o1, 424(o12)"); break;
	case 16: asm volatile("orefld o1, 432(o12)"); break;
	default: asm volatile("onull o1"); break;
	}
}

/* === Surface-cap loader (for wm_reply_with_ref_o14) =================== */

static void
load_surface_to_o14(int kind)
{
	switch (kind) {
	case WSURF_CONSOLE:
		asm volatile("orefld o14, %0(o12)"
		             :: "i"(WM_SURF_CONSOLE_SLOT_OFFSET));
		break;
	case WSURF_KEYBOARD:
		asm volatile("orefld o14, %0(o12)"
		             :: "i"(WM_SURF_KEYBOARD_SLOT_OFFSET));
		break;
	default:
		asm volatile("onull o14");
		break;
	}
}

/* === Op handlers ====================================================== */

/* Allocate a TAG_SERVICE per-window console object: ObjAlloc + attach
 * a queue.  Returns the firmware status; on success the full cap
 * has been stashed into WM_CONSOLE_BASE+wid*8.
 *
 * Phase 58: clients who bind WSURF_CONSOLE on a window get a R|S
 * sub-cap of this object, so their console writes land on this
 * object's queue.  The WM round-robin-polls all per-window queues
 * in the dispatch loop and forwards incoming bytes to the
 * underlying terminal's CONSOLE service. */
#define TAG_SERVICE 0x4103
static int
alloc_window_console(int wid)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"           /* TAG_SERVICE */
		"addiu r6, r0, %2\n"           /* R|W|S|V|C */
		"call  #0x100\n"               /* ObjAlloc → O1 */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	/* Stash the full cap (now in O1) per-window. */
	stash_console_o1(wid);

	asm volatile(
		"addiu r4, r0, 8\n"            /* depth = 8 */
		"call  #0x203\n"               /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* WM_OP_NEW_WINDOW — allocate the next free window slot.
 *   R5 = window type
 *   O2 = owner task ref (for task_query auto-destroy)
 * On success, returns wid in R6, geometry packed in R4/R5.
 *
 * Phase 58: also ObjAllocs a TAG_SERVICE object for the window's
 * CONSOLE service and attaches a queue.  Hardcoded N=1 for CONSOLE
 * in this milestone — second NEW_WINDOW(CONSOLE) returns E_NOSPC.
 * Multi-window tiling lifts this in a follow-up.  GRAPHICAL returns
 * E_NOTIMPL. */
static void
handle_new_window(int wtype)
{
	int wid;

	if (wtype == WIN_TYPE_GRAPHICAL) {
		wm_reply(E_NOTIMPL, 0, 0, 0);
		return;
	}
	if (wtype != WIN_TYPE_CONSOLE) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}

	/* Hardcoded N=1: refuse if any CONSOLE window is alive. */
	for (wid = 1; wid <= MAX_WINDOWS; wid++) {
		if (window_type[wid - 1] == WIN_TYPE_CONSOLE) {
			wm_reply(E_NOSPC, 0, 0, 0);
			return;
		}
	}

	/* Find a free slot. */
	for (wid = 1; wid <= MAX_WINDOWS; wid++) {
		if (window_type[wid - 1] == 0) break;
	}
	if (wid > MAX_WINDOWS) {
		wm_reply(E_NOSPC, 0, 0, 0);
		return;
	}

	/* Stash the owner ref (sender's O2 → our O2 from queue dispatch).
	 * Do this BEFORE alloc_window_console — alloc_window_console
	 * issues ObjAlloc which clobbers O1 (and thereby O2 if the asm
	 * is order-sensitive — being explicit about ordering avoids
	 * surprises). */
	stash_owner_o2(wid);

	/* Allocate the per-window CONSOLE service.  On failure roll
	 * back the slot allocation; client retries with the same op. */
	int status = alloc_window_console(wid);
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_window_console failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		wm_reply(E_IO, 0, 0, 0);
		return;
	}

	window_type[wid - 1] = WIN_TYPE_CONSOLE;
	window_subscribe_op[wid - 1] = 0;
	window_cur_col[wid - 1] = 0;
	window_cur_row[wid - 1] = 0;

	int geom_a = ((DEFAULT_W_PX & 0xFFFF) << 16) | (DEFAULT_H_PX & 0xFFFF);
	int geom_b = ((DEFAULT_W_CELLS & 0xFFFF) << 16) | (DEFAULT_H_CELLS & 0xFFFF);
	wm_reply(0, geom_a, geom_b, wid);
}

/* Free the per-window CONSOLE service: ObjFree the underlying object
 * + null the slot so polling skips it. */
static void
free_window_console(int wid)
{
	load_console_to_o1(wid);
	int isn;
	asm volatile("oisn %0, o1" : "=r"(isn));
	if (isn) return;        /* never allocated */
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x101\n"        /* ObjFree */
		"nop"
		: : : "r1", "r2", "r4"
	);
	/* Null-out the slot. */
	asm volatile("onull o1");
	stash_console_o1(wid);
}

/* WM_OP_BIND_SURFACE — return a surface cap for a window.
 *   R4 = wid  (already-validated by dispatch)
 *   R5 = surface kind
 *
 * Phase 58:
 *   - WSURF_CONSOLE returns an R|S sub-cap of the per-window
 *     CONSOLE service (the WM is in the data path; client SENDs
 *     land in the per-window queue and the WM forwards them to the
 *     underlying terminal).
 *   - WSURF_KEYBOARD still returns the underlying terminal's
 *     keyboard cap directly (passthrough — keyboard mediation
 *     comes with multi-window in the next milestone). */
static void
handle_bind_surface(int wid, int kind)
{
	if (wid < 1 || wid > MAX_WINDOWS) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}
	int wtype = window_type[wid - 1];
	if (wtype == 0) {
		wm_reply(E_NOENT, 0, 0, 0);
		return;
	}
	if (wtype != WIN_TYPE_CONSOLE) {
		/* Shouldn't reach — graphical windows never get created. */
		wm_reply(E_NOTIMPL, 0, 0, 0);
		return;
	}
	if (kind != WSURF_CONSOLE && kind != WSURF_KEYBOARD) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}

	if (kind == WSURF_CONSOLE) {
		/* Load full per-window CONSOLE service cap into O1, derive
		 * an R|S sub-cap for the client, park in O14 for reply. */
		load_console_to_o1(wid);
		int derive_status;
		asm volatile(
			"addiu r4, r0, %1\n"        /* R | S */
			"call  #0x103\n"            /* ObjDerive → O1 */
			"nop\n"
			"omov  o14, o1\n"
			"addu  %0, r2, r0"
			: "=r"(derive_status)
			: "i"(CAP_R | CAP_S)
			: "r1", "r2", "r4"
		);
		if (derive_status != 0) {
			wm_reply(E_IO, 0, 0, 0);
			return;
		}
		wm_reply_with_ref_o14(0);
		return;
	}

	/* WSURF_KEYBOARD: passthrough to the underlying terminal cap
	 * walked at WM startup. */
	load_surface_to_o14(kind);
	int isn;
	asm volatile("oisn %0, o14" : "=r"(isn));
	if (isn) {
		wm_reply(E_NOENT, 0, 0, 0);
		return;
	}
	wm_reply_with_ref_o14(0);
}

/* WM_OP_DESTROY_WINDOW — release a window. */
static void
handle_destroy_window(int wid)
{
	if (wid < 1 || wid > MAX_WINDOWS) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}
	if (window_type[wid - 1] == 0) {
		wm_reply(E_NOENT, 0, 0, 0);
		return;
	}
	free_window_console(wid);
	window_type[wid - 1] = 0;
	window_subscribe_op[wid - 1] = 0;
	/* Owner-ref stash is left in place; future allocations will
	 * overwrite it.  No SEND fires for the close — the WM doesn't
	 * push events to subscribers yet. */
	wm_reply(0, 0, 0, 0);
}

/* WM_OP_SUBSCRIBE_EVENTS — record a notify_op for a window.
 * Stub in this milestone (the WM doesn't yet emit any events —
 * resize/focus/close all wait for the layout work in later
 * milestones), but the wire shape is committed.
 *
 * Phase 58: storage for the notify_cap was repurposed for
 * WM_CONSOLE_BASE; we accept the cap but discard it.  When events
 * actually fire we'll re-add storage (probably by extending
 * ORX_STATE_BYTES). */
static void
handle_subscribe_events(int wid, int notify_op)
{
	if (wid < 1 || wid > MAX_WINDOWS) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}
	if (window_type[wid - 1] == 0) {
		wm_reply(E_NOENT, 0, 0, 0);
		return;
	}
	if (notify_op < 1 || notify_op > 255) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}
	window_subscribe_op[wid - 1] = notify_op;
	wm_reply(0, 0, 0, 0);
}

/* === Auto-destroy via task_query ====================================== */

/* Helper: load WM_OWNER_BASE+wid*8 into O1 and call task_query
 * (#0x008).  Returns the firmware status (R2) in `out_status` and
 * the packed-state word (R3) in `out_state_word`.  R3's low 8 bits
 * are the TASK_STATE_* enum.
 *
 * task_query is remote (the owner ref typically lives on a
 * different CPU), so this call may block briefly while the
 * TASK_QUERY_REQ round-trips.  That's fine — the dispatch loop is
 * idle when scan_owner_exits runs.
 *
 * Per-wid switch matches the load_owner_to_o14 pattern: pcc
 * rejects computed-offset OREFLDs, so each case bakes its
 * offset in.  Offsets must match WM_OWNER_BASE_OFFSET = 184 +
 * (wid-1)*8. */
static int
task_query_owner(int wid, int *out_state_word)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 184(o12)"); break;
	case  2: asm volatile("orefld o1, 192(o12)"); break;
	case  3: asm volatile("orefld o1, 200(o12)"); break;
	case  4: asm volatile("orefld o1, 208(o12)"); break;
	case  5: asm volatile("orefld o1, 216(o12)"); break;
	case  6: asm volatile("orefld o1, 224(o12)"); break;
	case  7: asm volatile("orefld o1, 232(o12)"); break;
	case  8: asm volatile("orefld o1, 240(o12)"); break;
	case  9: asm volatile("orefld o1, 248(o12)"); break;
	case 10: asm volatile("orefld o1, 256(o12)"); break;
	case 11: asm volatile("orefld o1, 264(o12)"); break;
	case 12: asm volatile("orefld o1, 272(o12)"); break;
	case 13: asm volatile("orefld o1, 280(o12)"); break;
	case 14: asm volatile("orefld o1, 288(o12)"); break;
	case 15: asm volatile("orefld o1, 296(o12)"); break;
	case 16: asm volatile("orefld o1, 304(o12)"); break;
	default: asm volatile("onull o1"); break;
	}

	int status, state_word;
	asm volatile(
		"call  #0x008\n"            /* TaskQuery — Vol VI §4.2 */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0"
		: "=r"(status), "=r"(state_word)
		:
		: "r1", "r2", "r3"
	);
	*out_state_word = state_word;
	return status;
}

/* Walk the window table; for each live window whose owner task has
 * EXITED, free the slot.  Called periodically from the dispatch
 * loop's idle pulse.  Mirrors the slot-reaper pattern in
 * supervisor.c::reap_exited_tasks.
 *
 * Graceful degradation: clients that pass non-task refs (or null
 * refs) for the owner cause task_query to return non-zero status;
 * those windows are simply skipped — the slot stays alive, manual
 * OP_DESTROY_WINDOW remains the only cleanup path.  This is fine
 * for clients that don't care about auto-destroy and for tests
 * where the smoke task itself doesn't exit during the run. */
static void
scan_owner_exits(void)
{
	int wid;
	for (wid = 1; wid <= MAX_WINDOWS; wid++) {
		if (window_type[wid - 1] == 0) continue;

		int state_word;
		int rc = task_query_owner(wid, &state_word);
		/* Non-zero status: ref isn't a task or has been freed.
		 * Skip — clients without auto-destroy semantics use
		 * OP_DESTROY_WINDOW manually. */
		if (rc != 0) continue;

		/* low 8 bits = state per Vol VI §4.2's packed return. */
		int state = state_word & 0xFF;
		if (state == TASK_STATE_EXITED) {
			free_window_console(wid);
			window_type[wid - 1] = 0;
			window_subscribe_op[wid - 1] = 0;
			/* The owner-ref slot stays populated; it'll get
			 * overwritten by the next allocation in this slot.
			 * No SEND fires for the close — the WM doesn't push
			 * events to subscribers yet. */
		}
	}
}

/* === Main loop ======================================================== */

#define WM_POLL_TICKS 100   /* ~100ms.  Short enough that per-window
                             * CONSOLE writes (which we round-robin
                             * after each main-service iteration) get
                             * drained promptly even when main is
                             * idle. */

static int
poll_one_request(int *out_op, int *out_wid, int *out_arg)
{
	int status, op, wid, arg;
	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, %4\n"           /* timeout */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0\n"
		"addu  %3, r5, r0"
		: "=r"(status), "=r"(op), "=r"(wid), "=r"(arg)
		: "i"(WM_POLL_TICKS)
		: "r1", "r2", "r3", "r4", "r5"
	);
	*out_op  = op;
	*out_wid = wid;
	*out_arg = arg;
	return status;
}

/* Render one glyph for character `ch` at the current cursor position
 * of window `wid`.  '\n' advances the cursor without rendering;
 * other unprintable chars are silently dropped.  Cursor wraps at
 * column N_COLS; rows past N_ROWS stop rendering (no scroll yet —
 * future milestone work).
 *
 * Each glyph requires CELL_H = 16 row writes to the framebuffer.
 * Each row is CELL_W = 8 bytes built on the local stack and pushed
 * via ObjStoreBytes (1 wire RTT per row, 16 RTTs per glyph).  Bulk
 * push + per-row pack is what makes interactive text rendering
 * tractable in the wire-mediated phase — without ObjStoreBytes
 * we'd need 128 RTTs per glyph (one per OSB byte). */
static void
render_glyph(int wid, int ch)
{
	int col = window_cur_col[wid - 1];
	int row = window_cur_row[wid - 1];

	if (ch == '\n') {
		window_cur_col[wid - 1] = 0;
		window_cur_row[wid - 1] = row + 1;
		return;
	}
	if (ch == '\r') {
		window_cur_col[wid - 1] = 0;
		return;
	}
	if (ch == '\b') {
		if (col > 0) window_cur_col[wid - 1] = col - 1;
		/* Backspace doesn't erase the rendered glyph in milestone
		 * γ — the next-char render at this position will overwrite
		 * it.  For a pure backspace-and-leave-blank, we'd need to
		 * render a space at (col-1, row). */
		return;
	}
	/* Out-of-range chars: skip render but advance cursor (so the
	 * source byte position is preserved visually). */
	if (ch < 32 || ch > 126) {
		window_cur_col[wid - 1] = (col + 1 >= N_COLS) ? 0 : col + 1;
		if (col + 1 >= N_COLS) window_cur_row[wid - 1] = row + 1;
		return;
	}
	if (row >= N_ROWS) return;     /* off-screen — drop, no scroll */

	/* Glyph data: 16 bytes from the embedded font.  Index by
	 * (ch - 32) since the font array starts at codepoint 32.
	 *
	 * For each row, build 8 palette-index bytes (foreground for set
	 * bits, background for clear) on the stack, then ObjStoreBytes
	 * them to the framebuffer at the right pixel offset. */
	const unsigned char *glyph = font_8x16[ch - 32];
	int cell_x = col * CELL_W;
	int cell_y = row * CELL_H;
	int r;
	for (r = 0; r < CELL_H; r++) {
		unsigned char glyph_byte = glyph[r];
		unsigned char row_pixels[CELL_W];
		int x;
		for (x = 0; x < CELL_W; x++) {
			int bit = (glyph_byte >> (7 - x)) & 1;
			row_pixels[x] = bit ? WM_FG_COLOR : WM_BG_COLOR;
		}
		int src_off = (int)((unsigned int)row_pixels - STACK_BOTTOM);
		int dst_off = (cell_y + r) * FB_W + cell_x;
		/* ObjStoreBytes(O1=src, O2=dst, R4=src_off, R5=dst_off,
		 * R6=count). */
		asm volatile(
			"omov  o1, o11\n"           /* boot stack ref */
			"orefld o2, %0(o12)\n"      /* framebuffer cap */
			"addu  r4, %1, r0\n"
			"addu  r5, %2, r0\n"
			"addiu r6, r0, %3\n"        /* CELL_W */
			"call  #0x109\n"            /* ObjStoreBytes */
			"nop"
			:
			: "i"(WM_SURF_FRAMEBUFFER_SLOT_OFFSET),
			  "r"(src_off), "r"(dst_off),
			  "i"(CELL_W)
			: "r1", "r2", "r3", "r4", "r5", "r6"
		);
	}

	/* Advance cursor; wrap at column N_COLS. */
	if (col + 1 >= N_COLS) {
		window_cur_col[wid - 1] = 0;
		window_cur_row[wid - 1] = row + 1;
	} else {
		window_cur_col[wid - 1] = col + 1;
	}
}

/* Forward an incoming per-window CONSOLE write to the underlying
 * terminal AND glyph-render the bytes into the framebuffer.  Called
 * from poll_window_consoles after a successful ReceiveQueuePoll on a
 * per-window CONSOLE queue.
 *
 * On dispatch the queue-poll overlay sets:
 *   R3 = sender's R4 = byte offset       (passed in as `offset`)
 *   R4 = sender's R5 = byte count        (passed in as `count`)
 *   O2 = sender's O2 = source bytes ref
 *   O3 = sender's O3 = reply_cap         (passes through to terminal)
 *
 * `wid`/`offset`/`count` are passed by the caller because the
 * function-call ABI puts them in R4..R6, which would otherwise
 * clobber the post-overlay R3/R4 we want to read.  O2/O3 are still
 * intact at function entry — pcc-orisc doesn't touch OPRs across
 * a call.
 *
 * Step-by-step:
 *   1. Stash O2 (source ref) and O3 (reply_cap) into per-request
 *      slots so subsequent asm can stomp on them.
 *   2. ObjFetchBytes the bytes from the client into a stack-local
 *      buffer (one wire RTT, arbitrary count up to 256).
 *   3. Iterate the buffer, render_glyph each byte.
 *   4. Re-emit the SEND to underlying terminal CONSOLE with the
 *      original (source, offset, count, reply_cap) — so the existing
 *      oriscterm console pane keeps showing text and tests that
 *      grep it keep passing.  When the γ-stage migration retires
 *      that pane, this forward step goes away (the render step
 *      stays — text becomes pixels only). */
static void
forward_console_write(int wid, int offset, int count)
{
	/* Stash source ref + reply_cap into per-request slots.  These
	 * orefst's must run before any C work — pcc's prologue can spill
	 * R4..R6 to stack to back the wid/offset/count locals, but it
	 * doesn't touch O2/O3 across the call, so they're still the
	 * SEND's source ref and reply_cap. */
	asm volatile("orefst o2, %0(o12)"
	             :: "i"(WM_FORWARD_SRC_SLOT_OFFSET));
	asm volatile("orefst o3, %0(o12)"
	             :: "i"(WM_FORWARD_REPLY_SLOT_OFFSET));

	/* ObjFetchBytes from the client's source into a stack-local
	 * buffer.  Clamp to 256 bytes — the standard term_print path
	 * batches per-string, so a typical write is short.  Anything
	 * larger gets the leading 256 rendered + the full count
	 * forwarded (so the underlying terminal still gets all the
	 * bytes). */
	unsigned char buf[256];
	int fetch_count = (count > 256) ? 256 : count;
	if (fetch_count > 0) {
		int buf_off = (int)((unsigned int)buf - STACK_BOTTOM);
		asm volatile(
			"orefld o1, %0(o12)\n"     /* source from stash */
			"omov  o2, o11\n"          /* dest = boot stack */
			"addu  r4, %1, r0\n"       /* src offset */
			"addu  r5, %2, r0\n"       /* dst offset */
			"addu  r6, %3, r0\n"       /* count */
			"call  #0x108\n"           /* ObjFetchBytes */
			"nop"
			:
			: "i"(WM_FORWARD_SRC_SLOT_OFFSET),
			  "r"(offset), "r"(buf_off), "r"(fetch_count)
			: "r1", "r2", "r3", "r4", "r5", "r6"
		);

		/* Render each fetched byte to the framebuffer.  This is the
		 * Phase 59 / WM γ.2 milestone: bytes that go to oriscterm's
		 * console pane also become pixels in the framebuffer pane.
		 * When the γ-stage migration retires the console pane this
		 * forward_console_write loses its forward step but keeps
		 * this render step — text goes only to pixels. */
		int i;
		for (i = 0; i < fetch_count; i++) {
			render_glyph(wid, (int)buf[i]);
		}
	}

	/* Forward: re-emit the SEND with the original source/offset/
	 * count/reply_cap.  Underlying terminal does its own
	 * OBJ_READ_REQ from the source, same as before. */
	asm volatile(
		"orefld o1, %0(o12)\n"          /* O1 = underlying CONSOLE cap */
		"orefld o2, %1(o12)\n"          /* O2 = original source ref */
		"orefld o3, %2(o12)\n"          /* O3 = client's reply_cap */
		"addu   r4, %3, r0\n"           /* R4 = offset */
		"addu   r5, %4, r0\n"           /* R5 = count */
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1\n"
		:
		: "i"(WM_SURF_CONSOLE_SLOT_OFFSET),
		  "i"(WM_FORWARD_SRC_SLOT_OFFSET),
		  "i"(WM_FORWARD_REPLY_SLOT_OFFSET),
		  "r"(offset), "r"(count)
		: "r1", "r4", "r5", "r6", "r7"
	);
}

/* Round-robin poll all per-window CONSOLE queues with timeout=0.
 * For each pending SEND, forward to the underlying terminal.
 *
 * Empty queues return ETIMEOUT immediately, so this is cheap.  We
 * call it after every main-service iteration (whether a request
 * dispatched or the poll timed out) so per-window writes have at
 * most ~WM_POLL_TICKS of latency on top of the natural turn-around
 * of the producing program. */
static void
poll_window_consoles(void)
{
	int wid;
	for (wid = 1; wid <= MAX_WINDOWS; wid++) {
		if (window_type[wid - 1] != WIN_TYPE_CONSOLE) continue;

		/* Load full per-window CONSOLE cap into O1 and poll with
		 * timeout=0.  Status ERR_OK (0) means we got a SEND;
		 * forward it.  Anything else (typically ETIMEOUT) means
		 * the queue is empty — move on. */
		load_console_to_o1(wid);
		int status, offset, count;
		asm volatile(
			"addiu r4, r0, 0\n"        /* timeout = 0 (non-blocking) */
			"call  #0x204\n"           /* ReceiveQueuePoll */
			"nop\n"
			"addu  %0, r2, r0\n"
			"addu  %1, r3, r0\n"
			"addu  %2, r4, r0"
			: "=r"(status), "=r"(offset), "=r"(count)
			:
			: "r1", "r2", "r3", "r4", "r5", "r6"
		);
		if (status == 0) {
			/* O2 (source) and O3 (reply_cap) survive the call —
			 * pcc-orisc treats OPRs as scratch but our asm reloads
			 * them from the stash slots inside forward_console_write. */
			forward_console_write(wid, offset, count);
		}
	}
}

const char banner_boot[]            = "oriscwm: booting\n";
const char banner_console_walk_ok[] = "oriscwm: /sys/term/0/console acquired\n";
const char banner_keyboard_walk_ok[]= "oriscwm: /sys/term/0/keyboard acquired\n";
const char banner_register_ok[]     = "oriscwm: registered at /sys/wm/0\n";
const char banner_register_fail[]   = "oriscwm: dir_register /sys/wm/0 failed: ";
const char banner_walk_console_fail[]  = "oriscwm: /sys/term/0/console walk failed: ";
const char banner_walk_keyboard_fail[] = "oriscwm: /sys/term/0/keyboard walk failed: ";
const char banner_alloc_fail[]      = "oriscwm: failed to allocate service mailbox: ";
const char banner_ready[]           = "oriscwm: ready\n";

int
main(void)
{
	int status;

	/* Phase 45e/55 pattern: allocate the service mailbox before
	 * task_init touches descriptors so it lands at a deterministic
	 * idx.  Then task_init(). */
	status = allocate_service_mailbox();
	if (status != 0) {
		print_str(banner_alloc_fail);
		print_int(status);
		print_str("\n");
		return 1;
	}
	task_init();

	WM_PRINT(banner_boot);

	/* Boot O8 carries the directory mailbox — the launcher wires it
	 * via --service "DIR=1@9".  task_init parked O8 into
	 * BOOT_PARENT_SLOT; promote it to DIR_SLOT so dir.c's init path
	 * skips the SUP_OP_GET_DIR fetch (we have no parent supervisor
	 * to ask). */
	asm volatile(
		"orefld o1, %0(o12)\n"
		"orefst o1, %1(o12)"
		:
		: "i"(BOOT_PARENT_SLOT_OFFSET), "i"(DIR_SLOT_OFFSET)
		: "r1"
	);

	/* Walk the directory for our underlying surface caps. */
	status = walk_console_to_slot();
	if (status == 0) {
		WM_PRINT(banner_console_walk_ok);
	} else {
		WM_PRINT(banner_walk_console_fail);
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		/* Continue — OP_BIND_SURFACE will return E_NOENT for the
		 * missing kind. */
	}
	status = walk_keyboard_to_slot();
	if (status == 0) {
		WM_PRINT(banner_keyboard_walk_ok);
	} else {
		WM_PRINT(banner_walk_keyboard_fail);
		WM_PRINT_INT(status);
		WM_PRINT("\n");
	}
	status = walk_framebuffer_to_slot();
	if (status == 0) {
		WM_PRINT("oriscwm: /sys/term/0/framebuffer acquired\n");
	} else {
		WM_PRINT("oriscwm: /sys/term/0/framebuffer walk failed: ");
		WM_PRINT_INT(status);
		WM_PRINT(" — glyph rendering disabled\n");
	}

	/* Self-register at /sys/wm/0. */
	status = self_register();
	if (status != 0) {
		WM_PRINT(banner_register_fail);
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		return 1;
	}
	WM_PRINT(banner_register_ok);

	/* Initialise per-window state. */
	{
		int i;
		for (i = 0; i < MAX_WINDOWS; i++) {
			window_type[i] = 0;
			window_subscribe_op[i] = 0;
			window_cur_col[i] = 0;
			window_cur_row[i] = 0;
		}
	}

	WM_PRINT(banner_ready);

	/* Dispatch loop.
	 *
	 * Each iteration:
	 *   1. Poll our main service queue with WM_POLL_TICKS timeout —
	 *      handles NEW_WINDOW / BIND_SURFACE / DESTROY_WINDOW /
	 *      SUBSCRIBE_EVENTS.
	 *   2. After the main poll (whether it dispatched or timed out),
	 *      drain all per-window CONSOLE queues round-robin and
	 *      forward writes to the underlying terminal.  Per-window
	 *      polling is non-blocking (timeout=0) so empty queues
	 *      cost essentially nothing.
	 *   3. On main-poll timeout: run the auto-destroy scan
	 *      (task_query each window's owner; free EXITed slots).
	 *
	 * Per-window CONSOLE writes get drained on every iteration, so
	 * their latency is bounded by the main-poll's turn-around.
	 * Typical interactive flow (user types → keyboard event wakes
	 * focused program → program writes to its console → SEND lands
	 * in per-window queue → next iteration drains it) keeps the
	 * latency to roughly the wall-clock time of the program's own
	 * processing.  Pure-idle latency caps at WM_POLL_TICKS. */
	for (;;) {
		int op, wid_or_zero, arg;
		int status = poll_one_request(&op, &wid_or_zero, &arg);
		if (status == 0) {
			/* Stash sender's reply_cap (their O3, our O3 post-
			 * dispatch) into WM_SCRATCH_SLOT before any subsequent
			 * asm clobbers O3. */
			stash_reply_cap_o3();

			if (op == WM_OP_NEW_WINDOW) {
				handle_new_window(arg);
			} else if (op == WM_OP_BIND_SURFACE) {
				handle_bind_surface(wid_or_zero, arg);
			} else if (op == WM_OP_DESTROY_WINDOW) {
				handle_destroy_window(wid_or_zero);
			} else if (op == WM_OP_SUBSCRIBE_EVENTS) {
				handle_subscribe_events(wid_or_zero, arg);
			} else {
				wm_reply(E_INVAL, 0, 0, 0);
			}
		} else {
			/* Timeout or transient — run the auto-destroy scan. */
			scan_owner_exits();
		}

		/* Drain any pending per-window CONSOLE writes. */
		poll_window_consoles();
	}
}
