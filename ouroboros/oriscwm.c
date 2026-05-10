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
 *   WM_OP_QUERY_GEOMETRY   R4 = wid (or 0 = first live window)
 *                          Reply: R3=status, R4=geom_a, R5=geom_b,
 *                                 R6=resolved wid
 *
 *   WM_OP_SET_TITLE        R4 = wid (or 0 = first live)
 *                          R5 = packed (len:high16, src_off:low16)
 *                          O2 = source ref containing title bytes
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
#define WM_OP_QUERY_GEOMETRY     5
#define WM_OP_SET_TITLE          6

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

/* Default window geometry — reported back to wm_new_window callers
 * as the *usable* (inside-the-border) cell area in geom_b, and the
 * usable pixel area in geom_a.  The full FB is FB_W × FB_H but the
 * outer ring of cells is reserved for the window border / padding,
 * so programs see a slightly smaller cell grid (see N_COLS / N_ROWS
 * below). */
/* DEFAULT_*_CELLS / DEFAULT_*_PX must track N_COLS / N_ROWS, which
 * are defined alongside the cell-grid constants further down.  We
 * forward-declare them as numeric literals here; if those change, so
 * must these. */
#define DEFAULT_W_CELLS  158
#define DEFAULT_H_CELLS  45    /* matches N_ROWS post-Phase-60-step-8 */
#define DEFAULT_W_PX     (DEFAULT_W_CELLS * 8)   /* 1264 */
#define DEFAULT_H_PX     (DEFAULT_H_CELLS * 16)  /* 720  */

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
 *     720..848  WM_GRID_BASE         per-window GRID service refs
 *                                   (16 windows × 8 bytes).  Same
 *                                   shape as WM_CONSOLE_BASE — clients
 *                                   that wm_bind_surface(WSURF_GRID)
 *                                   get an R|S sub-cap of the
 *                                   per-window object; the WM polls
 *                                   the queues from the dispatch loop
 *                                   and forward_grid_write rasterises
 *                                   the bytes into the framebuffer at
 *                                   the (col, row) the SEND specifies.
 *                                   Phase 59 / WM γ.9.  (Was 712..840
 *                                   before WM_VECTOR_CAP_SLOT pushed
 *                                   ORX_STATE_BYTES to 720 in γ.11.)
 *     848..976  WM_VECTOR_BASE       per-window VECTOR service refs
 *                                   (16 windows × 8 bytes).  Mirrors
 *                                   WM_GRID_BASE for line / rect /
 *                                   oval rasterisation through the
 *                                   WM.  Phase 59 / WM γ.11.
 *     984..1112 WM_RASTER_BASE       per-window RASTER service refs
 *                                   (16 windows × 8 bytes).  Same
 *                                   shape — clients that
 *                                   wm_bind_surface(WSURF_RASTER)
 *                                   get an R|S sub-cap; the WM polls
 *                                   the queues from the dispatch loop
 *                                   and forward_raster_write blits
 *                                   their pixel buffers into the
 *                                   framebuffer.  Phase 59 / WM γ.12.
 *                                   (Skips offset 976 — that's
 *                                   WM_RASTER_CAP_SLOT in the libc
 *                                   layout, lives between
 *                                   WM_VECTOR_BASE and WM_RASTER_BASE
 *                                   to avoid renumbering 32 hardcoded
 *                                   per-wid offsets in stash/load
 *                                   switches above.)
 *
 * task.c reserves up to offset 720 (post-γ.11: WM_LEADER_CONSOLE_SLOT
 * 696, WM_LEADER_GRID_SLOT 704, WM_VECTOR_CAP_SLOT 712), and the
 * orx-manifest area runs 152..535 — we land safely inside it for the
 * slots up to 464.  WM_GRID_BASE / WM_VECTOR_BASE / WM_RASTER_BASE
 * sit past the supervisor's scratch slots.  The WM never invokes
 * orx_spawn so the orx-manifest area's nominal use is moot.
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
#define WM_GRID_BASE_OFFSET             720
#define WM_VECTOR_BASE_OFFSET           848
#define WM_RASTER_BASE_OFFSET           984

/* Phase 59 / WM γ.13 — pointer mediation.  Single global subscriber
 * for v1; multi-window focus is post-multi-window WM work. */
#define WM_POINTER_CAP_SLOT_OFFSET      1112  /* libc-visible bound cap */
#define WM_POINTER_SVC_SLOT_OFFSET      1120  /* TAG_SERVICE clients SEND to */
#define WM_PTR_SUB_SLOT_OFFSET          1128  /* current subscriber's reply ref */
#define WM_PTR_EVENTS_SLOT_OFFSET       1136  /* TAG_INPUT_SINK kind=1 */
#define WM_SURF_POINTER_SLOT_OFFSET     1144  /* unused post step 3 */

/* Phase 60 step 3 — terminal-firmware unification.  The WM allocates
 * its keyboard / pointer event sinks LOCALLY via the simorisc
 * #0x10B ObjAllocInputSink primitive; the host display worker
 * (--display tk) translates Tk events into SEND_DELIVER packets and
 * appends them to the sink queue.  oriscterm no longer mediates
 * input — the WM CPU IS the terminal. */
#define WM_KEYBOARD_SVC_SLOT_OFFSET     1152  /* TAG_SERVICE clients SEND to */
#define WM_KBD_SUB_SLOT_OFFSET          1160  /* current subscriber's reply ref */
#define WM_KBD_EVENTS_SLOT_OFFSET       1168  /* TAG_INPUT_SINK kind=0 */

/* Phase 60 step 11 — per-wid window backing stores.  Each CONSOLE
 * window owns an offscreen TAG_FRAMEBUFFER at WM_WINDOW_FB_BASE +
 * (wid - 1) * 8.  load_window_fb_to_o1(wid) does the per-wid
 * dispatch (pcc-orisc rejects computed-offset OREFLD).  The screen
 * FB at WM_SURF_FRAMEBUFFER_SLOT_OFFSET stays the host-mirrored
 * surface; the WM ObjBlitCopy-composites window FBs onto it in
 * z-order. */
#define WM_WINDOW_FB_BASE_OFFSET        1176

/* The painting helpers (flush_strip, fb_blit_row, fill_rect_window,
 * blit_title_text, fb_scroll_up_one_cell, alloc/free_window_fb)
 * target this slot rather than reading from the per-wid array
 * directly — saves us from threading wid through every asm block.
 * Caller invokes set_active_window(wid) before any paint to copy
 * the right per-wid FB ref into here. */
#define WM_ACTIVE_FB_SLOT_OFFSET        1304

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

/* Cell dims set by gen_wm_font.py — don't change without regenerating
 * the embedded font_8x16 array. */
#define CELL_W   8
#define CELL_H   16

/* The framebuffer is 160 × 48 cells (1280 × 768 px), but the outer
 * ring of cells is reserved for the window border / padding.  We
 * report the *usable* inner grid to clients and offset all cell
 * rendering by BORDER_CELLS in each direction so (col=0, row=0) in
 * client coordinates lands at FB cell (BORDER_CELLS, BORDER_CELLS).
 *
 * Layout (with BORDER_CELLS=1):
 *   y px       what
 *   0..15      top margin (cell row 0); border line at y=14..15
 *   16..751    usable cell rows 0..45 (46 rows × 16 px = 736)
 *   752..767   bottom margin (cell row 47); border line at y=752..753
 *   x px       what
 *   0..7       left margin (cell col 0); border line at x=6..7
 *   8..1271    usable cell cols 0..157 (158 cols × 8 px = 1264)
 *   1272..1279 right margin (cell col 159); border line at x=1272..1273
 *
 * Border thickness BORDER_LINE_PX is 2 — readable on a 1280×768 CRT
 * without dominating the visual; matches the chunky one-pixel-line
 * look of mid-80s desktops without going full skeuomorphic.  Border
 * sits one pixel inside the cell-row/col boundary so it touches the
 * usable cell area's outer edge cleanly. */
#define BORDER_CELLS    1
#define BORDER_LINE_PX  2

/* Phase 60 step 8 — per-window title bar.  Lives at the top of each
 * window's backing store (pixel rows [0..TITLE_BAR_PX)); the cell
 * content area follows at [TITLE_BAR_PX..USABLE_H_PX).  Eats one
 * cell-row's worth of vertical space, so the cell grid client
 * programs see drops from 46 to 45 rows.  16-px tall fits the
 * 16-px glyph with zero vertical padding — tight but unambiguous;
 * a future polish pass can widen it. */
#define TITLE_BAR_CELLS 1
#define TITLE_BAR_PX    (CELL_H * TITLE_BAR_CELLS)   /* 16 */

#define N_COLS          158
#define N_ROWS          45     /* was 46; one cell row went to title bar */
#define FB_CELLS_W      (N_COLS + 2 * BORDER_CELLS)
#define FB_CELLS_H      (N_ROWS + 2 * BORDER_CELLS + TITLE_BAR_CELLS)
#define FB_W            (CELL_W * FB_CELLS_W)   /* 1280 */
#define FB_H            (CELL_H * FB_CELLS_H)   /* 768  */
#define CELL_ORIGIN_X   (CELL_W * BORDER_CELLS)  /* 8  */
#define CELL_ORIGIN_Y   (CELL_H * BORDER_CELLS)  /* 16 */
#define USABLE_W_PX     (CELL_W * N_COLS)        /* 1264 */
/* Window FB height covers title bar + cell content. */
#define USABLE_H_PX     (TITLE_BAR_PX + CELL_H * N_ROWS)   /* 736 */
#define CELL_CONTENT_PX (CELL_H * N_ROWS)                  /* 720 */

/* Palette indices (matching VEC_PALETTE in tools/devices/oriscterm). */
#define WM_BG_COLOR  0    /* dark navy background */
#define WM_FG_COLOR  1    /* light gray foreground */
#define WM_BORDER_COLOR 1 /* same fg gray for the chrome line */
/* Title bar uses inverse-video: bar bg = fg-gray, text = bg-navy. */
#define WM_TITLE_BAR_BG WM_FG_COLOR
#define WM_TITLE_BAR_FG WM_BG_COLOR

/* Title string storage — single window for v1, becomes per-wid when
 * multi-window lands.  Sized to the max title that fits the bar with
 * no horizontal padding (158 cells × 1 byte).  ASCII only — same
 * font_8x16 constraint as the rest of the WM's text rendering. */
#define MAX_TITLE_LEN   N_COLS

/* Stack VA layout (CONTRACT.md §2).  Used for stack-relative offsets
 * passed to ObjFetchBytes / ObjStoreBytes — the boot stack ref lives
 * in O11 after task_init, and we compute byte offsets by subtracting
 * STACK_BOTTOM from a stack-local buffer's VA. */
#define STACK_BOTTOM 0x001f0000
/* Boot data segment VA — used by forward_raster_write to address its
 * static scratch buffer (vec_scratch_row) through O15 (boot data ref)
 * for ObjFetchBytes / ObjStoreBytes.  Same constant grid.c / raster.c
 * use on the libc side. */
#define DATA_VA      0x00040000

/* 8×16 bitmap font, 95 printable ASCII chars (codepoints 32..126),
 * 16 pixel rows per char packed into 4 big-endian 32-bit words
 * (4 rows per word, MSB = earlier row; within a row, MSB = leftmost
 * pixel).  Stored as uint32 instead of uint8 because pcc-orisc has
 * an .ascii-emission bug for byte-array initializers — see the
 * generator's docstring for details.
 *
 * Generated by tools/gen_wm_font.py — to refresh, run e.g.:
 *   python3 tools/gen_wm_font.py --preset lucida > /tmp/font.txt
 * then paste the array below.  See the script for other presets
 * (`menlo`, `courier`) and `--font PATH` for custom fonts.
 *
 * Source font: Lucida Sans Typewriter @ 11pt (deliberately distinct
 * from oriscterm's Menlo console pane — the framebuffer pane and
 * console pane render the same text in different fonts so it's
 * obvious the WM is doing the bitmap rendering, not just mirroring
 * the console pane.)
 *
 * Glyphs that fall outside [32, 126] render as a blank cell.  '\n'
 * advances the cursor without rendering; other control chars are
 * silently dropped. */
static const unsigned int font_8x16[95][4] = {
    /*  \x20 */ { 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    /*   '!' */ { 0x00000000, 0x10101010, 0x10100010, 0x00000000 },
    /*   '"' */ { 0x00000068, 0x68680000, 0x00000000, 0x00000000 },
    /*   '#' */ { 0x00000000, 0x14287c28, 0x28fc5050, 0x00000000 },
    /*   '$' */ { 0x00000010, 0x38707030, 0x181c1c78, 0x00000000 },
    /*   '%' */ { 0x00000000, 0xe4a4a870, 0x3c32528c, 0x00000000 },
    /*   '&' */ { 0x00000000, 0x38683870, 0xd69ccc7c, 0x00000000 },
    /*   "'" */ { 0x00000030, 0x10100000, 0x00000000, 0x00000000 },
    /*   '(' */ { 0x00000000, 0x18102020, 0x20202010, 0x18040000 },
    /*   ')' */ { 0x00000000, 0x60101818, 0x08181810, 0x60400000 },
    /*   '*' */ { 0x00000000, 0x105c2838, 0x08000000, 0x00000000 },
    /*   '+' */ { 0x00000000, 0x00001010, 0x10fc1010, 0x00000000 },
    /*   ',' */ { 0x00000000, 0x00000000, 0x00001030, 0x10000000 },
    /*   '-' */ { 0x00000000, 0x00000000, 0x7c000000, 0x00000000 },
    /*   '.' */ { 0x00000000, 0x00000000, 0x00001030, 0x00000000 },
    /*   '/' */ { 0x00000004, 0x0c081810, 0x30202040, 0x40000000 },
    /*   '0' */ { 0x00000000, 0x384c4444, 0x44444c38, 0x00000000 },
    /*   '1' */ { 0x00000000, 0x30101010, 0x1010107c, 0x00000000 },
    /*   '2' */ { 0x00000000, 0x78080c08, 0x1020607c, 0x00000000 },
    /*   '3' */ { 0x00000000, 0x78080838, 0x080c0c78, 0x00000000 },
    /*   '4' */ { 0x00000000, 0x18182868, 0x487c0808, 0x00000000 },
    /*   '5' */ { 0x00000000, 0x78606030, 0x080c0878, 0x00000000 },
    /*   '6' */ { 0x00000000, 0x3c604078, 0x6c446438, 0x00000000 },
    /*   '7' */ { 0x00000000, 0x7c0c0818, 0x10302060, 0x00000000 },
    /*   '8' */ { 0x00000000, 0x384c6838, 0x48444438, 0x00000000 },
    /*   '9' */ { 0x00000000, 0x384c444c, 0x3c0c0878, 0x00000000 },
    /*   ':' */ { 0x00000000, 0x00003010, 0x00001030, 0x00000000 },
    /*   ';' */ { 0x00000000, 0x00003010, 0x00001030, 0x10000000 },
    /*   '<' */ { 0x00000000, 0x0000041c, 0x70601804, 0x00000000 },
    /*   '=' */ { 0x00000000, 0x00000000, 0xfc00fc00, 0x00000000 },
    /*   '>' */ { 0x00000000, 0x00000060, 0x181c70c0, 0x00000000 },
    /*   '?' */ { 0x00000000, 0x784c0c18, 0x10200030, 0x00000000 },
    /*   '@' */ { 0x00000000, 0x385c54a4, 0xacd64038, 0x00000000 },
    /*   'A' */ { 0x00000000, 0x30303868, 0x487cc486, 0x00000000 },
    /*   'B' */ { 0x00000000, 0x784c4878, 0x4c444c78, 0x00000000 },
    /*   'C' */ { 0x00000000, 0x3c604040, 0xc040603c, 0x00000000 },
    /*   'D' */ { 0x00000000, 0x784c4444, 0x44444c78, 0x00000000 },
    /*   'E' */ { 0x00000000, 0x7c404078, 0x4040407c, 0x00000000 },
    /*   'F' */ { 0x00000000, 0x7c606060, 0x7c606060, 0x00000000 },
    /*   'G' */ { 0x00000000, 0x3c6040c0, 0xc444643c, 0x00000000 },
    /*   'H' */ { 0x00000000, 0x4444447c, 0x44444444, 0x00000000 },
    /*   'I' */ { 0x00000000, 0x7c101010, 0x1010107c, 0x00000000 },
    /*   'J' */ { 0x00000000, 0x38080808, 0x08080870, 0x00000000 },
    /*   'K' */ { 0x00000000, 0x4c485070, 0x70584844, 0x00000000 },
    /*   'L' */ { 0x00000000, 0x40404040, 0x4040407c, 0x00000000 },
    /*   'M' */ { 0x00000000, 0xc4ccecac, 0xb4948484, 0x00000000 },
    /*   'N' */ { 0x00000000, 0x44646474, 0x545c4c4c, 0x00000000 },
    /*   'O' */ { 0x00000000, 0x384cc4c4, 0xc4c44c38, 0x00000000 },
    /*   'P' */ { 0x00000000, 0x7c44444c, 0x78404040, 0x00000000 },
    /*   'Q' */ { 0x00000000, 0x384cc4c4, 0xc4c44c38, 0x0c000000 },
    /*   'R' */ { 0x00000000, 0x784c4c48, 0x70584c44, 0x00000000 },
    /*   'S' */ { 0x00000000, 0x38404030, 0x1c040478, 0x00000000 },
    /*   'T' */ { 0x00000000, 0xfe101010, 0x10101010, 0x00000000 },
    /*   'U' */ { 0x00000000, 0x44444444, 0x44444c38, 0x00000000 },
    /*   'V' */ { 0x00000000, 0x84c44448, 0x68283030, 0x00000000 },
    /*   'W' */ { 0x00000000, 0x828494f4, 0x746c6c6c, 0x00000000 },
    /*   'X' */ { 0x00000000, 0xc46c3830, 0x30284cc4, 0x00000000 },
    /*   'Y' */ { 0x00000000, 0xc4446838, 0x10101010, 0x00000000 },
    /*   'Z' */ { 0x00000000, 0x7c0c0810, 0x302040fc, 0x00000000 },
    /*   '[' */ { 0x0000003c, 0x20202020, 0x20202020, 0x3c000000 },
    /*  '\\' */ { 0x00000040, 0x40202030, 0x1018080c, 0x04000000 },
    /*   ']' */ { 0x00000070, 0x10101010, 0x10101010, 0x70000000 },
    /*   '^' */ { 0x00000000, 0x10302868, 0x4c440000, 0x00000000 },
    /*   '_' */ { 0x00000000, 0x00000000, 0x00000000, 0xfe000000 },
    /*   '`' */ { 0x00000000, 0x10000000, 0x00000000, 0x00000000 },
    /*   'a' */ { 0x00000000, 0x00007808, 0x3c4c4c7c, 0x00000000 },
    /*   'b' */ { 0x00000040, 0x4040786c, 0x44444c78, 0x00000000 },
    /*   'c' */ { 0x00000000, 0x00003c60, 0x4040603c, 0x00000000 },
    /*   'd' */ { 0x00000004, 0x04043c4c, 0x44444c7c, 0x00000000 },
    /*   'e' */ { 0x00000000, 0x0000384c, 0x7c40603c, 0x00000000 },
    /*   'f' */ { 0x0000001c, 0x30307c30, 0x30303030, 0x00000000 },
    /*   'g' */ { 0x00000000, 0x00003c4c, 0x44444c7c, 0x0c780000 },
    /*   'h' */ { 0x00000040, 0x4040586c, 0x4c4c4c4c, 0x00000000 },
    /*   'i' */ { 0x00000010, 0x00007010, 0x10101010, 0x00000000 },
    /*   'j' */ { 0x00000018, 0x00007818, 0x18181818, 0x18700000 },
    /*   'k' */ { 0x00000040, 0x40404858, 0x7070484c, 0x00000000 },
    /*   'l' */ { 0x00000070, 0x10101010, 0x10101010, 0x00000000 },
    /*   'm' */ { 0x00000000, 0x0000fcd4, 0xd4d4d4d4, 0x00000000 },
    /*   'n' */ { 0x00000000, 0x0000586c, 0x4c4c4c4c, 0x00000000 },
    /*   'o' */ { 0x00000000, 0x0000384c, 0x44444c38, 0x00000000 },
    /*   'p' */ { 0x00000000, 0x00007864, 0x44444c78, 0x40400000 },
    /*   'q' */ { 0x00000000, 0x00003c4c, 0x44444c7c, 0x04040000 },
    /*   'r' */ { 0x00000000, 0x00003c24, 0x20202020, 0x00000000 },
    /*   's' */ { 0x00000000, 0x00003840, 0x701c0c78, 0x00000000 },
    /*   't' */ { 0x00000000, 0x00207c20, 0x2020201c, 0x00000000 },
    /*   'u' */ { 0x00000000, 0x00004c4c, 0x4c4c4c7c, 0x00000000 },
    /*   'v' */ { 0x00000000, 0x0000c444, 0x68283830, 0x00000000 },
    /*   'w' */ { 0x00000000, 0x000092b4, 0xf46c6c4c, 0x00000000 },
    /*   'x' */ { 0x00000000, 0x00004c68, 0x3030684c, 0x00000000 },
    /*   'y' */ { 0x00000000, 0x00004444, 0x68283010, 0x30600000 },
    /*   'z' */ { 0x00000000, 0x00007c08, 0x1030607c, 0x00000000 },
    /*   '{' */ { 0x0000001c, 0x10101010, 0x60101010, 0x1c000000 },
    /*   '|' */ { 0x00000010, 0x10101010, 0x10101010, 0x10000000 },
    /*   '}' */ { 0x00000060, 0x10101010, 0x18101010, 0x70000000 },
    /*   '~' */ { 0x00000000, 0x00000000, 0x741c0000, 0x00000000 },
};

#define MAX_WINDOWS 16

/* === Per-window state in normal C globals ============================= */

/* window_id 0 is "invalid"; valid ids are 1..MAX_WINDOWS, mapped
 * to the [id-1] entry of these arrays. */
static int    window_type[MAX_WINDOWS];        /* WIN_TYPE_* (0 = free) */
static int    window_subscribe_op[MAX_WINDOWS]; /* notify_op (0 = none) */
static int    window_cur_col[MAX_WINDOWS];      /* cursor col within cell grid */
static int    window_cur_row[MAX_WINDOWS];      /* cursor row within cell grid */
static unsigned char window_vec_color[MAX_WINDOWS]; /* current pen palette idx */

/* Phase 60 step 11 — per-wid screen position + z-order.  The window
 * FB at WM_WINDOW_FB_BASE+(wid-1)*8 composites onto the screen FB
 * starting at (window_pos_x[wid-1], window_pos_y[wid-1]).  Z-order
 * is a simple ordered list: window_z[0] = bottommost, [count-1] =
 * topmost.  New windows go on top; destroy compacts in place. */
static int           window_pos_x[MAX_WINDOWS];
static int           window_pos_y[MAX_WINDOWS];
static int           window_z[MAX_WINDOWS];
static int           window_z_count;

/* The wid most recently passed to set_active_window.  composite_-
 * window_region reads this to compute the affected screen rect — see
 * the trick described above WM_ACTIVE_FB_SLOT_OFFSET.  Painting
 * helpers that don't take a wid arg (flush_strip, fill_rect_window,
 * fb_scroll_up_one_cell, blit_title_text) inherit it transparently. */
static int           active_wid;

/* Phase 60 step 8 — title bar text storage.  Single buffer for v1
 * (one title overwrites another at wm_set_title time, regardless of
 * which window receives it); per-wid title strings would need a
 * (16 × MAX_TITLE_LEN) array — punt until programs actually want
 * distinct titles per window.  Lives in the data segment so the WM
 * can pass O3 = boot data (O15) to ObjBlitGlyphs without copying to
 * the stack.  Initialised empty; wm_set_title overwrites + repaints. */
static unsigned char window_title[MAX_TITLE_LEN];
static int           window_title_len;

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

/* === Per-instance terminal-index parameterisation ====================
 *
 * Phase 59 / WM γ.15 — multi-WM, one instance per terminal.  Boot
 * arg `--init-r4 N+1` (passed by oriscrun via `init-r4=N+1` in the
 * --cpu spec) sets _orisc_init_r4 to N+1; libc task_init decodes
 * that to my_terminal_idx = N.  We read it back here and compose
 * /sys/term/<N>/* and /sys/wm/<N>/0 paths instead of the legacy
 * hardcoded "/0" paths.  init_r4 == 0 (no boot arg) keeps
 * my_terminal_idx == -1 → we default to N=0, preserving single-WM
 * configurations and pre-multi-WM tests verbatim. */

static int my_term_idx;
static char path_console[40];        /* /sys/term/<N>/console */
static char path_keyboard[40];
/* No path_framebuffer — Phase 60 step 2 dropped the
 * /sys/term/<N>/framebuffer walk in favour of local allocation
 * via ObjAllocFramebuffer.  oriscterm still publishes the path
 * for fb_smoke / test_framebuffer.sh back-compat, but oriscwm
 * doesn't walk it. */
static char path_pointer[40];
static char path_self_register[40];  /* /sys/wm/<N>/0 */

static int
wm_append_decimal(int n, char *buf, int p)
{
	if (n >= 100) {
		buf[p++] = '0' + (n / 100);
		buf[p++] = '0' + ((n / 10) % 10);
		buf[p++] = '0' + (n % 10);
	} else if (n >= 10) {
		buf[p++] = '0' + (n / 10);
		buf[p++] = '0' + (n % 10);
	} else {
		buf[p++] = '0' + n;
	}
	return p;
}

static int
wm_append_str(const char *s, char *buf, int p)
{
	int i;
	for (i = 0; s[i]; i++) buf[p++] = s[i];
	return p;
}

static void
init_per_term_paths(void)
{
	int idx = task_my_terminal_idx();
	if (idx < 0) idx = 0;
	my_term_idx = idx;

	int p;

	p = wm_append_str("/sys/term/", path_console, 0);
	p = wm_append_decimal(idx, path_console, p);
	p = wm_append_str("/console", path_console, p);
	path_console[p] = '\0';

	p = wm_append_str("/sys/term/", path_keyboard, 0);
	p = wm_append_decimal(idx, path_keyboard, p);
	p = wm_append_str("/keyboard", path_keyboard, p);
	path_keyboard[p] = '\0';

	p = wm_append_str("/sys/term/", path_pointer, 0);
	p = wm_append_decimal(idx, path_pointer, p);
	p = wm_append_str("/pointer", path_pointer, p);
	path_pointer[p] = '\0';

	p = wm_append_str("/sys/wm/", path_self_register, 0);
	p = wm_append_decimal(idx, path_self_register, p);
	p = wm_append_str("/0", path_self_register, p);
	path_self_register[p] = '\0';
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
	int rc = dir_walk(path_console, &kind, rem, sizeof(rem));
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

/* Phase 60 step 3 — local input sinks.  Each of these allocates a
 * TAG_INPUT_SINK object via the simorisc #0x10B primitive; the host
 * display worker (--display tk) translates Tk events into
 * SEND_DELIVER packets and appends them to the sink's queue.  The
 * WM polls the queue from its main loop and forwards events on to
 * subscribers — replaces the prior walk + subscribe-to-/sys/term/-
 * <N>/pointer dance for pointer, and the prior walk + WSURF_KEYBOARD
 * passthrough for keyboard.  Two wrappers (instead of one
 * parameterised helper) because pcc-orisc rejects computed-offset
 * OREFST and each caller bakes its destination slot in via the "i"
 * asm constraint. */
static int
alloc_local_keyboard_sink(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, 0\n"            /* kind = 0 (keyboard) */
		"addiu r5, r0, 31\n"           /* caps R|W|S|V|C — CAP_V needed for RQP */
		"call  #0x10B\n"
		"nop\n"
		"orefst o1, %1(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(WM_KBD_EVENTS_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

static int
alloc_local_pointer_sink(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, 1\n"            /* kind = 1 (pointer) */
		"addiu r5, r0, 31\n"           /* caps R|W|S|V|C — CAP_V needed for RQP */
		"call  #0x10B\n"
		"nop\n"
		"orefst o1, %1(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(WM_PTR_EVENTS_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

/* Phase 60 step 2 — allocate the framebuffer LOCALLY via the new
 * #0x102 ObjAllocFramebuffer primitive.  The resulting TAG_FRAMEBUFFER
 * object is backed by host memory in our own simorisc process; when
 * the parent ran us with `--display tk`, the host worker mirrors
 * stores into a Tk window with no wire RTT.  Replaces the prior
 * walk_framebuffer_to_slot which dir_walk'd /sys/term/<N>/framebuffer
 * to get a remote object on oriscterm — every glyph / line / blit
 * paid a wire RTT through that path; γ.10 papered over the per-paint
 * cost in oriscterm but the per-write trip was structural. */
static int
alloc_local_framebuffer(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, %1\n"           /* width = FB_W */
		"addiu r5, r0, %2\n"           /* height = FB_H */
		"addiu r6, r0, 3\n"            /* CAP_R | CAP_W */
		"addiu r7, r0, 0\n"            /* flags = 0 (mirror to display) */
		"call  #0x102\n"               /* ObjAllocFramebuffer */
		"nop\n"
		"orefst o1, %3(o12)\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(FB_W), "i"(FB_H),
		  "i"(WM_SURF_FRAMEBUFFER_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	return status;
}

/* Phase 60 step 11 — per-wid window FB slot dispatch.  Mirrors the
 * load_console_to_o1 / stash_console_o1 pattern: pcc-orisc rejects
 * computed-offset OREFLD/OREFST so each wid has its own case with a
 * baked-in offset (WM_WINDOW_FB_BASE_OFFSET + (wid-1)*8). */
static void
load_window_fb_to_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 1176(o12)"); break;
	case  2: asm volatile("orefld o1, 1184(o12)"); break;
	case  3: asm volatile("orefld o1, 1192(o12)"); break;
	case  4: asm volatile("orefld o1, 1200(o12)"); break;
	case  5: asm volatile("orefld o1, 1208(o12)"); break;
	case  6: asm volatile("orefld o1, 1216(o12)"); break;
	case  7: asm volatile("orefld o1, 1224(o12)"); break;
	case  8: asm volatile("orefld o1, 1232(o12)"); break;
	case  9: asm volatile("orefld o1, 1240(o12)"); break;
	case 10: asm volatile("orefld o1, 1248(o12)"); break;
	case 11: asm volatile("orefld o1, 1256(o12)"); break;
	case 12: asm volatile("orefld o1, 1264(o12)"); break;
	case 13: asm volatile("orefld o1, 1272(o12)"); break;
	case 14: asm volatile("orefld o1, 1280(o12)"); break;
	case 15: asm volatile("orefld o1, 1288(o12)"); break;
	case 16: asm volatile("orefld o1, 1296(o12)"); break;
	default: asm volatile("onull o1"); break;
	}
}

static void
stash_window_fb_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o1, 1176(o12)"); break;
	case  2: asm volatile("orefst o1, 1184(o12)"); break;
	case  3: asm volatile("orefst o1, 1192(o12)"); break;
	case  4: asm volatile("orefst o1, 1200(o12)"); break;
	case  5: asm volatile("orefst o1, 1208(o12)"); break;
	case  6: asm volatile("orefst o1, 1216(o12)"); break;
	case  7: asm volatile("orefst o1, 1224(o12)"); break;
	case  8: asm volatile("orefst o1, 1232(o12)"); break;
	case  9: asm volatile("orefst o1, 1240(o12)"); break;
	case 10: asm volatile("orefst o1, 1248(o12)"); break;
	case 11: asm volatile("orefst o1, 1256(o12)"); break;
	case 12: asm volatile("orefst o1, 1264(o12)"); break;
	case 13: asm volatile("orefst o1, 1272(o12)"); break;
	case 14: asm volatile("orefst o1, 1280(o12)"); break;
	case 15: asm volatile("orefst o1, 1288(o12)"); break;
	case 16: asm volatile("orefst o1, 1296(o12)"); break;
	default: break;
	}
}

/* Install wid's window FB ref into WM_ACTIVE_FB_SLOT so the painting
 * helpers (which target that slot) operate on the right window.
 * Also updates the active_wid global so composite_window_region can
 * compute the correct screen position. */
static void
set_active_window(int wid)
{
	active_wid = wid;
	load_window_fb_to_o1(wid);
	asm volatile("orefst o1, %0(o12)"
	             :: "i"(WM_ACTIVE_FB_SLOT_OFFSET));
}

/* Phase 60 step 7 — allocate the offscreen backing store the WM
 * renders this window's content into.  Sized to the usable cell area
 * (USABLE_W_PX × USABLE_H_PX) so render coords land at (col*8,
 * row*16) inside the buffer with no border offset.  The
 * FB_FLAG_OFFSCREEN flag tells simorisc to skip Tk display
 * registration — we composite onto the screen FB rather than
 * mirroring directly.
 *
 * Post-alloc the storage is zeroed (palette index 0 = bg) which is
 * what we want for the cleared inner area — no explicit fill needed.
 *
 * Phase 60 step 11 — stashes into the per-wid slot AND the
 * WM_ACTIVE_FB_SLOT (so paint_title_bar / chrome paint right after
 * alloc target the freshly-created FB). */
static int
alloc_window_fb(int wid)
{
	int status;
	asm volatile(
		"addiu r4, r0, %1\n"           /* width = USABLE_W_PX */
		"addiu r5, r0, %2\n"           /* height = USABLE_H_PX */
		"addiu r6, r0, 3\n"            /* CAP_R | CAP_W */
		"addiu r7, r0, 1\n"            /* FB_FLAG_OFFSCREEN */
		"call  #0x102\n"               /* ObjAllocFramebuffer */
		"nop\n"
		"orefst o1, %3(o12)\n"         /* stash into ACTIVE */
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(USABLE_W_PX), "i"(USABLE_H_PX),
		  "i"(WM_ACTIVE_FB_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	if (status != 0) return status;
	/* Mirror into the per-wid slot.  ACTIVE still points at it; both
	 * slots hold the same ref. */
	asm volatile("orefld o1, %0(o12)"
	             :: "i"(WM_ACTIVE_FB_SLOT_OFFSET) : "r1");
	stash_window_fb_o1(wid);
	return 0;
}

static void
free_window_fb(int wid)
{
	load_window_fb_to_o1(wid);
	int isn;
	asm volatile("oisn %0, o1" : "=r"(isn));
	if (isn) return;
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x101\n"               /* ObjFree */
		"nop"
		:
		:
		: "r1", "r2", "r4"
	);
	asm volatile("onull o1");
	stash_window_fb_o1(wid);
}

/* Phase 60 step 11 — composite a sub-rectangle of the active
 * window's backing store onto the screen FB, respecting z-order.
 * Coords (wx, wy) + (w, h) are in window-local pixel space relative
 * to the active window (set via set_active_window).  The dirty
 * region on the screen is computed as
 *   (active.pos_x + wx, active.pos_y + wy, w, h)
 * and then redrawn by walking the z-stack from bottom to top: each
 * window's intersection with the dirty rect is blitted from that
 * window's FB.  Higher-z windows therefore correctly overpaint
 * lower-z ones, which is what overlapping opaque windows need.
 *
 * Side effect: WM_ACTIVE_FB_SLOT is clobbered during the walk
 * (each iteration loads a different window's FB into it).  All paint
 * helpers call set_active_window before painting anyway, so this
 * only matters in code that paints AFTER calling
 * composite_window_region — and there isn't any: composite is always
 * the last step of a forward_* / paint_* / flush_strip request. */
/* 3-arg packed signature dodges pcc-orisc's 4-arg call ceiling.
 * Caller packs (x:high16, y:low16) for src/dst and (w:high16,
 * h:low16) for size. */
static void
do_blit_copy_active_to_screen(int packed_src_xy, int packed_dst_xy,
                              int packed_wh)
{
	asm volatile(
		"addu   r8,  %0, r0\n"
		"addu   r9,  %1, r0\n"
		"addu   r10, %2, r0\n"
		"orefld o1, %3(o12)\n"          /* O1 = screen FB (dst) */
		"orefld o2, %4(o12)\n"          /* O2 = ACTIVE = source */
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"call   #0x10F\n"               /* ObjBlitCopy */
		"nop"
		:
		: "r"(packed_src_xy), "r"(packed_dst_xy), "r"(packed_wh),
		  "i"(WM_SURF_FRAMEBUFFER_SLOT_OFFSET),
		  "i"(WM_ACTIVE_FB_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6",
		  "r8", "r9", "r10"
	);
}

static void
composite_window_region(int wx, int wy, int w, int h)
{
	int wid = active_wid;
	if (wid < 1 || wid > MAX_WINDOWS) return;
	int sx  = window_pos_x[wid - 1] + wx;
	int sy  = window_pos_y[wid - 1] + wy;
	int sxe = sx + w;
	int sye = sy + h;
	int z;
	for (z = 0; z < window_z_count; z++) {
		int t = window_z[z];
		if (t < 1 || t > MAX_WINDOWS) continue;
		int tx  = window_pos_x[t - 1];
		int ty  = window_pos_y[t - 1];
		int txe = tx + USABLE_W_PX;
		int tye = ty + USABLE_H_PX;
		int ix  = sx  > tx  ? sx  : tx;
		int iy  = sy  > ty  ? sy  : ty;
		int ixe = sxe < txe ? sxe : txe;
		int iye = sye < tye ? sye : tye;
		if (ixe <= ix || iye <= iy) continue;
		/* Load t's FB into ACTIVE so do_blit_copy reads from it. */
		load_window_fb_to_o1(t);
		asm volatile("orefst o1, %0(o12)"
		             :: "i"(WM_ACTIVE_FB_SLOT_OFFSET));
		int srcx = ix - tx, srcy = iy - ty;
		int rw = ixe - ix, rh = iye - iy;
		int packed_src = ((srcx & 0xFFFF) << 16) | (srcy & 0xFFFF);
		int packed_dst = ((ix & 0xFFFF) << 16) | (iy & 0xFFFF);
		int packed_wh  = ((rw & 0xFFFF) << 16) | (rh & 0xFFFF);
		do_blit_copy_active_to_screen(packed_src, packed_dst, packed_wh);
	}
}

/* Composite the entire window backing store onto the screen FB.
 * Used after operations whose dirty rect is awkward to track (full-
 * window scroll, future vector / raster ops once they migrate to
 * the window FB).  Costs one bytearray copy of USABLE_W_PX ×
 * USABLE_H_PX = ~930KB ≈ 5ms — bearable at human typing rates. */
static void
composite_whole_window(void)
{
	composite_window_region(0, 0, USABLE_W_PX, USABLE_H_PX);
}

/* Phase 60 step 10 — composite the cell-content area only, leaving
 * the title bar pixels on the screen FB untouched.  Used by the
 * vector / raster forwarders, which paint into the window FB at
 * client-visible (0..USABLE_W_PX × 0..CELL_CONTENT_PX) coords; they
 * call this once per request rather than once per inner blit. */
static void
composite_content_area(void)
{
	composite_window_region(0, TITLE_BAR_PX,
	                        USABLE_W_PX, CELL_CONTENT_PX);
}

/* fill_rect_packed targets the screen FB (paint_window_chrome's
 * one caller).  fill_rect_window targets the window backing store —
 * same wire shape, different OPR slot.  Duplicated rather than
 * parametrised because pcc-orisc requires the slot offset to be a
 * compile-time immediate inside the asm. */
static void
fill_rect_window(int packed_xy, int packed_wh, int color)
{
	if (((packed_wh >> 16) & 0xFFFF) == 0) return;
	if ((packed_wh & 0xFFFF) == 0)         return;
	asm volatile(
		"addu   r8,  %0, r0\n"
		"addu   r9,  %1, r0\n"
		"addu   r10, %2, r0\n"
		"orefld o1, %3(o12)\n"
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"call   #0x10D\n"           /* ObjFillRect */
		"nop"
		:
		: "r"(packed_xy), "r"(packed_wh), "r"(color),
		  "i"(WM_ACTIVE_FB_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6",
		  "r8", "r9", "r10"
	);
}

/* Render `n` chars of `window_title` into the window FB's title-bar
 * region.  The title is centred horizontally; if n exceeds N_COLS,
 * extra chars truncate.  Source ref is the WM's boot data O15 since
 * window_title[] lives in our data segment.
 *
 * pcc-orisc input-clobber dance same as flush_strip. */
static void
blit_title_text(int start_col, int n_chars)
{
	if (n_chars <= 0) return;
	int text_off = (int)((unsigned int)window_title - DATA_VA);
	int font_off = (int)((unsigned int)&font_8x16[0][0] - DATA_VA);
	int packed_xy = ((start_col & 0xFFFF) << 16) | (0 & 0xFFFF);
	int packed_shape = ((n_chars & 0xFFFF) << 16)
	                 | ((WM_TITLE_BAR_FG & 0xFF) << 8)
	                 | (WM_TITLE_BAR_BG & 0xFF);
	asm volatile(
		"addu   r8,  %0, r0\n"
		"addu   r9,  %1, r0\n"
		"addu   r10, %2, r0\n"
		"addu   r11, %3, r0\n"
		"orefld o1, %4(o12)\n"      /* O1 = window FB */
		"omov   o2, o15\n"          /* O2 = boot data (font) */
		"omov   o3, o15\n"          /* O3 = boot data (title) */
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"addu   r7, r11, r0\n"
		"call   #0x10C\n"           /* ObjBlitGlyphs */
		"nop"
		:
		: "r"(packed_xy), "r"(packed_shape),
		  "r"(font_off),  "r"(text_off),
		  "i"(WM_ACTIVE_FB_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r7",
		  "r8", "r9", "r10", "r11"
	);
}

/* Phase 60 step 8 — paint the title bar at the top of the window FB.
 * Bar bg = WM_TITLE_BAR_BG (inverse video — light gray); title text
 * (centred) renders in WM_TITLE_BAR_FG (dark navy).  After painting
 * the window-local title bar region we composite it onto the screen.
 *
 * Called once at handle_new_window time (empty title — visible
 * bar) and after every wm_set_title to refresh the displayed
 * text. */
static void
paint_title_bar(void)
{
	int bar_xy = ((0 & 0xFFFF) << 16) | (0 & 0xFFFF);
	int bar_wh = ((USABLE_W_PX & 0xFFFF) << 16) | (TITLE_BAR_PX & 0xFFFF);
	fill_rect_window(bar_xy, bar_wh, WM_TITLE_BAR_BG);

	int n = window_title_len;
	if (n > N_COLS) n = N_COLS;
	if (n > 0) {
		int start_col = (N_COLS - n) / 2;
		blit_title_text(start_col, n);
	}

	composite_window_region(0, 0, USABLE_W_PX, TITLE_BAR_PX);
}

/* Phase 60 step 5 — fill a rectangle in the framebuffer with a single
 * palette index via the #0x10D ObjFillRect firmware op.  Used to paint
 * the bg backdrop and the four border lines at FB-init time.
 *
 * 3-arg signature (packed_xy, packed_wh, color) sidesteps pcc-orisc's
 * 5-arg call-codegen bug — the WM works around it elsewhere by
 * packing arg pairs into single ints (see flush_strip's packed_xy /
 * packed_shape).  Caller packs (x:high16, y:low16) and (w:high16,
 * h:low16).
 *
 * pcc-orisc input-clobber dance: copy the three "r" inputs to safe
 * temps before the body's first store of r4..r6 (same hygiene
 * flush_strip uses for ObjBlitGlyphs). */
static void
fill_rect_packed(int packed_xy, int packed_wh, int color)
{
	if (((packed_wh >> 16) & 0xFFFF) == 0) return;
	if ((packed_wh & 0xFFFF) == 0)         return;
	asm volatile(
		"addu   r8,  %0, r0\n"      /* save packed_xy */
		"addu   r9,  %1, r0\n"      /* save packed_wh */
		"addu   r10, %2, r0\n"      /* save color */
		"orefld o1, %3(o12)\n"      /* O1 = framebuffer */
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"call   #0x10D\n"           /* ObjFillRect */
		"nop"
		:
		: "r"(packed_xy), "r"(packed_wh), "r"(color),
		  "i"(WM_SURF_FRAMEBUFFER_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6",
		  "r8", "r9", "r10"
	);
}

/* Paint the window chrome: solid bg fill + a 2-pixel border line just
 * inside the outer cell ring.  Called once after alloc_local_framebuffer
 * so the FB never appears as a black void to the user — they see the
 * border before the first glyph lands.
 *
 * Border layout (BORDER_LINE_PX=2, BORDER_CELLS=1):
 *   top:    y ∈ [CELL_ORIGIN_Y - BORDER_LINE_PX, CELL_ORIGIN_Y)
 *   bottom: y ∈ [CELL_ORIGIN_Y + USABLE_H_PX, +BORDER_LINE_PX)
 *   left:   x ∈ [CELL_ORIGIN_X - BORDER_LINE_PX, CELL_ORIGIN_X)
 *   right:  x ∈ [CELL_ORIGIN_X + USABLE_W_PX, +BORDER_LINE_PX)
 * Lines extend across the full inner-cell-area width/height so the
 * four corners overlap and form a clean rectangle. */
static void
paint_window_chrome(void)
{
	/* Background. */
	fill_rect_packed(((0 & 0xFFFF) << 16) | (0 & 0xFFFF),
	                 ((FB_W & 0xFFFF) << 16) | (FB_H & 0xFFFF),
	                 WM_BG_COLOR);

	int top_y    = CELL_ORIGIN_Y - BORDER_LINE_PX;
	int bottom_y = CELL_ORIGIN_Y + USABLE_H_PX;
	int left_x   = CELL_ORIGIN_X - BORDER_LINE_PX;
	int right_x  = CELL_ORIGIN_X + USABLE_W_PX;
	int frame_w  = (right_x + BORDER_LINE_PX) - left_x;
	int frame_h  = (bottom_y + BORDER_LINE_PX) - top_y;
	int hline_wh = ((frame_w & 0xFFFF) << 16) | (BORDER_LINE_PX & 0xFFFF);
	int vline_wh = ((BORDER_LINE_PX & 0xFFFF) << 16) | (frame_h & 0xFFFF);

	fill_rect_packed(((left_x & 0xFFFF) << 16) | (top_y & 0xFFFF),
	                 hline_wh, WM_BORDER_COLOR);
	fill_rect_packed(((left_x & 0xFFFF) << 16) | (bottom_y & 0xFFFF),
	                 hline_wh, WM_BORDER_COLOR);
	fill_rect_packed(((left_x & 0xFFFF) << 16) | (top_y & 0xFFFF),
	                 vline_wh, WM_BORDER_COLOR);
	fill_rect_packed(((right_x & 0xFFFF) << 16) | (top_y & 0xFFFF),
	                 vline_wh, WM_BORDER_COLOR);
}

/* Phase 60 step 3 superseded subscribe_term_pointer (the
 * /sys/term/<N>/pointer subscribe) and walk_pointer_to_slot.  The
 * pointer events mailbox is now a TAG_INPUT_SINK allocated by
 * alloc_local_pointer_sink above; simorisc's display worker
 * enqueues Tk pointer events into it directly.  Kept the old
 * helper below dead-stripped via #if 0 to make the migration
 * easy to retrace later. */
#if 0
static int
walk_pointer_to_slot(void) { /* removed in Phase 60 step 3 */ }

static int
subscribe_term_pointer(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"
		"addiu r6, r0, %2\n"
		"call  #0x100\n"                  /* ObjAlloc → O1 */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	asm volatile("orefst o1, %0(o12)"
	             :: "i"(WM_PTR_EVENTS_SLOT_OFFSET));

	asm volatile(
		"addiu r4, r0, 64\n"
		"call  #0x203\n"                  /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	if (status != 0) return status;

	/* Derive R|S sub-cap and SEND it to /sys/term/0/pointer.  Wire:
	 * O2 = sub-cap, R4..R7 = 0 (subscribe). */
	asm volatile(
		"orefld o1, %0(o12)\n"
		"addiu  r4, r0, %2\n"             /* R|S */
		"call   #0x103\n"                 /* ObjDerive → O1 = sub-cap */
		"nop\n"
		"omov   o2, o1\n"
		"orefld o1, %1(o12)\n"            /* O1 = /sys/term/0/pointer */
		"onull  o3\n"
		"addiu  r4, r0, 0\n"
		"addiu  r5, r0, 0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1"
		:
		: "i"(WM_PTR_EVENTS_SLOT_OFFSET),
		  "i"(WM_SURF_POINTER_SLOT_OFFSET),
		  "i"(CAP_R | CAP_S)
		: "r1", "r2", "r4", "r5", "r6", "r7"
	);
	return 0;
}
#endif  /* end pre-step-3 helpers */

/* Allocate the WM-side pointer SERVICE object — clients SEND to a
 * derived R|S sub-cap of this when they subscribe via
 * wm_bind_surface(WSURF_POINTER).  ReceiveQueueAttach depth 16 is
 * generous: subscribe SENDs are rare. */
static int
alloc_pointer_service(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"
		"addiu r6, r0, %2\n"
		"call  #0x100\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	asm volatile("orefst o1, %0(o12)"
	             :: "i"(WM_POINTER_SVC_SLOT_OFFSET));

	asm volatile(
		"addiu r4, r0, 16\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Phase 60 step 3 — same shape as alloc_pointer_service for the
 * keyboard subscribe service.  Clients SEND subscribe to a derived
 * R|S sub-cap of WM_KEYBOARD_SVC_SLOT (returned by
 * wm_bind_surface(WSURF_KEYBOARD)). */
static int
alloc_keyboard_service(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"
		"addiu r6, r0, %2\n"
		"call  #0x100\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	asm volatile("orefst o1, %0(o12)"
	             :: "i"(WM_KEYBOARD_SVC_SLOT_OFFSET));

	asm volatile(
		"addiu r4, r0, 16\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* === Self-register at /sys/wm/<my_term>/0 ============================ */

/* dir_register publishes whatever's in O1 at the given path.  We
 * derive a R+S sub-cap of our mailbox first, then call dir_register.
 * Path is composed from my_term_idx by init_per_term_paths so each
 * WM instance lands at /sys/wm/<idx>/0; libc wm_init reads
 * task_my_terminal_idx() and walks the matching path. */
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

	register_status = dir_register(path_self_register);
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

/* === Per-window GRID service slot helpers =============================
 *
 * Same shape as the CONSOLE helpers above, just at WM_GRID_BASE.
 * Phase 59 / WM γ.9. */

static void
stash_grid_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o1, 720(o12)"); break;
	case  2: asm volatile("orefst o1, 728(o12)"); break;
	case  3: asm volatile("orefst o1, 736(o12)"); break;
	case  4: asm volatile("orefst o1, 744(o12)"); break;
	case  5: asm volatile("orefst o1, 752(o12)"); break;
	case  6: asm volatile("orefst o1, 760(o12)"); break;
	case  7: asm volatile("orefst o1, 768(o12)"); break;
	case  8: asm volatile("orefst o1, 776(o12)"); break;
	case  9: asm volatile("orefst o1, 784(o12)"); break;
	case 10: asm volatile("orefst o1, 792(o12)"); break;
	case 11: asm volatile("orefst o1, 800(o12)"); break;
	case 12: asm volatile("orefst o1, 808(o12)"); break;
	case 13: asm volatile("orefst o1, 816(o12)"); break;
	case 14: asm volatile("orefst o1, 824(o12)"); break;
	case 15: asm volatile("orefst o1, 832(o12)"); break;
	case 16: asm volatile("orefst o1, 840(o12)"); break;
	default: break;
	}
}

static void
load_grid_to_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 720(o12)"); break;
	case  2: asm volatile("orefld o1, 728(o12)"); break;
	case  3: asm volatile("orefld o1, 736(o12)"); break;
	case  4: asm volatile("orefld o1, 744(o12)"); break;
	case  5: asm volatile("orefld o1, 752(o12)"); break;
	case  6: asm volatile("orefld o1, 760(o12)"); break;
	case  7: asm volatile("orefld o1, 768(o12)"); break;
	case  8: asm volatile("orefld o1, 776(o12)"); break;
	case  9: asm volatile("orefld o1, 784(o12)"); break;
	case 10: asm volatile("orefld o1, 792(o12)"); break;
	case 11: asm volatile("orefld o1, 800(o12)"); break;
	case 12: asm volatile("orefld o1, 808(o12)"); break;
	case 13: asm volatile("orefld o1, 816(o12)"); break;
	case 14: asm volatile("orefld o1, 824(o12)"); break;
	case 15: asm volatile("orefld o1, 832(o12)"); break;
	case 16: asm volatile("orefld o1, 840(o12)"); break;
	default: asm volatile("onull o1"); break;
	}
}

/* === Per-window VECTOR service slot helpers ===========================
 *
 * Same shape as the GRID helpers above, just at WM_VECTOR_BASE
 * (848..968).  Phase 59 / WM γ.11. */

static void
stash_vector_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o1, 848(o12)"); break;
	case  2: asm volatile("orefst o1, 856(o12)"); break;
	case  3: asm volatile("orefst o1, 864(o12)"); break;
	case  4: asm volatile("orefst o1, 872(o12)"); break;
	case  5: asm volatile("orefst o1, 880(o12)"); break;
	case  6: asm volatile("orefst o1, 888(o12)"); break;
	case  7: asm volatile("orefst o1, 896(o12)"); break;
	case  8: asm volatile("orefst o1, 904(o12)"); break;
	case  9: asm volatile("orefst o1, 912(o12)"); break;
	case 10: asm volatile("orefst o1, 920(o12)"); break;
	case 11: asm volatile("orefst o1, 928(o12)"); break;
	case 12: asm volatile("orefst o1, 936(o12)"); break;
	case 13: asm volatile("orefst o1, 944(o12)"); break;
	case 14: asm volatile("orefst o1, 952(o12)"); break;
	case 15: asm volatile("orefst o1, 960(o12)"); break;
	case 16: asm volatile("orefst o1, 968(o12)"); break;
	default: break;
	}
}

static void
load_vector_to_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 848(o12)"); break;
	case  2: asm volatile("orefld o1, 856(o12)"); break;
	case  3: asm volatile("orefld o1, 864(o12)"); break;
	case  4: asm volatile("orefld o1, 872(o12)"); break;
	case  5: asm volatile("orefld o1, 880(o12)"); break;
	case  6: asm volatile("orefld o1, 888(o12)"); break;
	case  7: asm volatile("orefld o1, 896(o12)"); break;
	case  8: asm volatile("orefld o1, 904(o12)"); break;
	case  9: asm volatile("orefld o1, 912(o12)"); break;
	case 10: asm volatile("orefld o1, 920(o12)"); break;
	case 11: asm volatile("orefld o1, 928(o12)"); break;
	case 12: asm volatile("orefld o1, 936(o12)"); break;
	case 13: asm volatile("orefld o1, 944(o12)"); break;
	case 14: asm volatile("orefld o1, 952(o12)"); break;
	case 15: asm volatile("orefld o1, 960(o12)"); break;
	case 16: asm volatile("orefld o1, 968(o12)"); break;
	default: asm volatile("onull o1"); break;
	}
}

/* === Per-window RASTER service slot helpers ===========================
 *
 * Same shape as the GRID / VECTOR helpers above, just at
 * WM_RASTER_BASE (984..1104, in 8-byte stride per wid).  Phase 59 /
 * WM γ.12. */

static void
stash_raster_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o1, 984(o12)"); break;
	case  2: asm volatile("orefst o1, 992(o12)"); break;
	case  3: asm volatile("orefst o1, 1000(o12)"); break;
	case  4: asm volatile("orefst o1, 1008(o12)"); break;
	case  5: asm volatile("orefst o1, 1016(o12)"); break;
	case  6: asm volatile("orefst o1, 1024(o12)"); break;
	case  7: asm volatile("orefst o1, 1032(o12)"); break;
	case  8: asm volatile("orefst o1, 1040(o12)"); break;
	case  9: asm volatile("orefst o1, 1048(o12)"); break;
	case 10: asm volatile("orefst o1, 1056(o12)"); break;
	case 11: asm volatile("orefst o1, 1064(o12)"); break;
	case 12: asm volatile("orefst o1, 1072(o12)"); break;
	case 13: asm volatile("orefst o1, 1080(o12)"); break;
	case 14: asm volatile("orefst o1, 1088(o12)"); break;
	case 15: asm volatile("orefst o1, 1096(o12)"); break;
	case 16: asm volatile("orefst o1, 1104(o12)"); break;
	default: break;
	}
}

static void
load_raster_to_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 984(o12)"); break;
	case  2: asm volatile("orefld o1, 992(o12)"); break;
	case  3: asm volatile("orefld o1, 1000(o12)"); break;
	case  4: asm volatile("orefld o1, 1008(o12)"); break;
	case  5: asm volatile("orefld o1, 1016(o12)"); break;
	case  6: asm volatile("orefld o1, 1024(o12)"); break;
	case  7: asm volatile("orefld o1, 1032(o12)"); break;
	case  8: asm volatile("orefld o1, 1040(o12)"); break;
	case  9: asm volatile("orefld o1, 1048(o12)"); break;
	case 10: asm volatile("orefld o1, 1056(o12)"); break;
	case 11: asm volatile("orefld o1, 1064(o12)"); break;
	case 12: asm volatile("orefld o1, 1072(o12)"); break;
	case 13: asm volatile("orefld o1, 1080(o12)"); break;
	case 14: asm volatile("orefld o1, 1088(o12)"); break;
	case 15: asm volatile("orefld o1, 1096(o12)"); break;
	case 16: asm volatile("orefld o1, 1104(o12)"); break;
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

	/* Queue depth 256 — clients (shell.c's term_print_char) fire
	 * console SENDs with no reply_cap (fire-and-forget), so they
	 * don't block per-write.  At depth 8 we'd overflow under any
	 * sustained burst (e.g. shell echoing typed chars while the
	 * WM is busy in render_buffer's 16 wire RTTs per strip), and
	 * the simulator silently drops on overflow.  256 absorbs the
	 * largest burst we see in practice (a long term_print of
	 * help text) plus comfortable headroom. */
	asm volatile(
		"addiu r4, r0, 256\n"          /* depth = 256 */
		"call  #0x203\n"               /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Mirror of alloc_window_console for the GRID surface.  Same
 * lifetime: allocated alongside its CONSOLE peer in handle_new_window
 * and freed by free_window_grid in handle_destroy_window. */
static int
alloc_window_grid(int wid)
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

	stash_grid_o1(wid);

	/* Same depth-256 reasoning as the CONSOLE queue — grid SENDs
	 * are also fire-and-forget (term_grid_print etc.). */
	asm volatile(
		"addiu r4, r0, 256\n"
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
 * CONSOLE service and attaches a queue.  Phase 60 step 11: lifted
 * the N=1 CONSOLE restriction; multiple windows now coexist with
 * cascade positioning + z-order.  GRAPHICAL still returns E_NOTIMPL. */
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

	/* Same dance for GRID (Phase 59 / WM γ.9).  If GRID alloc fails
	 * we tear the CONSOLE allocation down too — clients see E_IO and
	 * retry, expecting a fresh window. */
	status = alloc_window_grid(wid);
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_window_grid failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		free_window_console(wid);
		wm_reply(E_IO, 0, 0, 0);
		return;
	}

	/* And again for VECTOR (Phase 59 / WM γ.11). */
	status = alloc_window_vector(wid);
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_window_vector failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		free_window_grid(wid);
		free_window_console(wid);
		wm_reply(E_IO, 0, 0, 0);
		return;
	}

	/* And RASTER (Phase 59 / WM γ.12). */
	status = alloc_window_raster(wid);
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_window_raster failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		free_window_vector(wid);
		free_window_grid(wid);
		free_window_console(wid);
		wm_reply(E_IO, 0, 0, 0);
		return;
	}

	/* Phase 60 step 11 — per-wid backing store.  CONSOLE / GRID /
	 * VECTOR / RASTER writes all paint into this offscreen FB; the
	 * WM ObjBlitCopy-composites the touched region (intersected with
	 * each window in z-order) onto the screen after every paint. */
	status = alloc_window_fb(wid);
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_window_fb failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		free_window_raster(wid);
		free_window_vector(wid);
		free_window_grid(wid);
		free_window_console(wid);
		wm_reply(E_IO, 0, 0, 0);
		return;
	}

	/* Phase 60 step 11 — assign default position.  The first window
	 * lands at the chrome-inset origin (CELL_ORIGIN_X, CELL_ORIGIN_Y)
	 * so the screen-FB chrome (border + bg margins) stays visible
	 * around it — matches the pre-multi-window single-window appearance.
	 * Subsequent windows cascade by CASCADE_OFFSET_PX wherever there's
	 * slack between FB size and window size; with today's nearly-
	 * fullscreen windows that slack is zero, so all cascaded windows
	 * stack at the same origin and z-order alone distinguishes them.
	 * When window resize lands the slack opens up and cascade
	 * activates naturally. */
#define CASCADE_OFFSET_PX 32
	{
		int idx = wid - 1;
		int n   = window_z_count;
		int slack_x = FB_W - USABLE_W_PX - 2 * CELL_ORIGIN_X;
		int slack_y = FB_H - USABLE_H_PX - 2 * CELL_ORIGIN_Y;
		if (slack_x < 0) slack_x = 0;
		if (slack_y < 0) slack_y = 0;
		int px = CELL_ORIGIN_X;
		int py = CELL_ORIGIN_Y;
		if (slack_x > 0) px += (n * CASCADE_OFFSET_PX) % (slack_x + 1);
		if (slack_y > 0) py += (n * CASCADE_OFFSET_PX) % (slack_y + 1);
		window_pos_x[idx] = px;
		window_pos_y[idx] = py;
	}

	/* Push to top of z-stack and mark active for the title-bar paint. */
	window_z[window_z_count] = wid;
	window_z_count += 1;
	set_active_window(wid);

	/* Phase 60 step 8 — paint the title bar so the window is visibly
	 * framed from creation.  Title is initially empty; the client
	 * (typically the shell or the spawning supervisor) wm_set_title's
	 * to fill it in. */
	window_title_len = 0;
	paint_title_bar();

	window_type[wid - 1] = WIN_TYPE_CONSOLE;
	window_subscribe_op[wid - 1] = 0;
	window_cur_col[wid - 1] = 0;
	window_cur_row[wid - 1] = 0;
	{
		int wvc_slot = wid - 1;   /* avoids `la sym+-1` codegen */
		window_vec_color[wvc_slot] = WM_FG_COLOR;
	}

	int geom_a = ((DEFAULT_W_PX & 0xFFFF) << 16) | (DEFAULT_H_PX & 0xFFFF);
	int geom_b = ((DEFAULT_W_CELLS & 0xFFFF) << 16) | (DEFAULT_H_CELLS & 0xFFFF);
	wm_reply(0, geom_a, geom_b, wid);
}

/* Phase 60 step 11 — z-stack remove.  Called from
 * handle_destroy_window and the auto-destroy scan.  O(N) compaction
 * over the live array; bounded since window_z_count <= MAX_WINDOWS. */
static void
window_z_remove(int wid)
{
	int i, found = -1;
	for (i = 0; i < window_z_count; i++) {
		if (window_z[i] == wid) { found = i; break; }
	}
	if (found < 0) return;
	for (i = found; i < window_z_count - 1; i++) {
		window_z[i] = window_z[i + 1];
	}
	window_z_count -= 1;
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

/* Mirror for the GRID service. */
static void
free_window_grid(int wid)
{
	load_grid_to_o1(wid);
	int isn;
	asm volatile("oisn %0, o1" : "=r"(isn));
	if (isn) return;
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x101\n"        /* ObjFree */
		"nop"
		: : : "r1", "r2", "r4"
	);
	asm volatile("onull o1");
	stash_grid_o1(wid);
}

/* Mirror of alloc_window_grid for the VECTOR surface (Phase 59 / WM
 * γ.11).  Same lifetime: allocated alongside its CONSOLE / GRID peers
 * in handle_new_window and freed by free_window_vector in
 * handle_destroy_window. */
static int
alloc_window_vector(int wid)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"
		"addiu r6, r0, %2\n"
		"call  #0x100\n"               /* ObjAlloc → O1 */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	stash_vector_o1(wid);

	/* Same depth-256 reasoning as the CONSOLE / GRID queues —
	 * vector SENDs are fire-and-forget; clients don't wait for an
	 * ack between vec_line / vec_rect_fill calls. */
	asm volatile(
		"addiu r4, r0, 256\n"
		"call  #0x203\n"               /* ReceiveQueueAttach */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Mirror for the VECTOR service. */
static void
free_window_vector(int wid)
{
	load_vector_to_o1(wid);
	int isn;
	asm volatile("oisn %0, o1" : "=r"(isn));
	if (isn) return;
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x101\n"
		"nop"
		: : : "r1", "r2", "r4"
	);
	asm volatile("onull o1");
	stash_vector_o1(wid);
}

/* Mirror of alloc_window_grid for the RASTER surface (Phase 59 / WM
 * γ.12).  Same lifetime as the CONSOLE / GRID / VECTOR peers. */
static int
alloc_window_raster(int wid)
{
	int status;
	asm volatile(
		"addiu r4, r0, 16\n"
		"addiu r5, r0, %1\n"
		"addiu r6, r0, %2\n"
		"call  #0x100\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(TAG_SERVICE),
		  "i"(CAP_R | CAP_W | CAP_S | CAP_V | CAP_C)
		: "r1", "r2", "r4", "r5", "r6"
	);
	if (status != 0) return status;

	stash_raster_o1(wid);

	asm volatile(
		"addiu r4, r0, 256\n"
		"call  #0x203\n"
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		:
		: "r1", "r2", "r3", "r4"
	);
	return status;
}

/* Mirror for the RASTER service. */
static void
free_window_raster(int wid)
{
	load_raster_to_o1(wid);
	int isn;
	asm volatile("oisn %0, o1" : "=r"(isn));
	if (isn) return;
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x101\n"
		"nop"
		: : : "r1", "r2", "r4"
	);
	asm volatile("onull o1");
	stash_raster_o1(wid);
}

/* WM_OP_BIND_SURFACE — return a surface cap for a window.
 *   R4 = wid  (already-validated by dispatch)
 *   R5 = surface kind
 *
 * Phase 58 + 59 / WM γ.9:
 *   - WSURF_CONSOLE returns an R|S sub-cap of the per-window
 *     CONSOLE service (the WM is in the data path; client SENDs
 *     land in the per-window queue and the WM forwards them to the
 *     underlying terminal AND glyph-renders them into the framebuffer).
 *   - WSURF_GRID returns an R|S sub-cap of the per-window GRID
 *     service.  Same shape as CONSOLE: client SENDs land in the
 *     per-window queue, the WM rasterises positioned text into the
 *     framebuffer.
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
	if (kind != WSURF_CONSOLE && kind != WSURF_KEYBOARD
	                          && kind != WSURF_GRID
	                          && kind != WSURF_VECTOR
	                          && kind != WSURF_RASTER
	                          && kind != WSURF_POINTER) {
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

	if (kind == WSURF_GRID) {
		load_grid_to_o1(wid);
		int derive_status;
		asm volatile(
			"addiu r4, r0, %1\n"
			"call  #0x103\n"
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

	if (kind == WSURF_VECTOR) {
		load_vector_to_o1(wid);
		int derive_status;
		asm volatile(
			"addiu r4, r0, %1\n"
			"call  #0x103\n"
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

	if (kind == WSURF_RASTER) {
		load_raster_to_o1(wid);
		int derive_status;
		asm volatile(
			"addiu r4, r0, %1\n"
			"call  #0x103\n"
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

	if (kind == WSURF_POINTER) {
		/* Single WM-wide pointer service (no per-window for v1).
		 * Derive R|S sub-cap of WM_POINTER_SVC_SLOT and return it. */
		int derive_status;
		asm volatile(
			"orefld o1, %1(o12)\n"
			"addiu  r4, r0, %2\n"
			"call   #0x103\n"
			"nop\n"
			"omov   o14, o1\n"
			"addu   %0, r2, r0"
			: "=r"(derive_status)
			: "i"(WM_POINTER_SVC_SLOT_OFFSET),
			  "i"(CAP_R | CAP_S)
			: "r1", "r2", "r4"
		);
		if (derive_status != 0) {
			wm_reply(E_IO, 0, 0, 0);
			return;
		}
		wm_reply_with_ref_o14(0);
		return;
	}

	/* WSURF_KEYBOARD: broker (Phase 60 step 3 — was passthrough to
	 * /sys/term/<N>/keyboard, but oriscterm is gone now).  Mirror
	 * of WSURF_POINTER above: derive R|S sub-cap of
	 * WM_KEYBOARD_SVC_SLOT, return it.  Clients SEND subscribe
	 * requests to the sub-cap; poll_keyboard_subscribes captures
	 * them; poll_keyboard_events forwards key events from the
	 * local TAG_INPUT_SINK queue. */
	if (kind == WSURF_KEYBOARD) {
		int derive_status;
		asm volatile(
			"orefld o1, %1(o12)\n"
			"addiu  r4, r0, %2\n"
			"call   #0x103\n"
			"nop\n"
			"omov   o14, o1\n"
			"addu   %0, r2, r0"
			: "=r"(derive_status)
			: "i"(WM_KEYBOARD_SVC_SLOT_OFFSET),
			  "i"(CAP_R | CAP_S)
			: "r1", "r2", "r4"
		);
		if (derive_status != 0) {
			wm_reply(E_IO, 0, 0, 0);
			return;
		}
		wm_reply_with_ref_o14(0);
		return;
	}

	/* No other surface kinds left (CONSOLE / GRID / VECTOR / RASTER /
	 * POINTER / KEYBOARD all handled above).  Should be unreachable. */
	wm_reply(E_INVAL, 0, 0, 0);
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
	free_window_grid(wid);
	free_window_vector(wid);
	free_window_raster(wid);
	free_window_fb(wid);
	window_z_remove(wid);
	window_type[wid - 1] = 0;
	window_subscribe_op[wid - 1] = 0;
	/* Owner-ref stash is left in place; future allocations will
	 * overwrite it.  No SEND fires for the close — the WM doesn't
	 * push events to subscribers yet.
	 * TODO: re-composite the screen so vacated pixels don't linger
	 * (currently the destroyed window's content stays painted on
	 * screen until something else paints over it). */
	wm_reply(0, 0, 0, 0);
}

/* Phase 60 step 8 — WM_OP_SET_TITLE.  Wire shape:
 *   R5 = wid (0 = first live window)
 *   R6 = packed (len:high16, src_off:low16)
 *   O2 = source ref containing the title bytes
 *
 * ObjFetchBytes the bytes into window_title[], repaint the title bar,
 * reply with status.  Same source-ref stash dance as
 * forward_console_write — pcc-orisc may spill R4..R6 to stack across
 * a function-call boundary but OPRs survive, so O2 is still the
 * caller's source ref at handler entry. */
static void
handle_set_title(int wid, int packed_len_off)
{
	if (wid == 0) {
		int i;
		for (i = 1; i <= MAX_WINDOWS; i++) {
			if (window_type[i - 1] != 0) { wid = i; break; }
		}
	}
	if (wid < 1 || wid > MAX_WINDOWS) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}
	if (window_type[wid - 1] == 0) {
		wm_reply(E_NOENT, 0, 0, 0);
		return;
	}

	int len     = (packed_len_off >> 16) & 0xFFFF;
	int src_off = packed_len_off & 0xFFFF;
	if (len < 0 || len > MAX_TITLE_LEN) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}

	/* Stash source ref before any subsequent asm clobbers O2. */
	asm volatile("orefst o2, %0(o12)"
	             :: "i"(WM_FORWARD_SRC_SLOT_OFFSET));

	if (len > 0) {
		/* The WM's boot data ref (O15) is allocated with caps R|C —
		 * no CAP_W at the ref level (its VA mapping is R|W, but
		 * ObjFetchBytes checks ref caps, not mapping perms).  So we
		 * fetch into a stack-local buffer first (boot stack O11 has
		 * CAP_W at the ref level), then memcpy through the data
		 * mapping's W bit into the persistent window_title.  Same
		 * trick forward_console_write uses. */
		unsigned char fetch_buf[MAX_TITLE_LEN];
		int dst_off = (int)((unsigned int)fetch_buf - STACK_BOTTOM);
		int fetch_status;
		asm volatile(
			"addu  r8, %1, r0\n"
			"addu  r9, %2, r0\n"
			"addu  r10, %3, r0\n"
			"orefld o1, %4(o12)\n"      /* O1 = caller's source */
			"omov   o2, o11\n"          /* O2 = boot stack (dest) */
			"addu  r4, r8,  r0\n"       /* src_off */
			"addu  r5, r9,  r0\n"       /* dst_off */
			"addu  r6, r10, r0\n"       /* count */
			"call  #0x108\n"            /* ObjFetchBytes */
			"nop\n"
			"addu  %0, r2, r0"
			: "=r"(fetch_status)
			: "r"(src_off), "r"(dst_off), "r"(len),
			  "i"(WM_FORWARD_SRC_SLOT_OFFSET)
			: "r1", "r2", "r3", "r4", "r5", "r6",
			  "r8", "r9", "r10"
		);
		if (fetch_status != 0) {
			wm_reply(E_IO, 0, 0, 0);
			return;
		}
		int i;
		for (i = 0; i < len; i++)
			window_title[i] = fetch_buf[i];
	}
	window_title_len = len;
	set_active_window(wid);
	paint_title_bar();
	wm_reply(0, 0, 0, 0);
}

/* WM_OP_QUERY_GEOMETRY — read back a window's pixel + cell extents.
 *   R5 = wid (or 0 to use the first live window — the typical
 *           leader-spawn shell that didn't open its own window
 *           inherits the supervisor's CONSOLE/GRID caps; passing
 *           wid=0 lets such a child query "the window I'm rendering
 *           into" without having to know its id).
 * Reply:
 *   R3 = status,
 *   R4 = geom_a = (w_px << 16) | h_px         (usable pixel area)
 *   R5 = geom_b = (w_cells << 16) | h_cells   (usable cell grid)
 *
 * No auth: window dimensions are public — every cap holder already
 * sees the rendered output and can guess the size from observation.
 * Phase 60 step 5. */
static void
handle_query_geometry(int wid)
{
	if (wid == 0) {
		int i;
		for (i = 1; i <= MAX_WINDOWS; i++) {
			if (window_type[i - 1] != 0) { wid = i; break; }
		}
	}
	if (wid < 1 || wid > MAX_WINDOWS) {
		wm_reply(E_INVAL, 0, 0, 0);
		return;
	}
	if (window_type[wid - 1] == 0) {
		wm_reply(E_NOENT, 0, 0, 0);
		return;
	}
	int geom_a = ((DEFAULT_W_PX & 0xFFFF) << 16) | (DEFAULT_H_PX & 0xFFFF);
	int geom_b = ((DEFAULT_W_CELLS & 0xFFFF) << 16) | (DEFAULT_H_CELLS & 0xFFFF);
	wm_reply(0, geom_a, geom_b, wid);
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
			free_window_grid(wid);
			free_window_vector(wid);
			free_window_raster(wid);
			free_window_fb(wid);
			window_z_remove(wid);
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

/* === Vector rasterisation =============================================
 *
 * Phase 59 / WM γ.11 — rasterise VEC_OP_* into the framebuffer
 * directly (no intermediate per-window backing store yet).  All
 * primitives share one ObjStoreBytes-per-row helper, fb_blit_row,
 * and clip to FB_W × FB_H themselves.  Coordinates are pixel-space
 * (NOT cell-space); the caller already passes 16-bit signed halves
 * unpacked from the wire's packed (x, y) / (w, h) words.
 *
 * Performance note: VEC_OP_LINE and VEC_OP_*_OUTLINE drop into
 * per-pixel writes via fb_blit_row(y, x, &c, 1).  That's one wire
 * RTT per pixel — fine for a smoke test (lines stay under a few
 * hundred pixels) and well under the rate the framebuffer-repaint
 * timer fires anyway.  Run-batching along the Bresenham trace is a
 * follow-up.  Fill primitives (RECT_FILL / OVAL_FILL) already pay
 * one RTT per row regardless of width. */

/* Phase 60 step 10 — fb_blit_row now writes into the WINDOW backing
 * store rather than the screen FB.  Coordinates (x, y) are in the
 * client-visible content area (0..USABLE_W_PX × 0..CELL_CONTENT_PX),
 * matching what wm_get_geometry advertises; we add TITLE_BAR_PX to y
 * before computing the destination offset so the title bar is never
 * touched.  Composite of the touched region happens once per
 * forward_* call (composite_content_area) — calling it per-row would
 * thrash the screen FB, and clients always paint a complete shape
 * before the next request anyway.
 *
 * Pixel buffer must live on our stack (STACK_BOTTOM-relative
 * offsets) — same as before. */
static void
fb_blit_row(int y, int x, const unsigned char *pixels, int n_pixels)
{
	if (y < 0 || y >= CELL_CONTENT_PX) return;
	if (n_pixels <= 0)                 return;
	if (x < 0) {
		if (n_pixels + x <= 0) return;
		pixels   -= x;        /* skip leading off-screen */
		n_pixels += x;
		x         = 0;
	}
	if (x >= USABLE_W_PX)            return;
	if (x + n_pixels > USABLE_W_PX)  n_pixels = USABLE_W_PX - x;

	int src_off = (int)((unsigned int)pixels - STACK_BOTTOM);
	int dst_off = (y + TITLE_BAR_PX) * USABLE_W_PX + x;
	asm volatile(
		"addu  r7, %1, r0\n"
		"addu  r8, %2, r0\n"
		"addu  r9, %3, r0\n"
		"omov  o1, o11\n"
		"orefld o2, %0(o12)\n"
		"addu  r4, r7, r0\n"
		"addu  r5, r8, r0\n"
		"addu  r6, r9, r0\n"
		"call  #0x109\n"        /* ObjStoreBytes */
		"nop"
		:
		: "i"(WM_ACTIVE_FB_SLOT_OFFSET),
		  "r"(src_off), "r"(dst_off), "r"(n_pixels)
		: "r1", "r2", "r3", "r4", "r5", "r6",
		  "r7", "r8", "r9"
	);
}

/* Forward decl — vec_scratch_row + prep_scratch_row are defined
 * after the rasterisers because the oval helpers use the same
 * scratch buffer too. */
static unsigned char *prep_scratch_row(int n, unsigned char color);

/* Single-pixel store.  Bresenham line and oval-outline plot pixel-
 * by-pixel; this is just a one-byte fb_blit_row.  Caller's `c`
 * lives on the stack across the call so the byte address is valid
 * for ObjStoreBytes. */
static void
fb_set_pixel(int x, int y, unsigned char color)
{
	unsigned char one = color;
	fb_blit_row(y, x, &one, 1);
}

/* pcc-orisc passes only 4 args in registers and trips ("adrput:
 * illegal op 57") on calls with five-or-more args.  Each rasteriser
 * therefore takes ≤4 args and reads the current pen color from
 * `cur_vec_color`, which forward_vector_write seeds before every
 * draw call. */
static unsigned char cur_vec_color;

/* Bresenham line.  Standard integer algorithm; signs of dx/dy give
 * the step direction, the error term governs which axis advances
 * each iteration.  Plots inclusive endpoints (x1,y1)..(x2,y2). */
static void
draw_line(int x1, int y1, int x2, int y2)
{
	unsigned char color = cur_vec_color;
	int dx = (x2 > x1) ? (x2 - x1) : (x1 - x2);
	int dy = (y2 > y1) ? (y2 - y1) : (y1 - y2);
	int sx = (x1 < x2) ? 1 : -1;
	int sy = (y1 < y2) ? 1 : -1;
	int err = (dx > dy ? dx : -dy) / 2;
	int x = x1, y = y1;
	for (;;) {
		fb_set_pixel(x, y, color);
		if (x == x2 && y == y2) break;
		int e2 = err;
		if (e2 > -dx) { err -= dy; x += sx; }
		if (e2 <  dy) { err += dx; y += sy; }
	}
}

/* Filled rect.  One pre-built row of `color` bytes, then h ObjStoreBytes
 * calls — h wire RTTs total regardless of width. */
static void
draw_rect_fill(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	unsigned char *rp = prep_scratch_row(w, cur_vec_color);
	int dy;
	for (dy = 0; dy < h; dy++) fb_blit_row(y + dy, x, rp, w);
}

/* Outline rect — top + bottom rows in two ObjStoreBytes, then per-pixel
 * vertical edges (degree(h) RTTs). */
static void
draw_rect_outline(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	unsigned char color = cur_vec_color;
	int top    = y;
	int bottom = y + h - 1;
	int left   = x;
	int right  = x + w - 1;
	unsigned char *rp = prep_scratch_row(w, color);
	fb_blit_row(top, x, rp, w);
	if (bottom != top) fb_blit_row(bottom, x, rp, w);
	int dy;
	for (dy = 1; dy < h - 1; dy++) {
		fb_set_pixel(left, y + dy, color);
		if (right != left) fb_set_pixel(right, y + dy, color);
	}
}

/* Filled ellipse — scanline approach.  For each row dy in [-ry, ry],
 * find the largest hx with `hx² · ry² + dy² · rx² ≤ rx² · ry²`, then
 * blit (2·hx + 1) pixels at (cx - hx, cy + dy).  No sqrt: linear scan
 * starting from 0 is O(rx) per row, O(rx · ry) total — fine for the
 * sizes a smoke test exercises (a 200×100 oval is 20K iterations on
 * the WM CPU, no wire traffic).  Inner blits already pay one RTT per
 * row, which dominates wallclock. */
/* Per-oval scratch — moved out of the function locals because
 * pcc-orisc's codegen hits "adrput: illegal op 57" when chained
 * three-way multiplies coexist with multiple in-scope locals and
 * 4-arg helper calls in the same function.  Single-threaded WM
 * means a single static set is fine; saves us from passing six
 * args through every helper. */
static int oval_rx;
static int oval_cx;
static int oval_cy;
static int oval_rx2;
static int oval_ry2;
static int oval_rxy2;

/* Stack-resident scratch row used by all fill primitives, also
 * static for the same register-pressure reason.  Sized to the
 * client-visible content width since fb_blit_row clips at
 * USABLE_W_PX anyway. */
static unsigned char vec_scratch_row[USABLE_W_PX];

/* Fill vec_scratch_row[0..n] with `color` and return its base. */
static unsigned char *
prep_scratch_row(int n, unsigned char color)
{
	if (n > USABLE_W_PX) n = USABLE_W_PX;
	int i;
	for (i = 0; i < n; i++) vec_scratch_row[i] = color;
	return vec_scratch_row;
}

/* Largest hx with `hx² · oval_ry2 + dy_term ≤ oval_rxy2`.  Linear
 * scan from 0; O(oval_rx) per call. */
static int
ellipse_hx_for(int dy_term)
{
	int hx = 0;
	int dx;
	for (dx = 0; dx <= oval_rx; dx++) {
		int dxsq = dx * dx;
		int term = dxsq * oval_ry2;
		if (term + dy_term > oval_rxy2) break;
		hx = dx;
	}
	return hx;
}

/* Init oval_* statics from (x, y, w, h).  Returns 0 if degenerate
 * (caller should fall back to rect_*), 1 otherwise. */
static int
oval_setup(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return 0;
	int rx = w / 2;
	int ry = h / 2;
	if (rx <= 0 || ry <= 0) return 0;
	oval_rx   = rx;
	oval_cx   = x + rx;
	oval_cy   = y + ry;
	oval_rx2  = rx * rx;
	oval_ry2  = ry * ry;
	oval_rxy2 = oval_rx2 * oval_ry2;
	return 1;
}

/* Plot one filled scanline of the current oval (statics) at offset
 * dy, painting through `rp`. */
static void
oval_fill_scanline(int dy, unsigned char *rp)
{
	int dy_term = dy * dy * oval_rx2;
	int hx = ellipse_hx_for(dy_term);
	fb_blit_row(oval_cy + dy, oval_cx - hx, rp, 2 * hx + 1);
}

static void
draw_oval_fill(int x, int y, int w, int h)
{
	if (!oval_setup(x, y, w, h)) {
		draw_rect_fill(x, y, w, h);
		return;
	}
	int ry = h / 2;
	unsigned char *rp = prep_scratch_row(2 * oval_rx + 1, cur_vec_color);
	int dy;
	for (dy = -ry; dy <= ry; dy++) oval_fill_scanline(dy, rp);
}

/* Outline ellipse — same per-row hx computation as draw_oval_fill
 * but plot only the leftmost and rightmost pixels per row.  Result
 * is a single-pixel outline that can have small gaps where the
 * curve is steepest (between rows where hx changes by more than 1);
 * acceptable for v1.  Midpoint ellipse with 4-quadrant symmetry is
 * the canonical fix and a follow-up. */
static void
oval_outline_scanline(int dy)
{
	unsigned char color = cur_vec_color;
	int dy_term = dy * dy * oval_rx2;
	int hx = ellipse_hx_for(dy_term);
	fb_set_pixel(oval_cx - hx, oval_cy + dy, color);
	if (hx != 0) fb_set_pixel(oval_cx + hx, oval_cy + dy, color);
}

static void
draw_oval_outline(int x, int y, int w, int h)
{
	if (!oval_setup(x, y, w, h)) {
		draw_rect_outline(x, y, w, h);
		return;
	}
	int ry = h / 2;
	int dy;
	for (dy = -ry; dy <= ry; dy++) oval_outline_scanline(dy);
}

/* Flush a strip of `n_glyphs` printable chars at cell (row, col_start)
 * to the framebuffer.  All glyphs share the same cell row, so they
 * project to CELL_H contiguous pixel rows.
 *
 * Phase 60 step 4: rasterisation moves into simorisc via the
 * #0x10C ObjBlitGlyphs primitive.  Pre-step-4 this function did
 * the bit-decode in interpreted WM C (~80K simulated insns per
 * 80-char strip = ~1s wallclock at simorisc's interpretation rate),
 * which dominated text-output latency.  Now the WM emits one
 * firmware-call per strip; simorisc does the decode + writes
 * natively in Python.  Off-screen clipping handled simorisc-side
 * via the FB descriptor's own width/height. */
static void
flush_strip(const unsigned char *glyphs, int n_glyphs,
            int cell_row, int col_start)
{
	if (n_glyphs <= 0)        return;
	if (cell_row < 0)         return;
	if (cell_row >= N_ROWS)   return;
	if (col_start < 0)        return;
	if (col_start >= N_COLS)  return;
	if (n_glyphs > N_COLS - col_start) n_glyphs = N_COLS - col_start;
	if (n_glyphs > 0xFFFF)    n_glyphs = 0xFFFF;

	int text_off = (int)((unsigned int)glyphs - STACK_BOTTOM);
	int font_off = (int)((unsigned int)&font_8x16[0][0] - DATA_VA);
	/* Phase 60 step 8: cell content sits below the title bar in the
	 * window FB, so we add TITLE_BAR_CELLS to the cell-row coord.
	 * `col_start` stays as-is (no horizontal padding); `cell_row` is
	 * the client's logical row within the N_ROWS-tall content area. */
	int fb_row = cell_row + TITLE_BAR_CELLS;
	int packed_xy = ((col_start & 0xFFFF) << 16) | (fb_row & 0xFFFF);
	int packed_shape = ((n_glyphs & 0xFFFF) << 16)
	                 | ((WM_FG_COLOR & 0xFF) << 8)
	                 | (WM_BG_COLOR & 0xFF);

	/* pcc-orisc input-clobber dance: copy the four "r" inputs to
	 * safe temps (r8..r11) before the body's first store of
	 * r4..r7. */
	asm volatile(
		"addu   r8,  %0, r0\n"      /* save packed_xy */
		"addu   r9,  %1, r0\n"      /* save packed_shape */
		"addu   r10, %2, r0\n"      /* save font_off */
		"addu   r11, %3, r0\n"      /* save text_off */
		"orefld o1, %4(o12)\n"      /* O1 = window FB (offscreen) */
		"omov   o2, o15\n"          /* O2 = boot data (font) */
		"omov   o3, o11\n"          /* O3 = boot stack (text) */
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"addu   r7, r11, r0\n"
		"call   #0x10C\n"           /* ObjBlitGlyphs */
		"nop"
		:
		: "r"(packed_xy), "r"(packed_shape),
		  "r"(font_off),  "r"(text_off),
		  "i"(WM_ACTIVE_FB_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r7",
		  "r8", "r9", "r10", "r11"
	);

	/* Composite the just-painted strip onto the screen FB so the
	 * change is visible.  Strip pixel rect in window-local coords:
	 * (col_start*CELL_W, fb_row*CELL_H) → (n_glyphs*CELL_W, CELL_H).
	 * fb_row already includes the TITLE_BAR_CELLS offset. */
	composite_window_region(col_start * CELL_W, fb_row * CELL_H,
	                        n_glyphs   * CELL_W, CELL_H);
}

/* Phase 60 step 6 — shift the inner cell area up by one cell row
 * (CELL_H pixels) and clear the freshly-exposed bottom row to the
 * background colour.  Used by render_buffer when the cursor advances
 * past the last usable row.  The chrome (border + outer-ring
 * margin) is left untouched: only the inner pixel rectangle scrolls.
 *
 * One firmware call (#0x10E ObjFbScroll) does the whole thing —
 * Python-side memmove + byte-fill, no per-pixel work in interpreted
 * WM code.  For an 80-char output the cost is ~CELL_H wallclock-ms
 * regardless of how many rows we're shifting; well below the
 * per-strip ObjBlitGlyphs cost. */
static void
fb_scroll_up_one_cell(void)
{
	/* Phase 60 step 8: scroll the cell-content region only.  The
	 * title bar at y ∈ [0..TITLE_BAR_PX) stays put so the window
	 * identity doesn't smear during fast output.  Region covers
	 * y ∈ [TITLE_BAR_PX..USABLE_H_PX) — i.e. CELL_CONTENT_PX rows. */
	int packed_xy = ((0 & 0xFFFF) << 16) | (TITLE_BAR_PX & 0xFFFF);
	int packed_wh = ((USABLE_W_PX & 0xFFFF) << 16)
	              | (CELL_CONTENT_PX & 0xFFFF);
	int packed_dy_fill = ((CELL_H & 0xFFFFFF) << 8)
	                   | (WM_BG_COLOR & 0xFF);
	asm volatile(
		"addu   r8,  %0, r0\n"      /* save packed_xy */
		"addu   r9,  %1, r0\n"      /* save packed_wh */
		"addu   r10, %2, r0\n"      /* save packed_dy_fill */
		"orefld o1, %3(o12)\n"
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"call   #0x10E\n"           /* ObjFbScroll */
		"nop"
		:
		: "r"(packed_xy), "r"(packed_wh), "r"(packed_dy_fill),
		  "i"(WM_ACTIVE_FB_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6",
		  "r8", "r9", "r10"
	);
	/* Only the cell-content area changed; title bar is untouched. */
	composite_window_region(0, TITLE_BAR_PX,
	                        USABLE_W_PX, CELL_CONTENT_PX);
}

/* Hoist `*row` back into [0, N_ROWS) by scrolling the inner cell
 * area up one cell-row at a time.  Called from render_buffer after
 * any branch that increments the cursor row.  Multiple newlines past
 * the bottom legitimately scroll once per newline — same semantics
 * as a real terminal. */
static void
maybe_scroll(int *row)
{
	while (*row >= N_ROWS) {
		fb_scroll_up_one_cell();
		*row -= 1;
	}
}

/* Render `count` bytes from `buf` to window `wid`'s framebuffer
 * surface, accumulating runs of printable chars on the same cell
 * row into strips and flushing each strip in 16 wire RTTs total.
 * Control chars ('\n' / '\r' / '\b') boundary the strip and adjust
 * the cursor; non-printable chars (other than those three) advance
 * the cursor without rendering, also boundarying the strip.
 *
 * Phase 60 step 6: the cursor scrolls when it advances past the last
 * usable row — fb_scroll_up_one_cell does the FB-internal pixel
 * shift, and `row` is rolled back to N_ROWS-1 so the next strip
 * lands on the fresh blank row.
 *
 * Cursor state in window_cur_col/row[] is updated on return. */
static void
render_buffer(int wid, const unsigned char *buf, int count)
{
	/* Phase 60 step 11: install this wid's FB as the painting target
	 * so flush_strip / maybe_scroll / fb_scroll_up_one_cell all hit
	 * the right window. */
	set_active_window(wid);

	int col = window_cur_col[wid - 1];
	int row = window_cur_row[wid - 1];

	unsigned char strip[N_COLS];
	int strip_len       = 0;
	int strip_row       = row;
	int strip_col_start = col;

	int i;
	for (i = 0; i < count; i++) {
		int ch = (int)buf[i];

		if (ch == '\n') {
			flush_strip(strip, strip_len, strip_row, strip_col_start);
			col = 0;
			row += 1;
			maybe_scroll(&row);
			strip_len       = 0;
			strip_row       = row;
			strip_col_start = 0;
			continue;
		}
		if (ch == '\r') {
			flush_strip(strip, strip_len, strip_row, strip_col_start);
			col = 0;
			strip_len       = 0;
			strip_row       = row;
			strip_col_start = 0;
			continue;
		}
		if (ch == '\b') {
			flush_strip(strip, strip_len, strip_row, strip_col_start);
			if (col > 0) col -= 1;
			/* Backspace doesn't erase the rendered glyph — the
			 * next char at this position will overwrite it.  For
			 * a pure erase we'd need to render a space here. */
			strip_len       = 0;
			strip_row       = row;
			strip_col_start = col;
			continue;
		}
		if (ch < 32 || ch > 126) {
			/* Non-printable: drop, advance cursor, boundary the
			 * strip so the next printable run starts fresh. */
			flush_strip(strip, strip_len, strip_row, strip_col_start);
			col += 1;
			if (col >= N_COLS) { col = 0; row += 1; maybe_scroll(&row); }
			strip_len       = 0;
			strip_row       = row;
			strip_col_start = col;
			continue;
		}

		/* Printable: append to strip, advance cursor.  On wrap,
		 * flush the strip and start a fresh one on the next row. */
		strip[strip_len++] = (unsigned char)ch;
		col += 1;
		if (col >= N_COLS) {
			flush_strip(strip, strip_len, strip_row, strip_col_start);
			col = 0;
			row += 1;
			maybe_scroll(&row);
			strip_len       = 0;
			strip_row       = row;
			strip_col_start = 0;
		}
	}
	flush_strip(strip, strip_len, strip_row, strip_col_start);

	window_cur_col[wid - 1] = col;
	window_cur_row[wid - 1] = row;
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
 *   2. ObjFetchBytes a private copy of the source bytes into a
 *      stack-local buffer (one wire RTT).  This MUST happen before
 *      we forward the SEND — once the terminal replies, the leader
 *      unblocks and may reuse its stack, racing any later read.
 *   3. Re-emit the SEND to the underlying terminal CONSOLE with the
 *      original (source, offset, count, reply_cap).  Terminal does
 *      its own OBJ_READ_REQ for the source; it lands while the
 *      leader is still blocked, so it sees the same bytes we did.
 *   4. Pass our private copy to render_buffer, which splits it into
 *      runs of printable chars on the same cell row and flushes each
 *      as one strip — CELL_H = 16 OBJ_WRITE_REQs per strip regardless
 *      of how many chars it contains.  These arrive at oriscterm
 *      after the SEND_DELIVER, so the console pane is current before
 *      the framebuffer pane catches up.
 * When the γ-stage migration retires the underlying console pane,
 * step 2 goes away (the render becomes the only output path — text
 * becomes pixels only). */
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

	/* ObjFetchBytes FIRST.  Pull a private copy of the source bytes
	 * before any other work.  The leader's source ref points into
	 * its stack — once the leader's task unblocks (when the
	 * terminal's reply lands at the reply_cap), it will reuse that
	 * stack region for the next print, and any later read of the
	 * source ref returns garbage.  We have to capture our copy
	 * while the leader is still blocked on this SEND, which is
	 * before we forward.  Clamp to 256 bytes — the standard
	 * term_print path batches per-string, so a typical write is
	 * short.  Anything larger gets the leading 256 rendered (the
	 * full count is forwarded below so the terminal still gets all
	 * the bytes). */
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
	}

	/* Phase 60 step 3 — was a forward to oriscterm's underlying
	 * CONSOLE here; with oriscterm gone, the WM IS the CONSOLE
	 * receiver.  If the client's SEND carried a non-null reply_cap
	 * (term_print_n_sync wants an ack so it can unblock), send a
	 * header-only SEND_DELIVER ack.  Otherwise nothing more to do
	 * on the wire — the client's SEND already returned (R2 = OK)
	 * the moment we delivered the packet to the per-window queue. */
	int reply_isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1"
		: "=r"(reply_isn)
		: "i"(WM_FORWARD_REPLY_SLOT_OFFSET)
		: "r1"
	);
	if (!reply_isn) {
		asm volatile(
			"orefld o1, %0(o12)\n"   /* O1 = client's reply_cap */
			"onull  o2\n"
			"onull  o3\n"
			"addiu  r4, r0, 0\n"
			"addiu  r5, r0, 0\n"
			"addiu  r6, r0, 0\n"
			"addiu  r7, r0, 0\n"
			"send   o1"
			:
			: "i"(WM_FORWARD_REPLY_SLOT_OFFSET)
			: "r1", "r4", "r5", "r6", "r7"
		);
	}

	/* Render against our private buf copy.  We've already replied
	 * (if needed); the leader's stack is fair game to reuse from
	 * here on out, which is fine since buf is on OUR stack. */
	if (fetch_count > 0) {
		render_buffer(wid, buf, fetch_count);
	}
}

/* WSURF_GRID counterpart of forward_console_write.  The SEND payload
 * for a per-window GRID service is:
 *   O2 = byte source ref      (passed via slot stash, like CONSOLE)
 *   R4 = byte offset within source
 *   R5 = byte count
 *   R6 = grid column (cell x)
 *   R7 = grid row    (cell y)
 *
 * Special: col == row == 0xFFFFFFFF (signed -1, -1) is the legacy
 * "clear the entire grid surface" sentinel from oriscterm's pre-WM
 * design.  In the framebuffer-shared world the right semantics
 * aren't obvious (clearing every pixel would also wipe the WM's
 * console glyphs), so for now we treat it as a no-op and TODO a
 * proper per-window backing store before reviving it.
 *
 * Non-clear path: ObjFetchBytes the bytes locally, then flush_strip
 * a single horizontal strip of (count) glyphs at (col, row).  No
 * cursor advance — grid is explicit-positioning, unlike CONSOLE. */
static void
forward_grid_write(int offset, int count, int col, int row)
{
	/* Clear sentinel: no-op for now.  See note above. */
	if ((unsigned int)col == 0xFFFFFFFF
	 && (unsigned int)row == 0xFFFFFFFF) {
		return;
	}
	/* Bounds: drop entirely if the start cell is off-screen.  Clamp
	 * count if the strip would extend past N_COLS — flush_strip
	 * itself drops cell rows >= N_ROWS, so the row check is
	 * advisory. */
	if (col < 0 || col >= N_COLS) return;
	if (row < 0 || row >= N_ROWS) return;
	if (count <= 0)               return;
	int max_glyphs = N_COLS - col;
	if (count > max_glyphs) count = max_glyphs;
	if (count > 256)        count = 256;     /* fetch buffer cap */

	/* Stash source ref so the asm below can OREFLD it back. */
	asm volatile("orefst o2, %0(o12)"
	             :: "i"(WM_FORWARD_SRC_SLOT_OFFSET));

	unsigned char buf[256];
	int buf_off = (int)((unsigned int)buf - STACK_BOTTOM);
	asm volatile(
		"orefld o1, %0(o12)\n"
		"omov   o2, o11\n"
		"addu   r4, %1, r0\n"
		"addu   r5, %2, r0\n"
		"addu   r6, %3, r0\n"
		"call   #0x108\n"              /* ObjFetchBytes */
		"nop"
		:
		: "i"(WM_FORWARD_SRC_SLOT_OFFSET),
		  "r"(offset), "r"(buf_off), "r"(count)
		: "r1", "r2", "r3", "r4", "r5", "r6"
	);

	/* flush_strip assumes printable ASCII (its callers in render_buffer
	 * filter ahead of it).  Grid input may contain anything; substitute
	 * non-printable bytes with space so we render a blank cell instead
	 * of indexing off the end of font_8x16. */
	int i;
	for (i = 0; i < count; i++) {
		if (buf[i] < 32 || buf[i] > 126) buf[i] = ' ';
	}

	/* Render one strip at (row, col).  No cursor advance — grid is
	 * explicit positioning, unlike CONSOLE. */
	flush_strip(buf, count, row, col);
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
			set_active_window(wid);
			forward_console_write(wid, offset, count);
		}
	}
}

/* See poll_window_grids — we sw R2 (status) into this global from
 * inside the poll asm because the asm body's output-operand limit
 * caps us at 4 and we need 5 captures (status + offset + count +
 * col + row). */
static int _wm_grid_poll_status;

/* Mirror of poll_window_consoles for the GRID surface.  Same shape:
 * round-robin per-window, ReceiveQueuePoll with timeout=0, dispatch
 * non-empty SENDs to forward_grid_write.  The queue-poll overlay
 * sets R3..R6 from the sender's R4..R7 (offset, count, col, row). */
static void
poll_window_grids(void)
{
	int wid;
	for (wid = 1; wid <= MAX_WINDOWS; wid++) {
		if (window_type[wid - 1] != WIN_TYPE_CONSOLE) continue;

		load_grid_to_o1(wid);
		/* Poll the per-window queue.  We need five values out
		 * (status + offset + count + col + row); pcc-orisc's
		 * codegen caps single-asm register outputs at 4.
		 * Workaround: capture R3..R6 (offset, count, col, row)
		 * via the asm's regular output operands, and stash R2
		 * (status) to a file-scope global from inside the same
		 * asm.  Using a global instead of a stack-local
		 * sidesteps any pcc bug with stack-spill addressing
		 * mid-asm; the global is fine since we only use it
		 * synchronously across this one call. */
		int g_offset, g_count, g_col, g_row;
		asm volatile(
			"addiu r4, r0, 0\n"
			"call  #0x204\n"           /* ReceiveQueuePoll */
			"nop\n"
			"la    r1, _wm_grid_poll_status\n"
			"sw    r2, 0(r1)\n"
			"addu  %0, r3, r0\n"
			"addu  %1, r4, r0\n"
			"addu  %2, r5, r0\n"
			"addu  %3, r6, r0"
			: "=r"(g_offset), "=r"(g_count),
			  "=r"(g_col),    "=r"(g_row)
			:
			: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
		);
		if (_wm_grid_poll_status == 0) {
			set_active_window(wid);
			forward_grid_write(g_offset, g_count, g_col, g_row);
		}
	}
}

/* Unpack a signed 16-bit half from a 32-bit packed word.  Top bit
 * sign-extends; matches oriscterm's unpack_pair so negative
 * coordinates round-trip end-to-end. */
static int
vec_unpack_hi(int packed)
{
	int v = (packed >> 16) & 0xFFFF;
	if (v & 0x8000) v |= ~0xFFFF;
	return v;
}

static int
vec_unpack_lo(int packed)
{
	int v = packed & 0xFFFF;
	if (v & 0x8000) v |= ~0xFFFF;
	return v;
}

/* WSURF_VECTOR forward.  Wire payload:
 *   R3 = op (VEC_OP_*)
 *   R4 = packed1 — for LINE: (x1<<16)|y1, for RECT/OVAL: (x<<16)|y,
 *                  for SET_COLOR: palette index in low half
 *   R5 = packed2 — for LINE: (x2<<16)|y2, for RECT/OVAL: (w<<16)|h,
 *                  unused for CLEAR / SET_COLOR
 *
 * No source bytes ref — vector ops carry their full payload in
 * int_payload.  Per-window pen color lives in window_vec_color[];
 * SET_COLOR mutates it, draw ops read it.  CLEAR is a no-op for
 * the same reason WSURF_GRID's clear sentinel is — no per-window
 * backing store yet (full-FB clear would also wipe console + grid
 * rendering).  Pending the per-window backing-store milestone. */
static void
forward_vector_write(int wid, int op, int packed1, int packed2)
{
	if (wid < 1 || wid > MAX_WINDOWS) return;
	int slot = wid - 1;       /* avoids `la sym+-1` codegen pcc emits
	                           * for window_vec_color[wid - 1] */
	cur_vec_color = window_vec_color[slot];

	if (op == VEC_OP_SET_COLOR) {
		int idx = vec_unpack_lo(packed1);
		if (idx < 0)   idx = 0;
		if (idx > 255) idx = 255;
		window_vec_color[slot] = (unsigned char)idx;
		return;
	}
	if (op == VEC_OP_CLEAR) {
		/* Repaint the cell-content area to bg via ObjFillRect on
		 * the window FB, then composite once.  Phase 60 step 10:
		 * with per-window backing stores this no longer wipes
		 * console + grid output — the content area is the
		 * vector/raster space, distinct from chrome and title. */
		int xy = ((0 & 0xFFFF) << 16) | (TITLE_BAR_PX & 0xFFFF);
		int wh = ((USABLE_W_PX & 0xFFFF) << 16)
		       | (CELL_CONTENT_PX & 0xFFFF);
		fill_rect_window(xy, wh, WM_BG_COLOR);
		composite_content_area();
		return;
	}

	int painted = 0;
	if (op == VEC_OP_LINE) {
		int x1 = vec_unpack_hi(packed1);
		int y1 = vec_unpack_lo(packed1);
		int x2 = vec_unpack_hi(packed2);
		int y2 = vec_unpack_lo(packed2);
		draw_line(x1, y1, x2, y2);
		painted = 1;
	} else {
		int x = vec_unpack_hi(packed1);
		int y = vec_unpack_lo(packed1);
		int w = vec_unpack_hi(packed2);
		int h = vec_unpack_lo(packed2);
		if      (op == VEC_OP_RECT_FILL)    { draw_rect_fill(x, y, w, h);    painted = 1; }
		else if (op == VEC_OP_RECT_OUTLINE) { draw_rect_outline(x, y, w, h); painted = 1; }
		else if (op == VEC_OP_OVAL_FILL)    { draw_oval_fill(x, y, w, h);    painted = 1; }
		else if (op == VEC_OP_OVAL_OUTLINE) { draw_oval_outline(x, y, w, h); painted = 1; }
		/* Unknown op: drop silently — clients see no error since
		 * vector SENDs are fire-and-forget anyway. */
	}
	if (painted) composite_content_area();
}

/* See poll_window_grids — same status-via-global trick, capped at 4
 * regular outputs by pcc-orisc.  Vector wire only carries 3 int
 * payload values (op + 2 packed words) so we'd fit in 4 outputs
 * naturally, but using the same idiom as grid keeps both polls
 * symmetrical. */
static int _wm_vector_poll_status;

static void
poll_window_vectors(void)
{
	int wid;
	for (wid = 1; wid <= MAX_WINDOWS; wid++) {
		if (window_type[wid - 1] != WIN_TYPE_CONSOLE) continue;

		load_vector_to_o1(wid);
		int op, packed1, packed2;
		asm volatile(
			"addiu r4, r0, 0\n"
			"call  #0x204\n"           /* ReceiveQueuePoll */
			"nop\n"
			"la    r1, _wm_vector_poll_status\n"
			"sw    r2, 0(r1)\n"
			"addu  %0, r3, r0\n"
			"addu  %1, r4, r0\n"
			"addu  %2, r5, r0"
			: "=r"(op), "=r"(packed1), "=r"(packed2)
			:
			: "r1", "r2", "r3", "r4", "r5", "memory"
		);
		if (_wm_vector_poll_status == 0) {
			set_active_window(wid);
			forward_vector_write(wid, op, packed1, packed2);
		}
	}
}

/* WSURF_RASTER forward.  Wire payload:
 *   O2 = source pixel buffer ref     (saved into WM_FORWARD_SRC_SLOT
 *                                     during dispatch)
 *   R3 = op (RST_OP_*)
 *   R4 = packed1: (x << 16) | y    (destination in framebuffer)
 *   R5 = packed2: (w << 16) | h    (dimensions of pixel buffer)
 *   R6 = byte offset within source where pixel data starts
 *
 * BLIT path: per-row ObjFetchBytes the source pixels into
 * vec_scratch_row (reused from γ.11; both fill primitives and raster
 * blits go through this one buffer), then ObjStoreBytes the row into
 * the framebuffer.  2 wire RTTs per row, h rows total = 2h RTTs.
 *
 * vec_scratch_row lives in the data segment (oriscwm's own boot data
 * ref O15), so ObjFetchBytes uses O15 as destination and ObjStoreBytes
 * uses O15 as source — different from flush_strip which addresses
 * the stack-resident pixel_row via O11.  Caller's source ref came in
 * O2 and is stashed via WM_FORWARD_SRC_SLOT same as forward_grid_write
 * does. */
static void
forward_raster_write(int op, int packed1, int packed2, int byte_offset)
{
	if (op == RST_OP_CLEAR) {
		/* Phase 60 step 10: same per-window backing-store treatment
		 * as VEC_OP_CLEAR — fill the cell-content area with bg and
		 * composite once. */
		int xy = ((0 & 0xFFFF) << 16) | (TITLE_BAR_PX & 0xFFFF);
		int wh = ((USABLE_W_PX & 0xFFFF) << 16)
		       | (CELL_CONTENT_PX & 0xFFFF);
		fill_rect_window(xy, wh, WM_BG_COLOR);
		composite_content_area();
		return;
	}
	if (op != RST_OP_BLIT) return;

	int x = vec_unpack_hi(packed1);
	int y = vec_unpack_lo(packed1);
	int w = vec_unpack_hi(packed2);
	int h = vec_unpack_lo(packed2);
	if (w <= 0 || h <= 0)        return;
	if (w > USABLE_W_PX) w = USABLE_W_PX;

	/* Stash sender's source ref so we can OREFLD it again on each
	 * per-row ObjFetchBytes. */
	asm volatile("orefst o2, %0(o12)"
	             :: "i"(WM_FORWARD_SRC_SLOT_OFFSET));

	int scratch_data_off = (int)((unsigned int)vec_scratch_row - DATA_VA);

	int painted = 0;
	int row;
	for (row = 0; row < h; row++) {
		int dst_y = y + row;
		if (dst_y < 0 || dst_y >= CELL_CONTENT_PX) continue;

		int src_off = byte_offset + row * w;
		/* dst_off lands in the WINDOW FB, with TITLE_BAR_PX added to
		 * the y coord and USABLE_W_PX as the row stride. */
		int dst_off = (dst_y + TITLE_BAR_PX) * USABLE_W_PX + x;

		/* ObjFetchBytes from sender's source (O2 ← slot) into
		 * our vec_scratch_row via boot data ref (O15). */
		asm volatile(
			"orefld o1, %0(o12)\n"
			"omov   o2, o15\n"
			"addu   r4, %1, r0\n"
			"addu   r5, %2, r0\n"
			"addu   r6, %3, r0\n"
			"call   #0x108\n"
			"nop"
			:
			: "i"(WM_FORWARD_SRC_SLOT_OFFSET),
			  "r"(src_off), "r"(scratch_data_off), "r"(w)
			: "r1", "r2", "r3", "r4", "r5", "r6"
		);

		/* ObjStoreBytes from O15 (boot data, holds vec_scratch_row)
		 * into the WINDOW FB slot. */
		asm volatile(
			"omov  o1, o15\n"
			"orefld o2, %0(o12)\n"
			"addu  r4, %1, r0\n"
			"addu  r5, %2, r0\n"
			"addu  r6, %3, r0\n"
			"call  #0x109\n"
			"nop"
			:
			: "i"(WM_ACTIVE_FB_SLOT_OFFSET),
			  "r"(scratch_data_off), "r"(dst_off), "r"(w)
			: "r1", "r2", "r3", "r4", "r5", "r6"
		);
		painted = 1;
	}
	if (painted) composite_content_area();
}

/* See poll_window_grids — same status-via-global trick.  Wire here
 * carries 4 int payload values (op + 2 packed words + byte offset),
 * which exactly fills the 4-output regular operand limit. */
static int _wm_raster_poll_status;

static void
poll_window_rasters(void)
{
	int wid;
	for (wid = 1; wid <= MAX_WINDOWS; wid++) {
		if (window_type[wid - 1] != WIN_TYPE_CONSOLE) continue;

		load_raster_to_o1(wid);
		int op, packed1, packed2, byte_off;
		asm volatile(
			"addiu r4, r0, 0\n"
			"call  #0x204\n"
			"nop\n"
			"la    r1, _wm_raster_poll_status\n"
			"sw    r2, 0(r1)\n"
			"addu  %0, r3, r0\n"
			"addu  %1, r4, r0\n"
			"addu  %2, r5, r0\n"
			"addu  %3, r6, r0"
			: "=r"(op), "=r"(packed1),
			  "=r"(packed2), "=r"(byte_off)
			:
			: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
		);
		if (_wm_raster_poll_status == 0) {
			set_active_window(wid);
			forward_raster_write(op, packed1, packed2, byte_off);
		}
	}
}

/* Phase 59 / WM γ.13 — pointer mediation polls.
 *
 * poll_pointer_subscribes: drain any subscribe SENDs queued on the
 * WM-side pointer service.  Wire: O2 = subscriber's reply ref (0 to
 * unsubscribe).  Single-subscriber v1 — replace whatever's in
 * WM_PTR_SUB_SLOT.
 *
 * poll_pointer_events: drain events from the underlying terminal.
 * Wire from oriscterm: int_payload[0..3] = (evt_type, packed_xy,
 * button, btn_state).  Forward each to the current subscriber if
 * any. */
static int _wm_ptr_sub_poll_status;

static void
poll_pointer_subscribes(void)
{
	asm volatile("orefld o1, %0(o12)"
	             :: "i"(WM_POINTER_SVC_SLOT_OFFSET));
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x204\n"                   /* ReceiveQueuePoll */
		"nop\n"
		"la    r1, _wm_ptr_sub_poll_status\n"
		"sw    r2, 0(r1)"
		:
		:
		: "r1", "r2", "memory"
	);
	if (_wm_ptr_sub_poll_status != 0) return;

	/* Subscribe SENDs land sender's O2 → our O2 (the receive-queue
	 * overlay maps wire slot 1 → O2; same convention stash_owner_o2
	 * uses on the main service queue).  Stash that ref into
	 * WM_PTR_SUB_SLOT.  Null O2 from the sender means
	 * unsubscribe-all → null the slot. */
	asm volatile(
		"orefst o2, %0(o12)"
		:
		: "i"(WM_PTR_SUB_SLOT_OFFSET)
		: "memory"
	);
}

static int _wm_ptr_evt_poll_status;

static void
poll_pointer_events(void)
{
	/* If no subscriber, leave events queued in the underlying
	 * mailbox (depth 64) so they aren't lost during the window
	 * between WM boot and the client's subscribe SEND landing —
	 * which is large enough to swallow several events from a
	 * --event sequence at default --delay.  When a subscriber
	 * appears, we'll start draining naturally.  Queue overflow
	 * past 64 deep is a silent drop, acceptable for this test
	 * shape. */
	int sub_isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1"
		: "=r"(sub_isn)
		: "i"(WM_PTR_SUB_SLOT_OFFSET)
		: "r1"
	);
	if (sub_isn) return;

	asm volatile("orefld o1, %0(o12)"
	             :: "i"(WM_PTR_EVENTS_SLOT_OFFSET));
	int evt_type, packed_xy, button, btn_state;
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x204\n"
		"nop\n"
		"la    r1, _wm_ptr_evt_poll_status\n"
		"sw    r2, 0(r1)\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r6, r0"
		: "=r"(evt_type), "=r"(packed_xy),
		  "=r"(button),   "=r"(btn_state)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
	);
	if (_wm_ptr_evt_poll_status != 0) return;

	/* Forward the event: SEND to subscriber's ref with the same
	 * int_payload[0..3] = (evt_type, packed_xy, button, btn_state).
	 * No source ref.
	 *
	 * pcc-orisc doesn't always honour r4..r7 in the clobber list
	 * when picking input regs — it can place an input operand in
	 * r4 (or r5), then the first `addu r4, %x, r0` stomps that
	 * input before the later `addu r6, %y, r0` reads it.  Save
	 * all four inputs into safe temps (r8..r11) FIRST, then move
	 * them into the ABI registers for the SEND. */
	asm volatile(
		"addu   r8,  %1, r0\n"        /* save evt_type */
		"addu   r9,  %2, r0\n"        /* save packed_xy */
		"addu   r10, %3, r0\n"        /* save button */
		"addu   r11, %4, r0\n"        /* save btn_state */
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"onull  o3\n"
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"addu   r7, r11, r0\n"
		"send   o1"
		:
		: "i"(WM_PTR_SUB_SLOT_OFFSET),
		  "r"(evt_type), "r"(packed_xy),
		  "r"(button),   "r"(btn_state)
		: "r1", "r4", "r5", "r6", "r7",
		  "r8", "r9", "r10", "r11"
	);
}

/* Phase 60 step 3 — keyboard subscribe/event polls.  Direct mirror
 * of the pointer pair above: subscribers register reply refs at
 * WM_KEYBOARD_SVC_SLOT (single subscriber for v1, stashed in
 * WM_KBD_SUB_SLOT); Tk events arrive in WM_KBD_EVENTS_SLOT (a
 * TAG_INPUT_SINK fed by simorisc's display worker); each event gets
 * forwarded to the current subscriber via SEND. */
static int _wm_kbd_sub_poll_status;

static void
poll_keyboard_subscribes(void)
{
	asm volatile("orefld o1, %0(o12)"
	             :: "i"(WM_KEYBOARD_SVC_SLOT_OFFSET));
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x204\n"
		"nop\n"
		"la    r1, _wm_kbd_sub_poll_status\n"
		"sw    r2, 0(r1)"
		:
		:
		: "r1", "r2", "memory"
	);
	if (_wm_kbd_sub_poll_status != 0) return;
	asm volatile(
		"orefst o2, %0(o12)"
		:
		: "i"(WM_KBD_SUB_SLOT_OFFSET)
		: "memory"
	);
}

static int _wm_kbd_evt_poll_status;

static void
poll_keyboard_events(void)
{
	int sub_isn;
	asm volatile(
		"orefld o1, %1(o12)\n"
		"oisn   %0, o1"
		: "=r"(sub_isn)
		: "i"(WM_KBD_SUB_SLOT_OFFSET)
		: "r1"
	);
	if (sub_isn) return;

	asm volatile("orefld o1, %0(o12)"
	             :: "i"(WM_KBD_EVENTS_SLOT_OFFSET));
	int code, mods, r5_zero, r6_zero;
	asm volatile(
		"addiu r4, r0, 0\n"
		"call  #0x204\n"
		"nop\n"
		"la    r1, _wm_kbd_evt_poll_status\n"
		"sw    r2, 0(r1)\n"
		"addu  %0, r3, r0\n"
		"addu  %1, r4, r0\n"
		"addu  %2, r5, r0\n"
		"addu  %3, r6, r0"
		: "=r"(code), "=r"(mods),
		  "=r"(r5_zero), "=r"(r6_zero)
		:
		: "r1", "r2", "r3", "r4", "r5", "r6", "memory"
	);
	if (_wm_kbd_evt_poll_status != 0) return;

	/* Forward: SEND to subscriber's ref with int_payload[0..3] =
	 * (code, mods, 0, 0).  Same safe-temps dance as the pointer
	 * forwarder. */
	asm volatile(
		"addu   r8,  %1, r0\n"
		"addu   r9,  %2, r0\n"
		"orefld o1, %0(o12)\n"
		"onull  o2\n"
		"onull  o3\n"
		"addu   r4, r8, r0\n"
		"addu   r5, r9, r0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1"
		:
		: "i"(WM_KBD_SUB_SLOT_OFFSET),
		  "r"(code), "r"(mods)
		: "r1", "r4", "r5", "r6", "r7", "r8", "r9"
	);
}

const char banner_boot[]            = "oriscwm: booting\n";
const char banner_alloc_fail[]      = "oriscwm: failed to allocate service mailbox: ";
const char banner_ready[]           = "oriscwm: ready\n";

/* Compose-and-print helpers — most banners now mention the composed
 * /sys/term/<N>/* and /sys/wm/<N>/0 paths so multi-WM boots are
 * self-identifying in the host log.  Each helper prints
 * "oriscwm: <path> <suffix>" and restores boot ORs. */
static void
wm_print_walk_ok(const char *path)
{
	WM_PRINT("oriscwm: ");
	WM_PRINT(path);
	WM_PRINT(" acquired\n");
}

static void
wm_print_walk_fail(const char *path, int status)
{
	WM_PRINT("oriscwm: ");
	WM_PRINT(path);
	WM_PRINT(" walk failed: ");
	WM_PRINT_INT(status);
	WM_PRINT("\n");
}

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
	init_per_term_paths();

	WM_PRINT(banner_boot);
	WM_PRINT("oriscwm: serving /sys/term/");
	WM_PRINT_INT(my_term_idx);
	WM_PRINT("\n");

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

	/* Phase 60 step 2 — register at /sys/wm/<my_term>/0 EARLY,
	 * before the slow per-surface walks + pointer-mediation
	 * setup.  Supervisors retry wm_init briefly at boot; with
	 * registration deferred until after all the slow steps,
	 * supervisors timed out before the WM appeared and fell
	 * through to direct terminal — sysinit / login then inherited
	 * direct boot OPRs and rendered to oriscterm's text widget
	 * instead of the framebuffer.  Registering up-front lets
	 * supervisors' first wm_init succeed; subsequent SENDs queue
	 * in our mailbox and dispatch from the main loop after we
	 * finish the rest of init. */
	status = self_register();
	if (status != 0) {
		WM_PRINT("oriscwm: dir_register ");
		WM_PRINT(path_self_register);
		WM_PRINT(" failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		return 1;
	}

	/* Phase 60 step 3 — nothing under /sys/term/<N>/* anymore.
	 * Keyboard, pointer, framebuffer are all local: keyboard +
	 * pointer via the #0x10B ObjAllocInputSink primitive
	 * (simorisc's display worker enqueues Tk events into the sink
	 * queues), framebuffer via #0x102 ObjAllocFramebuffer.
	 * forward_console_write replies to the client's reply_cap
	 * directly — no underlying CONSOLE service to forward to. */
	status = alloc_local_framebuffer();
	if (status == 0) {
		WM_PRINT("oriscwm: framebuffer allocated locally (");
		WM_PRINT_INT(FB_W);
		WM_PRINT("x");
		WM_PRINT_INT(FB_H);
		WM_PRINT(")\n");
		paint_window_chrome();
	} else {
		WM_PRINT("oriscwm: ObjAllocFramebuffer failed: ");
		WM_PRINT_INT(status);
		WM_PRINT(" — glyph rendering disabled\n");
	}

	/* Pointer: local sink + WM-side subscribe service. */
	status = alloc_local_pointer_sink();
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_local_pointer_sink failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
	} else {
		status = alloc_pointer_service();
		if (status != 0) {
			WM_PRINT("oriscwm: alloc_pointer_service failed: ");
			WM_PRINT_INT(status);
			WM_PRINT("\n");
		} else {
			WM_PRINT("oriscwm: pointer mediation ready\n");
		}
	}

	/* Keyboard: local sink + WM-side subscribe service. */
	status = alloc_local_keyboard_sink();
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_local_keyboard_sink failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
	} else {
		status = alloc_keyboard_service();
		if (status != 0) {
			WM_PRINT("oriscwm: alloc_keyboard_service failed: ");
			WM_PRINT_INT(status);
			WM_PRINT("\n");
		} else {
			WM_PRINT("oriscwm: keyboard mediation ready\n");
		}
	}

	WM_PRINT("oriscwm: registered at ");
	WM_PRINT(path_self_register);
	WM_PRINT("\n");

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
			} else if (op == WM_OP_QUERY_GEOMETRY) {
				handle_query_geometry(wid_or_zero);
			} else if (op == WM_OP_SET_TITLE) {
				handle_set_title(wid_or_zero, arg);
			} else {
				wm_reply(E_INVAL, 0, 0, 0);
			}
		} else {
			/* Timeout or transient — run the auto-destroy scan. */
			scan_owner_exits();
		}

		/* Drain any pending per-window CONSOLE writes. */
		poll_window_consoles();
		poll_window_grids();
		poll_window_vectors();
		poll_window_rasters();
		poll_pointer_subscribes();
		poll_pointer_events();
		poll_keyboard_subscribes();
		poll_keyboard_events();
	}
}
