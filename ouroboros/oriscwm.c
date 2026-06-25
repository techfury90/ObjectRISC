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
#define DEFAULT_W_CELLS  80
#define DEFAULT_H_CELLS  24    /* matches N_ROWS post-Phase-60-step-14 */
#define DEFAULT_W_PX     (DEFAULT_W_CELLS * 8)   /* 640 */
#define DEFAULT_H_PX     (DEFAULT_H_CELLS * 16)  /* 384 */

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

/* Phase 59 / WM γ.13 — pointer mediation.  Subscriber routing is
 * per-wid as of step 18 (see WM_PTR_SUB_BASE below); these slots
 * are the service-side plumbing. */
#define WM_POINTER_CAP_SLOT_OFFSET      1112  /* libc-visible bound cap */
#define WM_POINTER_SVC_SLOT_OFFSET      1120  /* TAG_SERVICE clients SEND to */
/*                                      1128    formerly WM_PTR_SUB_SLOT
 *                                              (single-subscriber slot) —
 *                                              superseded by per-wid
 *                                              WM_PTR_SUB_BASE.  Kept
 *                                              reserved so neighbouring
 *                                              offsets don't shift. */
#define WM_PTR_EVENTS_SLOT_OFFSET       1136  /* TAG_INPUT_SINK kind=1 */
#define WM_SURF_POINTER_SLOT_OFFSET     1144  /* unused post step 3 */

/* Phase 60 step 3 — terminal-firmware unification.  The WM allocates
 * its keyboard / pointer event sinks LOCALLY via the simorisc
 * #0x10B ObjAllocInputSink primitive; the host display worker
 * (--display tk) translates Tk events into SEND_DELIVER packets and
 * appends them to the sink queue.  oriscterm no longer mediates
 * input — the WM CPU IS the terminal. */
#define WM_KEYBOARD_SVC_SLOT_OFFSET     1152  /* TAG_SERVICE clients SEND to */
/*                                      1160    formerly WM_KBD_SUB_SLOT —
 *                                              same story as 1128. */
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

/* Phase 60 step 18 — focus model.  Replaces the global single-slot
 * subscriber + 1-deep history from step 17 with per-window
 * subscriber tables and a `focused_wid` global that selects which
 * window's slot the WM SENDs to.  Click on a window (or open a
 * new one) → focused_wid := that wid → its subscribers start
 * receiving events.  Destroy the focused window → focused_wid
 * reverts to the new topmost.  Multi-tool, multi-window UX with
 * no race between concurrent term_init calls — each window's
 * subscriber lives in its own slot.
 *
 * Two parallel arrays, 16 wids × 8 bytes each.  load_kbd_sub_to_o1
 * / load_ptr_sub_to_o1 select the right slot per wid (pcc-orisc
 * can't synthesise a computed OREFLD offset, so each per-wid case
 * compiles to a literal byte offset). */
#define WM_KBD_SUB_BASE_OFFSET          1312      /* 1312..1440 */
#define WM_PTR_SUB_BASE_OFFSET          1440      /* 1440..1568 */

/* Phase 61 — event-driven main loop.  WM_WAITSET_SLOT holds the ref to
 * a dedicated OBJSTORE object (allocated at boot by alloc_wait_set)
 * whose slots we refill each loop iteration with every queue the WM
 * owns, then hand to the WaitAnyQueue (#0x206) readiness primitive so
 * the WM idle-blocks instead of busy-polling on a timeout.
 *
 * The slot lives in the 128-byte gap between the WM's last libc slot
 * (WM_PTR_SUB_BASE end = 1568) and the compiler's OR-spill anchor
 * (ORSPILL_ANCHOR = 1696 in macdefs.h / task.c); see task.c's O12 map.
 *
 * Fixed wait-set layout (69 × 8-byte ref slots = 552 bytes), built by
 * build_wait_set: slot 0 = main service queue (O9); 1..16 console;
 * 17..32 grid; 33..48 vector; 49..64 raster; 65 pointer-svc; 66 pointer
 * events sink; 67 keyboard-svc; 68 keyboard events sink.  Empty / freed
 * per-window slots read back null or stale and WaitAnyQueue skips them,
 * so we can pass the full fixed count every iteration. */
#define WM_WAITSET_SLOT_OFFSET          1568
#define WM_WAITSET_BYTES                552       /* 69 slots × 8 bytes */
#define WM_WAITSET_COUNT                69
#define TAG_DATA                        0x4102

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

/* SCREEN dimensions — the host display surface. */
#define FB_W            1280
#define FB_H            768

/* Per-window title bar (Phase 60 step 8; regrown for the OPEN LOOK olwm
 * chrome).  Lives flush under the 2px black top border of each window's
 * backing store; the cell content area follows it.
 *
 * The bar's PIXEL height (24px, matching the OpenWindows reference) is now
 * decoupled from the chrome CELL budget.  TITLE_BAR_CELLS stays 1 so the
 * content cell area, CONTENT_Y_OFF_PX (=32) and USABLE_H_PX are unchanged
 * (the window size and compositor are untouched); the 32px of chrome above
 * the content is just REDISTRIBUTED: 2px border + 24px title bar + 1px
 * black separator + 5px BG1 frame pad = 32px (see the layout comment below
 * and paint_title_bar). */
#define TITLE_BAR_CELLS 1
#define TITLE_BAR_PX    24
/* The window-menu button and the focus stripe FLOAT inside the 24px bar:
 * TITLE_INSET px below the top border, TITLE_INNER_PX tall.  The reference's
 * name-stripe sits slightly HIGH — 3px above / 5px below — so the top inset and
 * the height are decoupled (not symmetric): top gap = TITLE_INSET (3px), bottom
 * gap = TITLE_BAR_PX - TITLE_INSET - TITLE_INNER_PX = 5px. */
#define TITLE_INSET     3     /* gap from the top border down to the float */
#define TITLE_INNER_PX  16    /* float (button/stripe) height */
/* Window-menu button: a small raised square (TITLE_INNER_PX tall) floating at
 * the bar's left, with the ▽ menu mark centred in it (paint_menu_button). */
#define MENU_BTN_W      TITLE_INNER_PX

/* Per-window border (Phase 60 step 15).  Each window FB has a
 * cell-aligned outer ring (1 cell wide on every side); inside that
 * ring sits a crisp BORDER_LINE_PX-thick gray line at the outermost
 * pixels.  Cell alignment lets ObjBlitGlyphs's cell-coord math
 * keep working — title bar text and content cells render with a
 * BORDER_CELLS_* offset that's a clean cell count, not a pixel
 * fraction.
 *
 * Layout inside each window FB (with BORDER_CELLS_X=BORDER_CELLS_Y=1,
 * BORDER_LINE_PX=2, TITLE_BAR_PX=24).  The chrome above the content is the
 * same 32px (2 cell rows) as before, just redistributed for the olwm look:
 *   y px       what
 *   0..1       top border line                              (2 px)
 *   2..25      title bar (flush under the border)           (24 px)
 *   26         title / content separator (1px black)        (1 px)
 *   27..31     BG1 frame pad                                (5 px bg)
 *   32..415    cell content area (24 rows × 16 px)          (384 px)
 *   416..429   bottom border ring padding                   (14 px bg)
 *   430..431   bottom border line                           (2 px)
 *   x px
 *   0..1       left border line                             (2 px)
 *   2..7       left border ring padding                     (6 px bg)
 *   8..647     cell columns (80 × 8 px)                     (640 px)
 *   648..653   right border ring padding                    (6 px bg)
 *   654..655   right border line                            (2 px)
 *
 * Window FB total: 656 × 432. */
#define BORDER_CELLS_X  1
#define BORDER_CELLS_Y  1
#define BORDER_LINE_PX  2

#define N_COLS          80     /* per-window cell columns (content area) */
#define N_ROWS          24     /* per-window cell rows (content area) */
#define CELL_AREA_W_PX  (CELL_W * N_COLS)                 /* 640 */
#define CELL_AREA_H_PX  (CELL_H * N_ROWS)                 /* 384 */
#define CELL_CONTENT_PX CELL_AREA_H_PX                    /* alias for fb_scroll */

/* USABLE_W_PX / USABLE_H_PX now refer to the full WINDOW FB size
 * (including border ring + title bar + content area).  Used by
 * alloc_window_fb, the compositor, and recompose_after_destroy
 * for "the window's full screen footprint." */
#define USABLE_W_PX  ((N_COLS + 2 * BORDER_CELLS_X) * CELL_W)
                                                          /* 656 */
#define USABLE_H_PX  ((BORDER_CELLS_Y + TITLE_BAR_CELLS + N_ROWS \
                       + BORDER_CELLS_Y) * CELL_H)        /* 432 */

/* Cell offsets in cell coords — passed to ObjBlitGlyphs's
 * (cell_x, cell_y) packed arg so glyphs land inside the border
 * ring + below the title bar. */
#define CONTENT_CELL_X_OFF  BORDER_CELLS_X
#define CONTENT_CELL_Y_OFF  (BORDER_CELLS_Y + TITLE_BAR_CELLS)
#define TITLE_CELL_X_OFF    BORDER_CELLS_X
#define TITLE_CELL_Y_OFF    BORDER_CELLS_Y

/* Pixel offsets — derived. */
#define CONTENT_X_OFF_PX    (CONTENT_CELL_X_OFF * CELL_W)
#define CONTENT_Y_OFF_PX    (CONTENT_CELL_Y_OFF * CELL_H)
#define TITLE_X_OFF_PX      (TITLE_CELL_X_OFF * CELL_W)
/* Title bar is now FLUSH under the 2px black top border (was one cell row
 * = 16px below it); pixel-positioned, decoupled from TITLE_CELL_Y_OFF. */
#define TITLE_Y_OFF_PX      BORDER_LINE_PX
/* Title text top — vertically centre the 16px font face in the floating stripe
 * (TITLE_INSET below the border), so it rides up with the button + stripe. */
#define TITLE_TEXT_Y_OFF_PX (TITLE_Y_OFF_PX + TITLE_INSET + (TITLE_INNER_PX - CELL_H) / 2)

/* OPEN LOOK title-bar element X positions (window-local px; origin = the window
 * top-left INCLUDING the 2px black border).  Sampled from the OpenWindows
 * reference: the recessed focus STRIPE / bar base / separator start at
 * TITLE_BAR_X and span the bar (TITLE_BAR_W); the raised menu BUTTON sits ON
 * TOP of the stripe at MENU_BTN_X (so the stripe shows to its left + right).
 * Content cells stay at TITLE_X_OFF_PX (=8, cell-aligned) — the bar is pixel-
 * positioned and a touch wider on the left. */
#define TITLE_BAR_X   5     /* bar base / focus stripe / separator left edge */
#define MENU_BTN_X    14    /* menu button left edge (over the stripe)        */
/* Bar spans SYMMETRICALLY — inset TITLE_BAR_X (2px border + 3px pad) on BOTH
 * sides, so the stripe's white right edge lands at wxmax-5, mirroring the BG3
 * left edge at wx5 (right pad = 3px BG1, then the 2px black border). */
#define TITLE_BAR_W   (USABLE_W_PX - 2 * TITLE_BAR_X)                  /* 646 */

/* Per-window border occupies the entire visible chrome now — there
 * is no screen-wide chrome any more.  CELL_ORIGIN_* survives only
 * as the minimum cascade position (small aesthetic margin so the
 * first window doesn't slam into the absolute (0, 0) corner). */
#define CELL_ORIGIN_X   (CELL_W)         /* 8  — one cell column */
#define CELL_ORIGIN_Y   (CELL_H)         /* 16 — one cell row */

/* Palette indices into simorisc's VEC_PALETTE_HEX (8-bit indexed FB).
 * These are INDICES, not RGB — the RGB lives only in VEC_PALETTE_HEX;
 * keep the two in sync.  Indices 1-8 are the dev accent ramp (mouse_paint
 * cycles them); the OPEN LOOK gray-on-blue scheme lives at 9-14 (the
 * palette PR — matched to the real OpenWindows 3 shot,
 * docs/images/openwindows_ipx.png).
 *
 * The terminal body keeps its existing colors (WM_BG/FG) so the cell
 * grid is untouched; only the chrome (workspace, frame, title) moves to
 * the OPEN LOOK indices. */
#define WM_BG_COLOR        0    /* terminal-body / window-content bg (navy) */
#define WM_FG_COLOR        1    /* terminal-body text (light gray) */
#define WM_WORKSPACE_COLOR 9    /* desktop background — OPEN LOOK blue #40a0c0 */
#define WM_FACE_BG1        10   /* window face / unfocused title — BG1 #cccccc */
#define WM_FACE_WHITE      11   /* olgx button highlight / pane — White #f5f5f5 */
#define WM_FACE_BG2        12   /* olgx button mid — BG2 #b8b8b8 */
#define WM_FACE_BG3        13   /* olgx button shadow — BG3 #666666 */
#define WM_OL_WHITE        8    /* pure white #ffffff (unused by the title bar
                                 * since the olwm focus rework — kept as a
                                 * palette slot) */
#define WM_OL_BLACK        14   /* text / window border / separator #000000 */
#define WM_BORDER_COLOR    WM_OL_BLACK  /* flat 2px black olwm window frame */
/* Title bar uses inverse-video: bar bg = fg-gray, text = bg-navy.
 * (Vestigial — paint_title_bar uses the WM_TITLE_* indices below.) */
#define WM_TITLE_BAR_BG WM_FG_COLOR
#define WM_TITLE_BAR_FG WM_BG_COLOR

/* Focus indicator — olwm's drawHeaderBar3D / drawHeaderNoFocus3D model
 * (winframe.c:1005-1024), replacing the old pure-white focused bar.
 *
 * The bar's base is the FLAT gray window face (BG1) for BOTH focus states.
 * Focus is shown by a RECESSED stripe drawn across the title-text area of
 * the FOCUSED window only — draw_bevel_box in PRESSED mode (BEVEL_FILL,
 * no BEVEL_RAISED): BG3 #666 top-left edge / BG2 #b8b8b8 face / white
 * bottom-right edge.  Unfocused windows stay flat BG1.  Title text is
 * black on either.  Verified vs docs/images/openwindows_ipx.png: focused
 * emacs name-stripe = BG3 top / BG2 face / white bottom; unfocused cmdtool
 * bar = flat BG1. */
#define WM_TITLE_BG            WM_FACE_BG1   /* flat title-bar base, both states */
#define WM_TITLE_STRIPE_FACE   WM_FACE_BG2   /* recessed focus-stripe face (BG2) */
#define WM_TITLE_TEXT_FG       WM_OL_BLACK   /* black title text */

/* Window-menu button mark — the OPEN LOOK ▽ vertical menu mark, drawn as the
 * authentic 3-layer olgx glyph (olgx_draw_menu_mark, 3D path, ol_button.c): the
 * UL edge (olgl cp 45) in BG3, the LR edge (cp 46) in white, the fill (cp 47) in
 * BG2 — a 7x7 ENGRAVED ▽ centred on the raised button.  This replaces the
 * whole-button cp 22 glyph (OLG_ABBREV_MENU_BUTTON), which is the 2D mono button
 * sprite — drawing it on top of our draw_bevel_box double-drew the box.  A
 * left-click (SELECT) destroys the window — interim behaviour from the old "[X]"
 * (real OPEN LOOK SELECT iconifies).  All three layers are 7x7 anchored
 * top-left in the olgl cell. */
#define OL_MENU_MARK_UL_CP    45   /* ▽ upper-left edge  — drawn BG3   */
#define OL_MENU_MARK_LR_CP    46   /* ▽ lower-right edge — drawn white */
#define OL_MENU_MARK_FILL_CP  47   /* ▽ interior fill    — drawn BG2   */
#define OL_MENU_MARK_W   7
#define OL_MENU_MARK_H   7

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
 *   python3 tools/gen_wm_font.py --pcf lutRS10.pcf > /tmp/font.txt
 * then paste the array below.  (--font PATH / --preset rasterize a
 * TTF via ImageMagick; --pcf reads an X11 bitmap font directly via
 * tools/pcf.py.)
 *
 * Source font: OpenWindows Lucida Sans Typewriter — the real X11
 * bitmap (B&H "lucidatypewriter" medium-R, 14px), from Solaris 2.6
 * SUNWxwoft, 100dpi/lutRS10.pcf.  Replaces the prior ImageMagick
 * rasterization of the LucidaSansTypewriter TTF @ 11pt: these are
 * the hand-tuned period bitmaps, crisper in the 8×16 cell.
 *
 * Glyphs that fall outside [32, 126] render as a blank cell.  '\n'
 * advances the cursor without rendering; other control chars are
 * silently dropped. */
static const unsigned int font_8x16[95][4] = {
    /*  \x20 */ { 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    /*   '!' */ { 0x00000808, 0x08080808, 0x08000008, 0x00000000 },
    /*   '"' */ { 0x00002424, 0x24000000, 0x00000000, 0x00000000 },
    /*   '#' */ { 0x00001212, 0x167f2424, 0xfe684848, 0x00000000 },
    /*   '$' */ { 0x00081e28, 0x2838180c, 0x0a0a2a3c, 0x08000000 },
    /*   '%' */ { 0x00006192, 0x949c6810, 0x36294986, 0x00000000 },
    /*   '&' */ { 0x00001c22, 0x223c70d8, 0x8c87c33d, 0x00000000 },
    /*   "'" */ { 0x00001818, 0x08180000, 0x00000000, 0x00000000 },
    /*   '(' */ { 0x0000060c, 0x10302020, 0x20202010, 0x10080600 },
    /*   ')' */ { 0x00006030, 0x08080404, 0x04040408, 0x08106000 },
    /*   '*' */ { 0x0000185a, 0x66143c24, 0x00000000, 0x00000000 },
    /*   '+' */ { 0x00000000, 0x00080808, 0x7f080808, 0x00000000 },
    /*   ',' */ { 0x00000000, 0x00000000, 0x00001818, 0x08081000 },
    /*   '-' */ { 0x00000000, 0x00000000, 0x7f000000, 0x00000000 },
    /*   '.' */ { 0x00000000, 0x00000000, 0x00000018, 0x00000000 },
    /*   '/' */ { 0x00000102, 0x02040408, 0x08081010, 0x20204000 },
    /*   '0' */ { 0x00001c22, 0x41414141, 0x4141221c, 0x00000000 },
    /*   '1' */ { 0x00001878, 0x08080808, 0x0808087f, 0x00000000 },
    /*   '2' */ { 0x00003c22, 0x0202060c, 0x0810303e, 0x00000000 },
    /*   '3' */ { 0x00003c22, 0x02021c06, 0x0202063c, 0x00000000 },
    /*   '4' */ { 0x00000c0c, 0x1c142444, 0x7e040404, 0x00000000 },
    /*   '5' */ { 0x00003e20, 0x20203806, 0x0202063c, 0x00000000 },
    /*   '6' */ { 0x00001e22, 0x20405e63, 0x4141231e, 0x00000000 },
    /*   '7' */ { 0x00007e06, 0x040c0808, 0x10102020, 0x00000000 },
    /*   '8' */ { 0x00003c42, 0x4266382c, 0x4242623c, 0x00000000 },
    /*   '9' */ { 0x00003c62, 0x4141633d, 0x0102023c, 0x00000000 },
    /*   ':' */ { 0x00000000, 0x00080000, 0x00000008, 0x00000000 },
    /*   ';' */ { 0x00000000, 0x00080000, 0x00000808, 0x08080800 },
    /*   '<' */ { 0x00000000, 0x00010618, 0x60180601, 0x00000000 },
    /*   '=' */ { 0x00000000, 0x0000007f, 0x007f0000, 0x00000000 },
    /*   '>' */ { 0x00000000, 0x0040300c, 0x030c3040, 0x00000000 },
    /*   '?' */ { 0x00007c42, 0x02060c10, 0x10000008, 0x00000000 },
    /*   '@' */ { 0x00001e61, 0x47899193, 0x97cd623e, 0x00000000 },
    /*   'A' */ { 0x00001818, 0x18242424, 0x7e424281, 0x00000000 },
    /*   'B' */ { 0x00007c42, 0x42467c44, 0x4242427c, 0x00000000 },
    /*   'C' */ { 0x00001f31, 0x20404040, 0x4020301f, 0x00000000 },
    /*   'D' */ { 0x00007c42, 0x41414141, 0x4141427c, 0x00000000 },
    /*   'E' */ { 0x00003f20, 0x20203e20, 0x2020203f, 0x00000000 },
    /*   'F' */ { 0x00003f20, 0x2020203e, 0x20202020, 0x00000000 },
    /*   'G' */ { 0x00001f31, 0x20404041, 0x4121311f, 0x00000000 },
    /*   'H' */ { 0x00004242, 0x42427e42, 0x42424242, 0x00000000 },
    /*   'I' */ { 0x00003e08, 0x08080808, 0x0808083e, 0x00000000 },
    /*   'J' */ { 0x00003c04, 0x04040404, 0x04040478, 0x00000000 },
    /*   'K' */ { 0x00004244, 0x48507050, 0x584c4643, 0x00000000 },
    /*   'L' */ { 0x00002020, 0x20202020, 0x2020203f, 0x00000000 },
    /*   'M' */ { 0x00006363, 0x63635555, 0x55554941, 0x00000000 },
    /*   'N' */ { 0x00004262, 0x6252524a, 0x4a464642, 0x00000000 },
    /*   'O' */ { 0x00003c42, 0xc3818181, 0x81c3423c, 0x00000000 },
    /*   'P' */ { 0x00003e21, 0x2121223c, 0x20202020, 0x00000000 },
    /*   'Q' */ { 0x00003c42, 0xc2818181, 0x81c3433e, 0x04030100 },
    /*   'R' */ { 0x00007c42, 0x42424478, 0x4c444243, 0x00000000 },
    /*   'S' */ { 0x00003c42, 0x4060300c, 0x0202467c, 0x00000000 },
    /*   'T' */ { 0x00007f08, 0x08080808, 0x08080808, 0x00000000 },
    /*   'U' */ { 0x00004242, 0x42424242, 0x4242423c, 0x00000000 },
    /*   'V' */ { 0x00008142, 0x42422424, 0x24181818, 0x00000000 },
    /*   'W' */ { 0x00008191, 0x91aaaa6a, 0x6a6a4444, 0x00000000 },
    /*   'X' */ { 0x0000c362, 0x24181818, 0x3c2442c3, 0x00000000 },
    /*   'Y' */ { 0x0000c342, 0x24241810, 0x10101010, 0x00000000 },
    /*   'Z' */ { 0x00007f02, 0x02040808, 0x1020207f, 0x00000000 },
    /*   '[' */ { 0x00001e10, 0x10101010, 0x10101010, 0x10101e00 },
    /*  '\\' */ { 0x00004020, 0x20101008, 0x08080404, 0x02020100 },
    /*   ']' */ { 0x00007808, 0x08080808, 0x08080808, 0x08087800 },
    /*   '^' */ { 0x00000808, 0x1c141422, 0x22410000, 0x00000000 },
    /*   '_' */ { 0x00000000, 0x00000000, 0x00000000, 0x00ff0000 },
    /*   '`' */ { 0x00001810, 0x18180000, 0x00000000, 0x00000000 },
    /*   'a' */ { 0x00000000, 0x003e2202, 0x3e62423f, 0x00000000 },
    /*   'b' */ { 0x00004040, 0x405c6642, 0x4242467c, 0x00000000 },
    /*   'c' */ { 0x00000000, 0x001e2040, 0x4040201e, 0x00000000 },
    /*   'd' */ { 0x00000202, 0x023e6242, 0x4242663a, 0x00000000 },
    /*   'e' */ { 0x00000000, 0x003c6642, 0x7e40221e, 0x00000000 },
    /*   'f' */ { 0x000f1010, 0x107f1010, 0x10101010, 0x00000000 },
    /*   'g' */ { 0x00000000, 0x001d2341, 0x4141633d, 0x01213e00 },
    /*   'h' */ { 0x00004040, 0x405c6242, 0x42424242, 0x00000000 },
    /*   'i' */ { 0x00000800, 0x00780808, 0x08080808, 0x00000000 },
    /*   'j' */ { 0x00000400, 0x003c0404, 0x04040404, 0x04047800 },
    /*   'k' */ { 0x00002020, 0x20262428, 0x382c2423, 0x00000000 },
    /*   'l' */ { 0x00007808, 0x08080808, 0x08080808, 0x00000000 },
    /*   'm' */ { 0x00000000, 0x005b6d49, 0x49494949, 0x00000000 },
    /*   'n' */ { 0x00000000, 0x005c6242, 0x42424242, 0x00000000 },
    /*   'o' */ { 0x00000000, 0x001c2241, 0x4141221c, 0x00000000 },
    /*   'p' */ { 0x00000000, 0x005c6642, 0x4242467c, 0x40404000 },
    /*   'q' */ { 0x00000000, 0x003e6242, 0x4242663a, 0x02020200 },
    /*   'r' */ { 0x00000000, 0x002e3220, 0x20202020, 0x00000000 },
    /*   's' */ { 0x00000000, 0x003e4060, 0x3c02427c, 0x00000000 },
    /*   't' */ { 0x00000010, 0x107e1010, 0x1010100e, 0x00000000 },
    /*   'u' */ { 0x00000000, 0x00424242, 0x4242463a, 0x00000000 },
    /*   'v' */ { 0x00000000, 0x00814242, 0x24241818, 0x00000000 },
    /*   'w' */ { 0x00000000, 0x0081999a, 0x6a666666, 0x00000000 },
    /*   'x' */ { 0x00000000, 0x00633614, 0x08142663, 0x00000000 },
    /*   'y' */ { 0x00000000, 0x00412222, 0x16141408, 0x08107000 },
    /*   'z' */ { 0x00000000, 0x007f0204, 0x0810207f, 0x00000000 },
    /*   '{' */ { 0x00000608, 0x08080808, 0x30080808, 0x08080600 },
    /*   '|' */ { 0x00000808, 0x08080808, 0x08080808, 0x08080800 },
    /*   '}' */ { 0x00003008, 0x08080808, 0x06080808, 0x08083000 },
    /*   '~' */ { 0x00000000, 0x00000031, 0x49460000, 0x00000000 },
};

/* === Font manager (OPEN LOOK chrome) ================================
 *
 * The terminal body renders through font_8x16 on the cell grid via the
 * LEGACY ObjBlitGlyphs path (unchanged, byte-identical).  Chrome — title
 * bars, and later menus / pushpins / bevels — renders through this font
 * manager: a self-describing font face + draw_string, on the EXTENDED
 * ObjBlitGlyphs path (proportional advance, absolute pixel positioning,
 * optional transparent background).  One firmware call still renders a
 * whole string; the WM never blits per-pixel.
 *
 * A wm_font_t is a thin descriptor over a `<name>_blob[]` the font
 * generator bakes (tools/gen_wm_font.py --face) — magic + header +
 * per-glyph width table + bitmaps, laid out exactly as the firmware's
 * _blit_glyphs_extended reads it.  cell_w/cell_h/base/n_glyphs are
 * cached here so the WM can MEASURE a proportional string (to centre a
 * title) without re-reading the blob header. */
typedef struct {
	const unsigned int *blob;  /* self-describing font object (read via O15) */
	int cell_w;                /* glyph bounding-box width, px */
	int cell_h;                /* rows per glyph */
	int base;                  /* first codepoint */
	int n_glyphs;
	int flags;                 /* bit0 = proportional */
} wm_font_t;

/* The baked faces (font_luRS proportional title face, font_lutRS mono
 * body face).  Generated — see scripts/gen_wm_fonts.sh.  Included here,
 * after wm_font_t is defined, so the initializers resolve. */
#include "wm_fonts.h"
/* The OPEN LOOK glyph face (font_olgl) — UI marks (pushpins, the ▽ menu
 * mark, scrollbar arrows, …).  Same wm_font_t descriptor, baked from
 * olgl12.bdf; the title bar's window-menu button renders codepoint 22. */
#include "wm_fonts_olgl.h"

/* FONT_MAGIC / flags / R5 shape bits — must match simorisc's extended
 * ObjBlitGlyphs path. */
#define FONT_FLAG_PROPORTIONAL  1
#define FONT_R5_EXTENDED        0x80000000u   /* R5 bit 31 selects ext mode */
#define FONT_R5_TRANSPARENT     0x40000000u   /* R5 bit 30: skip bg writes  */

/* Width-table byte offset within a face blob: magic(4) + header(12). */
#define FONT_WIDTHS_OFF  16

/* Advance width (px) of a single codepoint.  Reads the blob's width
 * table directly (the same bytes the firmware reads) so measuring and
 * rendering can never disagree.  Out-of-range codepoints advance by the
 * face's cell width — matching the firmware's unknown-glyph handling. */
static int
font_advance(const wm_font_t *font, int cp)
{
	int gi = cp - font->base;
	if (gi < 0 || gi >= font->n_glyphs) return font->cell_w;
	int byte_off = FONT_WIDTHS_OFF + gi;
	unsigned int word = font->blob[byte_off >> 2];
	return (word >> (8 * (3 - (byte_off & 3)))) & 0xFF;
}

/* Total pixel advance of the first `n` bytes of `text` in `font`. */
static int
font_measure(const wm_font_t *font, const unsigned char *text, int n)
{
	int w = 0, i;
	for (i = 0; i < n; i++) w += font_advance(font, text[i]);
	return w;
}

/* Pack the EXTENDED ObjBlitGlyphs R5 "shape" word: glyph count, fg/bg
 * palette indices, the transparent flag, and the mode-select bit. */
static int
font_shape(int n, int fg, int bg, int transparent)
{
	unsigned int s = FONT_R5_EXTENDED;
	if (transparent) s |= FONT_R5_TRANSPARENT;
	s |= ((unsigned int)(n  & 0x3FFF)) << 16;
	s |= ((unsigned int)(fg & 0xFF))   << 8;
	s |=  (unsigned int)(bg & 0xFF);
	return (int)s;
}

#define MAX_WINDOWS 16

/* === Per-window state in normal C globals ============================= */

/* window_id 0 is "invalid"; valid ids are 1..MAX_WINDOWS, mapped
 * to the [id-1] entry of these arrays. */
static int    window_type[MAX_WINDOWS];        /* WIN_TYPE_* (0 = free) */
static int    window_subscribe_op[MAX_WINDOWS]; /* notify_op (0 = none) */
static int    window_cur_col[MAX_WINDOWS];      /* cursor col within cell grid */
static int    window_cur_row[MAX_WINDOWS];      /* cursor row within cell grid */
static unsigned char window_vec_color[MAX_WINDOWS]; /* current pen palette idx */
/* Per-window TEXT pen (VEC_OP_TEXT_MOVE / _CHAR): content-relative position +
 * selected font face.  Only the WM holds the proportional width tables, so it
 * advances the pen per glyph — vec_text_char on the client just streams a
 * codepoint and the pen walks forward here. */
static int           window_text_x[MAX_WINDOWS];
static int           window_text_y[MAX_WINDOWS];
static unsigned char window_text_face[MAX_WINDOWS];

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

/* Phase 60 step 16/17 — outline-style window dragging.  While
 * drag_active is non-zero the WM tracks the pointer with a 2px
 * outline rectangle drawn directly on the screen FB; the actual
 * window stays put.  Button-up erases the outline and snaps the
 * window to drag_outline_x / drag_outline_y.
 *
 * drag_window_x / drag_window_y snapshot the window's position at
 * grab time so motion deltas are absolute from the grab point,
 * immune to compounding rounding.  drag_outline_x / drag_outline_y
 * track the outline's currently-painted position so the next
 * motion can erase the strips it occupies before painting at the
 * new spot. */
#define OUTLINE_PX        2
#define WM_OUTLINE_COLOR  8       /* palette idx 8 = bright white */
static int           drag_active;
static int           drag_wid;
static int           drag_start_x, drag_start_y;
static int           drag_window_x, drag_window_y;
static int           drag_outline_x, drag_outline_y;

/* Phase 60 step 8 / step 22 — per-window title bar text storage.
 * Was a single shared buffer through step 21; step 22 added title-bar
 * repaints on focus change (set_focus / refocus_to_topmost), which
 * re-blit the stored title — so a shared buffer painted the LAST
 * wm_set_title's text onto whichever window got focus next (visible
 * bug: closing a window stamped its title onto the new top window).
 * Per-wid storage fixes it: each window keeps its own title and
 * repaints reproduce it faithfully.  Lives in the data segment so
 * win_draw_string can reach it via O15 (boot data ref). */
static unsigned char window_titles[MAX_WINDOWS][MAX_TITLE_LEN];
static int           window_title_lens[MAX_WINDOWS];

/* Phase 60 step 19 — desktop root menu.  Old-school X11 style: right-
 * click on empty desktop summons a vertical list of program entries,
 * left-click an item to spawn it + dismiss, left-click off-menu or
 * ESC to cancel.  Hardcoded items for v1; reads-from-config /
 * client-registered items can come later.
 *
 * Spawn target uses sup_spawn (libc) which defaults to round-robin
 * across live CPUs — same SUP_TARGET_ANY semantics `run` uses.  WM
 * gets a supervisor cap lazily on first menu use via a dir_walk of
 * /sys/cpu/0/supervisor (the leader); installed into SUP_SLOT_OFFSET
 * (544) so libc's sup_have_supervisor / sup_spawn pick it up.
 *
 * Natural array-of-pointers shape — restored once asmorisc grew
 * R_ORISC_ABS32 relocations for `.word LABEL` fixups (Phase 60
 * step 21).  Earlier versions had to pack everything into char
 * buffers with parallel offset arrays because the .word LABEL
 * path resolved to `DATA_BASE + offset_in_obj` at assemble time,
 * ignoring this .oro's actual placement in the linked image. */
#define DESKTOP_MENU_N 6
static const char *const desktop_menu_labels[DESKTOP_MENU_N] = {
	"Shell",
	"Edit",
	"Mouse Paint",
	"Menu Demo",
	"Font Demo",
	"Cancel",
};
static const int desktop_menu_label_lens[DESKTOP_MENU_N] = {
	5, 4, 11, 9, 9, 6,
};
static const char *const desktop_menu_spawn_paths[DESKTOP_MENU_N] = {
	"/programs/shell.orx",
	"/programs/edit.orx",
	"/programs/mouse_paint.orx",
	"/programs/menudemo.orx",
	"/programs/font_demo.orx",
	(const char *)0,         /* Cancel — dismiss, no spawn */
};

/* Menu chrome: padding around each item in cells.  Width = max label
 * cells + 2 padding.  Height = N items × 1 row.  Cell-aligned so the
 * ObjBlitGlyphs cell-coord math stays clean. */
#define MENU_PAD_CELLS_X    1
#define MENU_BG_COLOR       1     /* light gray (matches title bar bg) */
#define MENU_FG_COLOR       0     /* dark navy (text over gray)        */
#define MENU_HI_BG_COLOR    8     /* bright white when hovered         */
#define MENU_HI_FG_COLOR    0     /* dark navy text over hilite        */

static int  menu_active;           /* non-zero while desktop menu showing */
static int  menu_x_cell, menu_y_cell;  /* top-left in CELL coords */
static int  menu_w_cells, menu_h_cells;
static int  menu_highlighted;     /* -1 = none, else 0..N-1 */

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
 * #0x10A ObjAllocFramebuffer primitive.  The resulting TAG_FRAMEBUFFER
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
		"call  #0x10A\n"               /* ObjAllocFramebuffer */
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

/* Phase 60 step 18 — focus model.  focused_wid is the window that
 * receives keyboard input and (by virtue of always being the topmost
 * window after a click) the window pointer events route to.  Updated
 * by handle_new_window, wm_handle_pointer (click-to-focus), and
 * handle_destroy_window (auto-revert on destroy). */
static int focused_wid;

/* Phase 60 step 22 — repaint wid's title bar with focus-appropriate
 * colors + composite it onto screen.  Defined after paint_title_bar;
 * forward-declared here because set_focus / refocus_to_topmost (both
 * above paint_title_bar in source order) call it. */
static void repaint_title_bar(int wid);

/* Move keyboard/pointer focus to new_wid, repainting the title bars
 * of both the previously-focused and newly-focused windows so the
 * brightness indicator tracks focus.  No-op if focus doesn't move. */
static void
set_focus(int new_wid)
{
	if (new_wid == focused_wid) return;
	int old = focused_wid;
	focused_wid = new_wid;
	repaint_title_bar(old);
	repaint_title_bar(new_wid);
}

/* Per-wid keyboard subscriber ref load/stash.  Same pcc-orisc dance
 * as load_window_fb_to_o1 — computed-offset OREFLD/OREFST isn't
 * legal, so each wid gets a switch case with a literal byte offset
 * (WM_KBD_SUB_BASE_OFFSET + (wid - 1) * 8). */
static void
load_kbd_sub_to_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 1312(o12)"); break;
	case  2: asm volatile("orefld o1, 1320(o12)"); break;
	case  3: asm volatile("orefld o1, 1328(o12)"); break;
	case  4: asm volatile("orefld o1, 1336(o12)"); break;
	case  5: asm volatile("orefld o1, 1344(o12)"); break;
	case  6: asm volatile("orefld o1, 1352(o12)"); break;
	case  7: asm volatile("orefld o1, 1360(o12)"); break;
	case  8: asm volatile("orefld o1, 1368(o12)"); break;
	case  9: asm volatile("orefld o1, 1376(o12)"); break;
	case 10: asm volatile("orefld o1, 1384(o12)"); break;
	case 11: asm volatile("orefld o1, 1392(o12)"); break;
	case 12: asm volatile("orefld o1, 1400(o12)"); break;
	case 13: asm volatile("orefld o1, 1408(o12)"); break;
	case 14: asm volatile("orefld o1, 1416(o12)"); break;
	case 15: asm volatile("orefld o1, 1424(o12)"); break;
	case 16: asm volatile("orefld o1, 1432(o12)"); break;
	default: asm volatile("onull o1"); break;
	}
}

static void
stash_kbd_sub_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o1, 1312(o12)"); break;
	case  2: asm volatile("orefst o1, 1320(o12)"); break;
	case  3: asm volatile("orefst o1, 1328(o12)"); break;
	case  4: asm volatile("orefst o1, 1336(o12)"); break;
	case  5: asm volatile("orefst o1, 1344(o12)"); break;
	case  6: asm volatile("orefst o1, 1352(o12)"); break;
	case  7: asm volatile("orefst o1, 1360(o12)"); break;
	case  8: asm volatile("orefst o1, 1368(o12)"); break;
	case  9: asm volatile("orefst o1, 1376(o12)"); break;
	case 10: asm volatile("orefst o1, 1384(o12)"); break;
	case 11: asm volatile("orefst o1, 1392(o12)"); break;
	case 12: asm volatile("orefst o1, 1400(o12)"); break;
	case 13: asm volatile("orefst o1, 1408(o12)"); break;
	case 14: asm volatile("orefst o1, 1416(o12)"); break;
	case 15: asm volatile("orefst o1, 1424(o12)"); break;
	case 16: asm volatile("orefst o1, 1432(o12)"); break;
	default: break;
	}
}

static void
load_ptr_sub_to_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefld o1, 1440(o12)"); break;
	case  2: asm volatile("orefld o1, 1448(o12)"); break;
	case  3: asm volatile("orefld o1, 1456(o12)"); break;
	case  4: asm volatile("orefld o1, 1464(o12)"); break;
	case  5: asm volatile("orefld o1, 1472(o12)"); break;
	case  6: asm volatile("orefld o1, 1480(o12)"); break;
	case  7: asm volatile("orefld o1, 1488(o12)"); break;
	case  8: asm volatile("orefld o1, 1496(o12)"); break;
	case  9: asm volatile("orefld o1, 1504(o12)"); break;
	case 10: asm volatile("orefld o1, 1512(o12)"); break;
	case 11: asm volatile("orefld o1, 1520(o12)"); break;
	case 12: asm volatile("orefld o1, 1528(o12)"); break;
	case 13: asm volatile("orefld o1, 1536(o12)"); break;
	case 14: asm volatile("orefld o1, 1544(o12)"); break;
	case 15: asm volatile("orefld o1, 1552(o12)"); break;
	case 16: asm volatile("orefld o1, 1560(o12)"); break;
	default: asm volatile("onull o1"); break;
	}
}

static void
stash_ptr_sub_o1(int wid)
{
	switch (wid) {
	case  1: asm volatile("orefst o1, 1440(o12)"); break;
	case  2: asm volatile("orefst o1, 1448(o12)"); break;
	case  3: asm volatile("orefst o1, 1456(o12)"); break;
	case  4: asm volatile("orefst o1, 1464(o12)"); break;
	case  5: asm volatile("orefst o1, 1472(o12)"); break;
	case  6: asm volatile("orefst o1, 1480(o12)"); break;
	case  7: asm volatile("orefst o1, 1488(o12)"); break;
	case  8: asm volatile("orefst o1, 1496(o12)"); break;
	case  9: asm volatile("orefst o1, 1504(o12)"); break;
	case 10: asm volatile("orefst o1, 1512(o12)"); break;
	case 11: asm volatile("orefst o1, 1520(o12)"); break;
	case 12: asm volatile("orefst o1, 1528(o12)"); break;
	case 13: asm volatile("orefst o1, 1536(o12)"); break;
	case 14: asm volatile("orefst o1, 1544(o12)"); break;
	case 15: asm volatile("orefst o1, 1552(o12)"); break;
	case 16: asm volatile("orefst o1, 1560(o12)"); break;
	default: break;
	}
}

/* Recompute focused_wid after a window is destroyed.  Falls back to
 * the topmost remaining window (top of window_z), or 0 if no windows
 * are left.  Repaints the newly-focused window's title bar so it
 * lights up — the just-destroyed window is already gone from the
 * z-stack, so there's no old bar to dim. */
static void
refocus_to_topmost(void)
{
	if (window_z_count > 0) {
		focused_wid = window_z[window_z_count - 1];
		repaint_title_bar(focused_wid);
	} else {
		focused_wid = 0;
	}
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
		"call  #0x10A\n"               /* ObjAllocFramebuffer */
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

/* Forward decl — fill_rect_packed lives further down (alongside the
 * paint_window_chrome helpers) but recompose_after_destroy needs it
 * to clear the destroyed-window screen rect to bg before
 * compositing the remaining z-stack. */
static void fill_rect_packed(int packed_xy, int packed_wh, int color);

/* Composite an arbitrary screen rect by walking z-order bottom-to-
 * top: each window's intersection with the rect is blitted from its
 * FB.  Used by composite_window_region (for paint-after-write) and
 * recompose_after_destroy (for vacated-pixel cleanup). */
static void
composite_screen_rect(int sx, int sy, int w, int h)
{
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

static void
composite_window_region(int wx, int wy, int w, int h)
{
	int wid = active_wid;
	if (wid < 1 || wid > MAX_WINDOWS) return;
	int sx = window_pos_x[wid - 1] + wx;
	int sy = window_pos_y[wid - 1] + wy;
	composite_screen_rect(sx, sy, w, h);
}

/* Phase 60 step 14 — vacated-pixel cleanup after a window is
 * destroyed.  Fills the destroyed window's screen rect with bg
 * (clears its content), then composites the rect from the
 * remaining z-stack so any windows beneath show through their
 * intersections.  Without this, closing a window leaves its
 * content painted on the screen until something else happens
 * to paint over those pixels. */
static void
recompose_after_destroy(int sx, int sy, int w, int h)
{
	int packed_xy = ((sx & 0xFFFF) << 16) | (sy & 0xFFFF);
	int packed_wh = ((w  & 0xFFFF) << 16) | (h  & 0xFFFF);
	fill_rect_packed(packed_xy, packed_wh, WM_WORKSPACE_COLOR);
	composite_screen_rect(sx, sy, w, h);
	/* No screen chrome border to repaint — per-window borders are
	 * inside each remaining window's FB and re-blitted as part of
	 * composite_screen_rect above. */
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
 * the title bar + border pixels untouched.  Used by the vector /
 * raster forwarders, which paint into the window FB at
 * client-visible (0..CELL_AREA_W_PX × 0..CELL_AREA_H_PX) coords;
 * they call this once per request rather than once per inner blit. */
static void
composite_content_area(void)
{
	composite_window_region(CONTENT_X_OFF_PX, CONTENT_Y_OFF_PX,
	                        CELL_AREA_W_PX, CELL_AREA_H_PX);
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

/* === OPEN LOOK 3D bevels ============================================
 *
 * The olgx 3-color bevel (libolgx ol_draw.c): every raised/recessed
 * chrome element composites three colors from the window's gray group
 * {White, BG1, BG2, BG3} (the palette PR).  RAISED = White highlight on
 * the top + left edges, BG1 face, BG3 shadow on the bottom + right
 * edges; PRESSED = inverted (BG3 top-left, BG2 face, White bottom-right).
 * The shadow edges are drawn last so they win the top-right and
 * bottom-left corners — the classic raised look.
 *
 * draw_bevel_box draws the four BEVEL_EDGE_PX-wide edges and, when the
 * BEVEL_FILL bit is set, the interior face first.  Edges-only (no FILL)
 * is the beveled FRAME form used for the window outline and the
 * title-bar strip — their interiors are already painted (the gray
 * window face; the focus-coded title bar), so the bevel only adds the
 * highlight/shadow ring.  All fills go through fill_rect_window
 * (#0x10D ObjFillRect) on the active window FB; a bevel is a handful of
 * cheap edge fills.
 *
 * 3-arg packed signature (pcc-orisc 5+-arg-call bug): packed_xy =
 * (x<<16)|y, packed_wh = (w<<16)|h, packed_mode = flag bits below. */
#define BEVEL_EDGE_PX  1        /* 1px chisel — matches the OpenWindows olgx look */
#define BEVEL_RAISED   0x01     /* else pressed / recessed */
#define BEVEL_FILL     0x02     /* fill the interior face before the edges */

static void
draw_bevel_box(int packed_xy, int packed_wh, int packed_mode)
{
	int x = (packed_xy >> 16) & 0xFFFF;
	int y =  packed_xy        & 0xFFFF;
	int w = (packed_wh >> 16) & 0xFFFF;
	int h =  packed_wh        & 0xFFFF;
	int e = BEVEL_EDGE_PX;
	int raised = packed_mode & BEVEL_RAISED;
	int hi = raised ? WM_FACE_WHITE : WM_FACE_BG3;    /* top-left edge */
	int lo = raised ? WM_FACE_BG3   : WM_FACE_WHITE;  /* bottom-right edge */

	if (packed_mode & BEVEL_FILL) {
		int face = raised ? WM_FACE_BG1 : WM_FACE_BG2;
		fill_rect_window(packed_xy, packed_wh, face);
	}
	/* top + left highlight (for raised) */
	fill_rect_window(((x & 0xFFFF) << 16) | (y & 0xFFFF),
	                 ((w & 0xFFFF) << 16) | (e & 0xFFFF), hi);
	fill_rect_window(((x & 0xFFFF) << 16) | (y & 0xFFFF),
	                 ((e & 0xFFFF) << 16) | (h & 0xFFFF), hi);
	/* bottom + right shadow (for raised) — last, so they win the corners */
	fill_rect_window(((x & 0xFFFF) << 16) | ((y + h - e) & 0xFFFF),
	                 ((w & 0xFFFF) << 16) | (e & 0xFFFF), lo);
	fill_rect_window((((x + w - e) & 0xFFFF) << 16) | (y & 0xFFFF),
	                 ((e & 0xFFFF) << 16) | (h & 0xFFFF), lo);
}

/* === Centralized glyph-blit emitters ================================
 *
 * One ObjBlitGlyphs (#0x10C) firmware call, fully described by
 * (packed_xy, packed_shape, font_off, text_off).  Both the LEGACY
 * cell-grid chrome callers (font_8x16, R5 bit 31 clear) and the
 * EXTENDED proportional font-manager caller (a wm_font_t face, R5 bit 31
 * set) funnel through these, so there is exactly one asm site per
 * destination FB.  O2 = O15 (the font blob lives in boot data) and
 * O3 = O15 (chrome text — titles, labels — also lives in boot data).
 * 4-arg signature dodges pcc-orisc's 5+-arg-call bug; the FB slot is an
 * asm immediate, not a runtime arg.
 *
 * (The terminal body strip in flush_strip keeps its own inline emitter:
 * its text is on the boot STACK, O11, not boot data, and it is the one
 * latency-critical render path — see flush_strip.) */
static void
blit_glyphs_winfb(int packed_xy, int packed_shape, int font_off, int text_off)
{
	asm volatile(
		"addu   r8,  %0, r0\n"
		"addu   r9,  %1, r0\n"
		"addu   r10, %2, r0\n"
		"addu   r11, %3, r0\n"
		"orefld o1, %4(o12)\n"      /* O1 = active window FB */
		"omov   o2, o15\n"          /* O2 = boot data (font) */
		"omov   o3, o15\n"          /* O3 = boot data (text) */
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

static void
blit_glyphs_screenfb(int packed_xy, int packed_shape, int font_off, int text_off)
{
	asm volatile(
		"addu   r8,  %0, r0\n"
		"addu   r9,  %1, r0\n"
		"addu   r10, %2, r0\n"
		"addu   r11, %3, r0\n"
		"orefld o1, %4(o12)\n"        /* O1 = screen FB */
		"omov   o2, o15\n"            /* O2 = boot data (font) */
		"omov   o3, o15\n"            /* O3 = boot data (text) */
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"addu   r7, r11, r0\n"
		"call   #0x10C\n"             /* ObjBlitGlyphs */
		"nop"
		:
		: "r"(packed_xy), "r"(packed_shape),
		  "r"(font_off),  "r"(text_off),
		  "i"(WM_SURF_FRAMEBUFFER_SLOT_OFFSET)
		: "r1", "r2", "r3", "r4", "r5", "r6", "r7",
		  "r8", "r9", "r10", "r11"
	);
}

/* Window-menu mark text — the three olgl ▽ menu-mark layers (UL/LR/fill), each
 * a single byte in boot data so win_draw_string can reach it via O15.  Rendered
 * through the EXTENDED font-manager path with font_olgl (not the legacy cell
 * path), one transparent blit per layer in its olgx color (BG3/white/BG2). */
static const unsigned char mm_ul_str[]   = { OL_MENU_MARK_UL_CP, 0 };
static const unsigned char mm_lr_str[]   = { OL_MENU_MARK_LR_CP, 0 };
static const unsigned char mm_fill_str[] = { OL_MENU_MARK_FILL_CP, 0 };

/* EXTENDED proportional string blit into the ACTIVE window FB.  `font`
 * is a baked face (e.g. font_luRS); packed_xy is an absolute pixel
 * position (pixel_x:high16, pixel_y:low16); packed_shape comes from
 * font_shape() (glyph count + fg/bg + transparent + EXTENDED bits).
 * Source text must live in boot data (O15) — window_titles and the
 * chrome string literals all do.  One firmware call renders the whole
 * string; the firmware advances the pen per-glyph from the face's
 * width table. */
static void
win_draw_string(const wm_font_t *font, int packed_xy, int packed_shape,
                const unsigned char *text)
{
	if (((packed_shape >> 16) & 0x3FFF) == 0) return;
	int text_off = (int)((unsigned int)text - DATA_VA);
	int font_off = (int)((unsigned int)font->blob - DATA_VA);
	blit_glyphs_winfb(packed_xy, packed_shape, font_off, text_off);
}

/* Phase 60 step 19 — screen-FB variant.  LEGACY cell-grid row blit into
 * the SCREEN framebuffer (no window backing store), using font_8x16.
 * Used by the desktop menu — its glyphs live above all windows in the
 * compositor order, so painting straight into the screen FB skips a
 * composite step entirely.  Same packing as the legacy cell-grid blit;
 * source text must live in boot data (O15). */
static void
screen_blit_glyph_row(int packed_xy, const unsigned char *text,
                      int packed_shape)
{
	if (((packed_shape >> 16) & 0xFFFF) == 0) return;
	int text_off = (int)((unsigned int)text - DATA_VA);
	int font_off = (int)((unsigned int)&font_8x16[0][0] - DATA_VA);
	blit_glyphs_screenfb(packed_xy, packed_shape, font_off, text_off);
}

/* OPEN LOOK window-menu button — a small RAISED beveled square FLOATING at the
 * LEFT of the title bar (inset TITLE_INSET px top/bottom), on EVERY window
 * (focus-independent), with the ▽ menu mark centred in it.  The raised face is
 * the olgx RAISED bevel (White top-left / BG1 face / BG3 bottom-right) via
 * draw_bevel_box.  The mark is the authentic 3-layer engraved ▽: its UL edge
 * (BG3), LR edge (white), and fill (BG2), each blit transparently so they
 * composite on the BG1 face (olgx_draw_menu_mark's 3D recipe).
 *
 * SELECT (left-click) on the button destroys the window — see
 * point_in_menu_button / wm_handle_pointer.  Targets the ACTIVE window FB
 * (caller set_active_window's first), same as the rest of paint_title_bar. */
static void
paint_menu_button(void)
{
	int bx = MENU_BTN_X;
	int by = TITLE_Y_OFF_PX + TITLE_INSET;
	int btn_xy = ((bx & 0xFFFF) << 16) | (by & 0xFFFF);
	int btn_wh = ((MENU_BTN_W & 0xFFFF) << 16) | (TITLE_INNER_PX & 0xFFFF);
	draw_bevel_box(btn_xy, btn_wh, BEVEL_RAISED | BEVEL_FILL);

	/* Centre the 7x7 ▽ in the MENU_BTN_W x TITLE_INNER_PX button and blit the
	 * three engraved layers (UL=BG3, LR=white, fill=BG2), transparently. */
	int gx = bx + (MENU_BTN_W - OL_MENU_MARK_W) / 2;
	int gy = by + (TITLE_INNER_PX - OL_MENU_MARK_H) / 2;
	int gxy = ((gx & 0xFFFF) << 16) | (gy & 0xFFFF);
	win_draw_string(&font_olgl, gxy, font_shape(1, WM_FACE_BG3,   WM_TITLE_BG, 1), mm_ul_str);
	win_draw_string(&font_olgl, gxy, font_shape(1, WM_FACE_WHITE, WM_TITLE_BG, 1), mm_lr_str);
	win_draw_string(&font_olgl, gxy, font_shape(1, WM_FACE_BG2,   WM_TITLE_BG, 1), mm_fill_str);
}

/* Phase 60 step 8 / OPEN LOOK olwm rework — paint the title bar at the top
 * of the window FB.  The bar is flush under the 2px black border, 24px
 * tall, with a 1px black separator below it.  Layout left-to-right: the
 * raised window-menu button (▽), then the centred proportional title text;
 * focus is shown by a recessed stripe behind the title area (focused) vs a
 * flat BG1 bar (unfocused) — the olwm drawHeaderBar3D / drawHeaderNoFocus3D
 * model.  After painting we composite the bar + separator onto the screen.
 *
 * Called from handle_new_window, wm_set_title, and repaint_title_bar
 * (focus changes).  active_wid must be set to the target window first
 * (set_active_window) — fill_rect_window / draw_bevel_box / win_draw_string
 * / composite_window_region all key off it. */
static void
paint_title_bar(void)
{
	int focused = (active_wid == focused_wid);

	/* Bar base: the flat gray window face (BG1) for BOTH focus states
	 * (olwm drawHeaderNoFocus3D).  Spans (TITLE_BAR_X, TITLE_BAR_W) — wide
	 * enough to also clear the recessed stripe when a window defocuses. */
	int bar_xy = ((TITLE_BAR_X & 0xFFFF) << 16)
	           | (TITLE_Y_OFF_PX & 0xFFFF);
	int bar_wh = ((TITLE_BAR_W & 0xFFFF) << 16)
	           | (TITLE_BAR_PX & 0xFFFF);
	fill_rect_window(bar_xy, bar_wh, WM_TITLE_BG);

	/* Focus indicator (olwm drawHeaderBar3D): the FOCUSED window gets a
	 * RECESSED stripe spanning the WHOLE bar (TITLE_BAR_X..+TITLE_BAR_W),
	 * inset TITLE_INSET px top/bottom so it floats — draw_bevel_box PRESSED
	 * (BEVEL_FILL, no BEVEL_RAISED) = BG3 top-left / BG2 face / white
	 * bottom-right.  The menu button paints OVER it.  Unfocused = flat BG1. */
	if (focused) {
		int stripe_xy = ((TITLE_BAR_X & 0xFFFF) << 16)
		              | ((TITLE_Y_OFF_PX + TITLE_INSET) & 0xFFFF);
		int stripe_wh = ((TITLE_BAR_W & 0xFFFF) << 16)
		              | (TITLE_INNER_PX & 0xFFFF);
		draw_bevel_box(stripe_xy, stripe_wh, BEVEL_FILL);
	}

	/* Raised window-menu button (▽) ON TOP of the stripe, at MENU_BTN_X. */
	paint_menu_button();

	/* Title text centres in the span to the RIGHT of the menu button (with a
	 * TITLE_INSET gap after the button and before the bar's right edge). */
	int span_x = MENU_BTN_X + MENU_BTN_W + TITLE_INSET;
	int span_w = (TITLE_BAR_X + TITLE_BAR_W) - span_x - TITLE_INSET;

	/* Title text — proportional Lucida Sans (font_luRS), absolute-pixel
	 * positioned, centred in the span right of the menu button and
	 * vertically centred in the 24px bar.  Rendered transparently so the
	 * flat fill / recessed stripe shows through the gaps between glyphs.
	 * Title bytes come from the ACTIVE window's own slot (per-wid) and
	 * live in boot data (O15). */
	if (active_wid >= 1 && active_wid <= MAX_WINDOWS) {
		const unsigned char *title = window_titles[active_wid - 1];
		int n0 = window_title_lens[active_wid - 1];
		int title_px = 0, n = 0;
		while (n < n0) {
			int adv = font_advance(&font_luRS, title[n]);
			if (title_px + adv > span_w) break;
			title_px += adv;
			n++;
		}
		if (n > 0) {
			int start_x = span_x + (span_w - title_px) / 2;
			int packed_xy = ((start_x & 0xFFFF) << 16)
			              | (TITLE_TEXT_Y_OFF_PX & 0xFFFF);
			int text_bg = focused ? WM_TITLE_STRIPE_FACE : WM_TITLE_BG;
			int packed_shape = font_shape(n, WM_TITLE_TEXT_FG, text_bg, 1);
			win_draw_string(&font_luRS, packed_xy, packed_shape, title);
		}
	}

	/* 1px black separator directly below the bar (window-local
	 * y = TITLE_Y_OFF_PX + TITLE_BAR_PX), spanning the bar width. */
	{
		int sep_xy = ((TITLE_BAR_X & 0xFFFF) << 16)
		           | ((TITLE_Y_OFF_PX + TITLE_BAR_PX) & 0xFFFF);
		int sep_wh = ((TITLE_BAR_W & 0xFFFF) << 16) | 1;
		fill_rect_window(sep_xy, sep_wh, WM_OL_BLACK);
	}

	/* Composite the bar + the separator row below it. */
	composite_window_region(TITLE_BAR_X, TITLE_Y_OFF_PX,
	                        TITLE_BAR_W, TITLE_BAR_PX + 1);
}

/* Phase 60 step 22 — repaint wid's title bar (focus-color refresh).
 * Saves/restores active_wid via set_active_window so callers that
 * weren't mid-paint don't get their active window clobbered. */
static void
repaint_title_bar(int wid)
{
	if (wid < 1 || wid > MAX_WINDOWS) return;
	if (window_type[wid - 1] == 0) return;
	set_active_window(wid);
	paint_title_bar();
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

/* Paint the screen chrome: a 2-pixel border line just inside the
 * outer pixel edges of the screen FB.  Frames the whole desktop —
 * windows live inside the chrome's inner area.
 *
 * Phase 60 step 14: was using USABLE_W_PX / USABLE_H_PX (the
 * per-window size) which post-step-14 are 640/400 rather than the
 * screen-sized 1280/768.  Result: chrome was a small 644×404 frame
 * stuck in the top-left, and any window whose cascade offset
 * crossed its right/bottom edges would erase part of it on destroy.
 * Now uses FB_W/FB_H so the frame surrounds the whole screen.
 *
 * Border layout (BORDER_LINE_PX=2, CELL_ORIGIN=8/16):
 *   top:    y ∈ [14, 16)         left:  x ∈ [6, 8)
 *   bottom: y ∈ [752, 754)       right: x ∈ [1272, 1274)
 * Lines extend across the full screen so the four corners overlap
 * and form a clean rectangle. */
/* Phase 60 step 15 — boot-time desktop paint.  Just bg fill;
 * decoration is per-window now, not screen-wide. */
static void
paint_window_chrome(void)
{
	fill_rect_packed(((0 & 0xFFFF) << 16) | (0 & 0xFFFF),
	                 ((FB_W & 0xFFFF) << 16) | (FB_H & 0xFFFF),
	                 WM_WORKSPACE_COLOR);
}

/* Paint the window's outer frame: a flat BORDER_LINE_PX-wide (2px) BLACK
 * border at the outermost pixels of the window FB.  Sampled from the real
 * OpenWindows shot (docs/images/openwindows_ipx.png): the olwm window
 * frame is a SOLID 2px black outline — NOT a 3D bevel.  OPEN LOOK reserves
 * beveling for buttons/controls (the window-menu button, oblong buttons,
 * scrollbar elevators), never the window frame or title bar.  Edges only;
 * the interior is the BG1 face + title bar + content.  Called from
 * handle_new_window after the title bar.  Window-local coords, targets
 * WM_ACTIVE_FB_SLOT (same as paint_title_bar's fills). */
static void
paint_window_border(void)
{
	int w = USABLE_W_PX, h = USABLE_H_PX;
	int e = BORDER_LINE_PX;   /* 2px, per the OpenWindows olwm frame */

	fill_rect_window(((0 & 0xFFFF) << 16) | (0 & 0xFFFF),
	                 ((w & 0xFFFF) << 16) | e, WM_OL_BLACK);            /* top    */
	fill_rect_window(((0 & 0xFFFF) << 16) | ((h - e) & 0xFFFF),
	                 ((w & 0xFFFF) << 16) | e, WM_OL_BLACK);            /* bottom */
	fill_rect_window(((0 & 0xFFFF) << 16) | (0 & 0xFFFF),
	                 (e << 16) | (h & 0xFFFF), WM_OL_BLACK);            /* left   */
	fill_rect_window((((w - e) & 0xFFFF) << 16) | (0 & 0xFFFF),
	                 (e << 16) | (h & 0xFFFF), WM_OL_BLACK);            /* right  */
}

/* Palette PR — paint the window's gray FACE (BG1) across the whole
 * backing store, then restore the cell-content area to the terminal bg
 * so the console interior is unchanged.  The title bar + border paint on
 * top, giving the OPEN LOOK light-gray window frame on the blue
 * workspace.  Flat fills — the OPEN LOOK frame/title bar are flat (the
 * White/BG2/BG3 bevel colors are for buttons/controls, not the frame).
 * Called once at window creation (handle_new_window) before
 * paint_title_bar; VEC/RST_OP_CLEAR only repaint the content area, so the
 * face persists for the window's lifetime. */
static void
paint_window_face(void)
{
	int all_xy = ((0 & 0xFFFF) << 16) | (0 & 0xFFFF);
	int all_wh = ((USABLE_W_PX & 0xFFFF) << 16) | (USABLE_H_PX & 0xFFFF);
	fill_rect_window(all_xy, all_wh, WM_FACE_BG1);
	int c_xy = ((CONTENT_X_OFF_PX & 0xFFFF) << 16)
	         | (CONTENT_Y_OFF_PX & 0xFFFF);
	int c_wh = ((CELL_AREA_W_PX & 0xFFFF) << 16)
	         | (CELL_AREA_H_PX & 0xFFFF);
	fill_rect_window(c_xy, c_wh, WM_BG_COLOR);
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

	/* Push to top of z-stack and mark active for the title-bar paint.
	 * A new window takes focus immediately (Phase 60 step 18) — that
	 * way the program calling wm_open_session + term_init will land
	 * its keyboard subscribe in its own freshly-opened slot. */
	window_z[window_z_count] = wid;
	window_z_count += 1;

	/* Phase 60 step 22 — dim the previously-focused window's title
	 * bar before this one steals focus, so only one bar reads as
	 * focused at a time.  Done before set_active_window(wid) below
	 * since repaint_title_bar reassigns active_wid. */
	{
		int prev_focus = focused_wid;
		focused_wid = wid;
		if (prev_focus && prev_focus != wid)
			repaint_title_bar(prev_focus);
	}
	set_active_window(wid);

	/* Palette PR — lay down the gray window face first (the freshly
	 * allocated FB is zero-filled = terminal bg); the title bar + border
	 * paint on top of it. */
	paint_window_face();

	/* Phase 60 step 8 — paint the title bar so the window is visibly
	 * framed from creation.  Title is initially empty; the client
	 * (typically the shell or the spawning supervisor) wm_set_title's
	 * to fill it in.  Reset this wid's title slot in case the id was
	 * recycled from a destroyed window. */
	window_title_lens[wid - 1] = 0;
	paint_title_bar();

	/* Phase 60 step 13 — composite the full window onto the screen
	 * so the (zero-filled = bg-colored) content area replaces
	 * whatever underlying window's pixels were at this screen
	 * position.  Without this, a new window stacked on top of an
	 * older one leaves the older one's text "shining through" the
	 * un-painted parts of the new content area until something
	 * actually renders there.  The composite walks z-order so
	 * higher-z windows still correctly overpaint. */
	composite_window_region(CONTENT_X_OFF_PX, CONTENT_Y_OFF_PX,
	                        CELL_AREA_W_PX, CELL_AREA_H_PX);
	/* Paint the per-window border ring inside the FB + composite
	 * the whole window so the border + bg padding around it land
	 * on screen too. */
	paint_window_border();
	composite_window_region(0, 0, USABLE_W_PX, USABLE_H_PX);

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

/* Core window teardown shared by the WM_OP_DESTROY_WINDOW handler
 * and the title-bar close-box click path (Phase 60 step 22).  Frees
 * the per-wid surfaces, removes the window from the z-stack, nulls
 * its subscriber slots, refocuses, and recomposites the vacated
 * screen rect.  Does NOT wm_reply — the close-box path has no
 * client reply_cap to answer.  Assumes wid is valid + live. */
static void
window_teardown(int wid)
{
	int sx = window_pos_x[wid - 1];
	int sy = window_pos_y[wid - 1];
	free_window_console(wid);
	free_window_grid(wid);
	free_window_vector(wid);
	free_window_raster(wid);
	free_window_fb(wid);
	window_z_remove(wid);
	window_type[wid - 1] = 0;
	window_subscribe_op[wid - 1] = 0;
	/* Phase 60 step 18 — null per-wid kbd / ptr subscriber slots so
	 * future events for a recycled wid don't land on a dead ref.  If
	 * the destroyed window was focused, refocus to the next topmost
	 * remaining window (= z-stack top after the remove above). */
	asm volatile("onull o1");
	stash_kbd_sub_o1(wid);
	asm volatile("onull o1");
	stash_ptr_sub_o1(wid);
	if (focused_wid == wid) refocus_to_topmost();
	/* Owner-ref stash is left in place; future allocations will
	 * overwrite it.  No SEND fires for the close — the WM doesn't
	 * push events to subscribers yet. */
	recompose_after_destroy(sx, sy, USABLE_W_PX, USABLE_H_PX);
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
	window_teardown(wid);
	wm_reply(0, 0, 0, 0);
}

/* Phase 60 step 8 — WM_OP_SET_TITLE.  Wire shape:
 *   R5 = wid (0 = first live window)
 *   R6 = packed (len:high16, src_off:low16)
 *   O2 = source ref containing the title bytes
 *
 * ObjFetchBytes the bytes into the target wid's window_titles[] slot,
 * repaint the title bar, reply with status.  Same source-ref stash
 * dance as forward_console_write — pcc-orisc may spill R4..R6 to
 * stack across a function-call boundary but OPRs survive, so O2 is
 * still the caller's source ref at handler entry. */
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
		 * mapping's W bit into the persistent window_titles slot.
		 * Same trick forward_console_write uses. */
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
			window_titles[wid - 1][i] = fetch_buf[i];
	}
	window_title_lens[wid - 1] = len;
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
			int sx = window_pos_x[wid - 1];
			int sy = window_pos_y[wid - 1];
			free_window_console(wid);
			free_window_grid(wid);
			free_window_vector(wid);
			free_window_raster(wid);
			free_window_fb(wid);
			window_z_remove(wid);
			window_type[wid - 1] = 0;
			window_subscribe_op[wid - 1] = 0;
			/* Same focus + subscriber cleanup as the explicit
			 * handle_destroy_window path. */
			asm volatile("onull o1");
			stash_kbd_sub_o1(wid);
			asm volatile("onull o1");
			stash_ptr_sub_o1(wid);
			if (focused_wid == wid) refocus_to_topmost();
			recompose_after_destroy(sx, sy,
			                        USABLE_W_PX, USABLE_H_PX);
			/* The owner-ref slot stays populated; it'll get
			 * overwritten by the next allocation in this slot.
			 * No SEND fires for the close — the WM doesn't push
			 * events to subscribers yet. */
		}
	}
}

/* === Main loop ======================================================== */

/* Phase 61 — the WM is event-driven.  Instead of blocking on the main
 * service queue with a WM_POLL_TICKS timeout and draining the other
 * queues only between polls (a ~100ms interactive-latency floor), the
 * loop blocks on WaitAnyQueue (#0x206) over EVERY queue the WM owns,
 * then drains them all with non-blocking polls.  Any keystroke, pointer
 * event, console/grid/vector/raster write, or service request wakes the
 * WM the instant it lands — no timeout floor, and a truly idle WM burns
 * no instructions (the firmware parks the CPU). */

/* Allocate the wait-set OBJSTORE once at boot and park its ref in the
 * WM_WAITSET_SLOT.  Sized for the full fixed wait-set layout (69 ×
 * 8-byte ref slots). */
static int
alloc_wait_set(void)
{
	int status;
	asm volatile(
		"addiu r4, r0, %1\n"           /* size = WM_WAITSET_BYTES */
		"addiu r5, r0, %2\n"           /* TAG_DATA (objstore flag set by op) */
		"addiu r6, r0, %3\n"           /* CAP_R | CAP_W */
		"call  #0x106\n"               /* ObjAllocStore -> O1 */
		"nop\n"
		"orefst o1, %4(o12)\n"         /* stash ref in the wait-set slot */
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(WM_WAITSET_BYTES), "i"(TAG_DATA),
		  "i"(CAP_R | CAP_W), "i"(WM_WAITSET_SLOT_OFFSET)
		: "r1", "r2", "r4", "r5", "r6"
	);
	return status;
}

/* Refill the wait-set OBJSTORE (loaded into O2, which doubles as the
 * OREFST base and the WaitAnyQueue argument) from the WM's current
 * queue caps, then block until any of them is non-empty.  Returns the
 * WaitAnyQueue status (ERR_OK once something is ready).
 *
 * Every source is a fixed O12 offset and every destination a fixed
 * wait-set offset, so this is straight-line OREFLD/OREFST — no
 * computed offsets (which pcc-orisc rejects) and no switch tables.
 * Empty/freed per-window slots copy through as null/stale refs that
 * WaitAnyQueue skips, so we always pass the full fixed count. */
static int
wm_block_until_ready(void)
{
	int status;

	/* O2 = wait-set objstore: the OREFST base below and the call arg. */
	asm volatile("orefld o2, %0(o12)" :: "i"(WM_WAITSET_SLOT_OFFSET));

	/* slot 0 = main service queue (O9). */
	asm volatile("orefst o9, 0(o2)");

	/* console queues: O12 312.. -> wait_set 8.. */
	asm volatile(
		"orefld o14, 312(o12)\n orefst o14, 8(o2)\n"
		"orefld o14, 320(o12)\n orefst o14, 16(o2)\n"
		"orefld o14, 328(o12)\n orefst o14, 24(o2)\n"
		"orefld o14, 336(o12)\n orefst o14, 32(o2)\n"
		"orefld o14, 344(o12)\n orefst o14, 40(o2)\n"
		"orefld o14, 352(o12)\n orefst o14, 48(o2)\n"
		"orefld o14, 360(o12)\n orefst o14, 56(o2)\n"
		"orefld o14, 368(o12)\n orefst o14, 64(o2)\n"
		"orefld o14, 376(o12)\n orefst o14, 72(o2)\n"
		"orefld o14, 384(o12)\n orefst o14, 80(o2)\n"
		"orefld o14, 392(o12)\n orefst o14, 88(o2)\n"
		"orefld o14, 400(o12)\n orefst o14, 96(o2)\n"
		"orefld o14, 408(o12)\n orefst o14, 104(o2)\n"
		"orefld o14, 416(o12)\n orefst o14, 112(o2)\n"
		"orefld o14, 424(o12)\n orefst o14, 120(o2)\n"
		"orefld o14, 432(o12)\n orefst o14, 128(o2)"
		::: "memory");

	/* grid queues: O12 720.. -> wait_set 136.. */
	asm volatile(
		"orefld o14, 720(o12)\n orefst o14, 136(o2)\n"
		"orefld o14, 728(o12)\n orefst o14, 144(o2)\n"
		"orefld o14, 736(o12)\n orefst o14, 152(o2)\n"
		"orefld o14, 744(o12)\n orefst o14, 160(o2)\n"
		"orefld o14, 752(o12)\n orefst o14, 168(o2)\n"
		"orefld o14, 760(o12)\n orefst o14, 176(o2)\n"
		"orefld o14, 768(o12)\n orefst o14, 184(o2)\n"
		"orefld o14, 776(o12)\n orefst o14, 192(o2)\n"
		"orefld o14, 784(o12)\n orefst o14, 200(o2)\n"
		"orefld o14, 792(o12)\n orefst o14, 208(o2)\n"
		"orefld o14, 800(o12)\n orefst o14, 216(o2)\n"
		"orefld o14, 808(o12)\n orefst o14, 224(o2)\n"
		"orefld o14, 816(o12)\n orefst o14, 232(o2)\n"
		"orefld o14, 824(o12)\n orefst o14, 240(o2)\n"
		"orefld o14, 832(o12)\n orefst o14, 248(o2)\n"
		"orefld o14, 840(o12)\n orefst o14, 256(o2)"
		::: "memory");

	/* vector queues: O12 848.. -> wait_set 264.. */
	asm volatile(
		"orefld o14, 848(o12)\n orefst o14, 264(o2)\n"
		"orefld o14, 856(o12)\n orefst o14, 272(o2)\n"
		"orefld o14, 864(o12)\n orefst o14, 280(o2)\n"
		"orefld o14, 872(o12)\n orefst o14, 288(o2)\n"
		"orefld o14, 880(o12)\n orefst o14, 296(o2)\n"
		"orefld o14, 888(o12)\n orefst o14, 304(o2)\n"
		"orefld o14, 896(o12)\n orefst o14, 312(o2)\n"
		"orefld o14, 904(o12)\n orefst o14, 320(o2)\n"
		"orefld o14, 912(o12)\n orefst o14, 328(o2)\n"
		"orefld o14, 920(o12)\n orefst o14, 336(o2)\n"
		"orefld o14, 928(o12)\n orefst o14, 344(o2)\n"
		"orefld o14, 936(o12)\n orefst o14, 352(o2)\n"
		"orefld o14, 944(o12)\n orefst o14, 360(o2)\n"
		"orefld o14, 952(o12)\n orefst o14, 368(o2)\n"
		"orefld o14, 960(o12)\n orefst o14, 376(o2)\n"
		"orefld o14, 968(o12)\n orefst o14, 384(o2)"
		::: "memory");

	/* raster queues: O12 984.. -> wait_set 392.. */
	asm volatile(
		"orefld o14, 984(o12)\n orefst o14, 392(o2)\n"
		"orefld o14, 992(o12)\n orefst o14, 400(o2)\n"
		"orefld o14, 1000(o12)\n orefst o14, 408(o2)\n"
		"orefld o14, 1008(o12)\n orefst o14, 416(o2)\n"
		"orefld o14, 1016(o12)\n orefst o14, 424(o2)\n"
		"orefld o14, 1024(o12)\n orefst o14, 432(o2)\n"
		"orefld o14, 1032(o12)\n orefst o14, 440(o2)\n"
		"orefld o14, 1040(o12)\n orefst o14, 448(o2)\n"
		"orefld o14, 1048(o12)\n orefst o14, 456(o2)\n"
		"orefld o14, 1056(o12)\n orefst o14, 464(o2)\n"
		"orefld o14, 1064(o12)\n orefst o14, 472(o2)\n"
		"orefld o14, 1072(o12)\n orefst o14, 480(o2)\n"
		"orefld o14, 1080(o12)\n orefst o14, 488(o2)\n"
		"orefld o14, 1088(o12)\n orefst o14, 496(o2)\n"
		"orefld o14, 1096(o12)\n orefst o14, 504(o2)\n"
		"orefld o14, 1104(o12)\n orefst o14, 512(o2)"
		::: "memory");

	/* fixed input slots: pointer-svc, pointer-events, keyboard-svc,
	 * keyboard-events sinks (always live after boot). */
	asm volatile(
		"orefld o14, 1120(o12)\n orefst o14, 520(o2)\n"   /* pointer svc  */
		"orefld o14, 1136(o12)\n orefst o14, 528(o2)\n"   /* pointer evts */
		"orefld o14, 1152(o12)\n orefst o14, 536(o2)\n"   /* keyboard svc */
		"orefld o14, 1168(o12)\n orefst o14, 544(o2)"     /* keyboard evts*/
		::: "memory");

	/* Block until any listed queue is non-empty.  O2 already holds the
	 * wait-set list; R5 = the fixed count. */
	asm volatile(
		"addiu r5, r0, %1\n"           /* count */
		"call  #0x206\n"               /* WaitAnyQueue */
		"nop\n"
		"addu  %0, r2, r0"
		: "=r"(status)
		: "i"(WM_WAITSET_COUNT)
		: "r1", "r2", "r5"
	);

	/* We clobbered O2 (the wait-set list) to make the call; restore the
	 * boot stack/data ORs print_str and the console helpers expect —
	 * same dance ReceiveQueuePoll needed. */
	wm_restore_boot_or();
	return status;
}

/* Non-blocking poll of the main service queue (timeout = 0): the
 * WaitAnyQueue above already parked us until something was ready, so
 * here we just drain whatever the service queue holds and return
 * immediately if it's empty. */
static int
poll_one_request(int *out_op, int *out_wid, int *out_arg)
{
	int status, op, wid, arg;
	asm volatile(
		"omov  o1, o9\n"
		"addiu r4, r0, 0\n"            /* timeout = 0 (non-blocking) */
		"call  #0x204\n"               /* ReceiveQueuePoll */
		"nop\n"
		"addu  %0, r2, r0\n"
		"addu  %1, r3, r0\n"
		"addu  %2, r4, r0\n"
		"addu  %3, r5, r0"
		: "=r"(status), "=r"(op), "=r"(wid), "=r"(arg)
		:
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
 * Pixel buffer must live in the boot data segment (O15 / DATA_VA-
 * relative offsets).  Callers stage pixels into vec_scratch_row
 * (file-scope static = .data) and pass that.  Was STACK-relative
 * in the original glyph-render path; latent bug because the only
 * shipped caller back then went through ObjBlitGlyphs (different
 * primitive).  mouse_paint is the first program to actually drive
 * vec_rect_fill / vec_line / fb_blit_row through clients, exposing
 * the wrong-ref / wrong-offset combo. */
static void
fb_blit_row(int y, int x, const unsigned char *pixels, int n_pixels)
{
	if (y < 0 || y >= CELL_AREA_H_PX) return;
	if (n_pixels <= 0)                return;
	if (x < 0) {
		if (n_pixels + x <= 0) return;
		pixels   -= x;        /* skip leading off-screen */
		n_pixels += x;
		x         = 0;
	}
	if (x >= CELL_AREA_W_PX)           return;
	if (x + n_pixels > CELL_AREA_W_PX) n_pixels = CELL_AREA_W_PX - x;

	int src_off = (int)((unsigned int)pixels - DATA_VA);
	/* Window FB stride is USABLE_W_PX (the full FB width, including
	 * border ring); cell-area pixel (x, y) lives at window-FB pixel
	 * (CONTENT_X_OFF_PX + x, CONTENT_Y_OFF_PX + y). */
	int dst_off = (y + CONTENT_Y_OFF_PX) * USABLE_W_PX
	            + (x + CONTENT_X_OFF_PX);
	asm volatile(
		"addu  r7, %1, r0\n"
		"addu  r8, %2, r0\n"
		"addu  r9, %3, r0\n"
		"omov  o1, o15\n"            /* boot DATA ref */
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
static unsigned char  vec_scratch_row[];
static unsigned char *prep_scratch_row(int n, unsigned char color);

/* Single-pixel store.  Bresenham line and oval-outline plot pixel-
 * by-pixel; this is just a one-byte fb_blit_row.  Stages the pixel
 * into vec_scratch_row[0] (data-segment buffer) since fb_blit_row
 * now addresses through O15/DATA_VA. */
static void
fb_set_pixel(int x, int y, unsigned char color)
{
	vec_scratch_row[0] = color;
	fb_blit_row(y, x, vec_scratch_row, 1);
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
	/* Phase 60 step 15: cell content sits below the title bar AND
	 * inside the per-window border ring, so we add
	 * CONTENT_CELL_Y_OFF (= BORDER_CELLS_Y + TITLE_BAR_CELLS) to
	 * the row coord and CONTENT_CELL_X_OFF (= BORDER_CELLS_X) to
	 * the column.  Both are cell counts, so ObjBlitGlyphs's
	 * pixel-coord math (cell_x * 8, cell_y * 16) lands at the
	 * right place automatically. */
	int fb_col = col_start + CONTENT_CELL_X_OFF;
	int fb_row = cell_row  + CONTENT_CELL_Y_OFF;
	int packed_xy = ((fb_col & 0xFFFF) << 16) | (fb_row & 0xFFFF);
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
	 * (fb_col*CELL_W, fb_row*CELL_H) → (n_glyphs*CELL_W, CELL_H).
	 * fb_col / fb_row already include the border + title offsets. */
	composite_window_region(fb_col * CELL_W, fb_row * CELL_H,
	                        n_glyphs * CELL_W, CELL_H);
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
	/* Phase 60 step 15: scroll the cell-content region only.  The
	 * border + title bar stay put.  Region covers x ∈
	 * [CONTENT_X_OFF_PX..CONTENT_X_OFF_PX+CELL_AREA_W_PX), y ∈
	 * [CONTENT_Y_OFF_PX..CONTENT_Y_OFF_PX+CELL_AREA_H_PX). */
	int packed_xy = ((CONTENT_X_OFF_PX & 0xFFFF) << 16)
	              | (CONTENT_Y_OFF_PX & 0xFFFF);
	int packed_wh = ((CELL_AREA_W_PX & 0xFFFF) << 16)
	              | (CELL_AREA_H_PX & 0xFFFF);
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
	/* Only the cell-content area changed; border + title stay put. */
	composite_window_region(CONTENT_X_OFF_PX, CONTENT_Y_OFF_PX,
	                        CELL_AREA_W_PX, CELL_AREA_H_PX);
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
			/* Phase 60 step 14: destructive backspace — render
			 * a space at the new cursor position so the erased
			 * glyph actually disappears.  Previously the WM just
			 * moved the cursor back, leaving the old glyph on
			 * screen; the shell's read_line expects backspace
			 * to visually delete and was sending '\b' for that
			 * effect. */
			unsigned char space = ' ';
			flush_strip(&space, 1, row, col);
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
	 * its stack — once the leader's task unblocks (when our reply
	 * lands at the reply_cap), it will reuse that stack region for
	 * the next print, and any later read of the source ref returns
	 * garbage.  We have to capture our copy while the leader is
	 * still blocked on this SEND, which is before we reply.
	 *
	 * Buffer sized at 1024 bytes — covers the typical large write
	 * (a directory listing from `ls /programs` is ~375 bytes today
	 * and growing).  The pre-step-3 forwarder used to relay the
	 * full count downstream to oriscterm regardless, so the buf
	 * was sized for visibility only; with the WM as the CONSOLE
	 * endpoint, anything past the buf SILENTLY DROPS — keep this
	 * generous.  Bigger writes get the leading 1024 rendered and
	 * the tail discarded (no callers in tree exceed this). */
	unsigned char buf[1024];
	int fetch_count = (count > 1024) ? 1024 : count;
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
 * call it after every wake (Phase 61: WaitAnyQueue wakes us the
 * instant any console queue receives a SEND), so per-window writes
 * now have no timeout floor — just the natural turn-around of the
 * producing program. */
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
/* Scratch byte for client glyph rendering — lives in boot DATA (O15) so
 * win_draw_string can reach it the same way window_titles does.  The WM
 * drains vector ops one at a time, so a single shared byte is race-free. */
static unsigned char wm_text_scratch[2];

/* Map a client font-face id (FONT_FACE_* in liborisc.h) to a baked face.
 * Out-of-range falls back to the proportional Lucida face. */
static const wm_font_t *
text_face_lookup(int id)
{
	if (id == FONT_FACE_MONO)  return &font_lutRS;
	if (id == FONT_FACE_GLYPH) return &font_olgl;
	return &font_luRS;
}

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
		 * the window FB, then composite once.  Border + title bar
		 * stay put. */
		int xy = ((CONTENT_X_OFF_PX & 0xFFFF) << 16)
		       | (CONTENT_Y_OFF_PX & 0xFFFF);
		int wh = ((CELL_AREA_W_PX & 0xFFFF) << 16)
		       | (CELL_AREA_H_PX & 0xFFFF);
		fill_rect_window(xy, wh, WM_BG_COLOR);
		composite_content_area();
		return;
	}

	if (op == VEC_OP_TEXT_MOVE) {
		window_text_x[slot]    = vec_unpack_hi(packed1);
		window_text_y[slot]    = vec_unpack_lo(packed1);
		int f = packed2;
		if (f < 0 || f > FONT_FACE_GLYPH) f = FONT_FACE_PROP;
		window_text_face[slot] = (unsigned char)f;
		return;
	}
	if (op == VEC_OP_TEXT_CHAR) {
		const wm_font_t *face = text_face_lookup(window_text_face[slot]);
		int cp  = packed1 & 0xFF;
		int adv = font_advance(face, cp);
		/* Content-relative pen -> absolute window-FB pixel: win_draw_string
		 * takes absolute coords, unlike fb_blit_row's content-relative ones,
		 * so add the content origin here to match the vec_* draw space. */
		int ax = window_text_x[slot] + CONTENT_X_OFF_PX;
		int ay = window_text_y[slot] + CONTENT_Y_OFF_PX;
		wm_text_scratch[0] = (unsigned char)cp;
		int pxy = ((ax & 0xFFFF) << 16) | (ay & 0xFFFF);
		win_draw_string(face, pxy,
		                font_shape(1, cur_vec_color, WM_BG_COLOR, 1),
		                wm_text_scratch);
		window_text_x[slot] += adv;
		/* Composite only THIS glyph's box, not the whole content area — keeps
		 * the per-char drain cheap so a long string can't back up the depth-64
		 * vector queue (and gives the Markdown viewer a fast path). */
		composite_window_region(ax, ay, adv > 0 ? adv : face->cell_w,
		                        face->cell_h);
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
		/* Same per-window backing-store treatment as VEC_OP_CLEAR
		 * — fill the cell-content area with bg and composite. */
		int xy = ((CONTENT_X_OFF_PX & 0xFFFF) << 16)
		       | (CONTENT_Y_OFF_PX & 0xFFFF);
		int wh = ((CELL_AREA_W_PX & 0xFFFF) << 16)
		       | (CELL_AREA_H_PX & 0xFFFF);
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
	if (w > CELL_AREA_W_PX) w = CELL_AREA_W_PX;

	/* Stash sender's source ref so we can OREFLD it again on each
	 * per-row ObjFetchBytes. */
	asm volatile("orefst o2, %0(o12)"
	             :: "i"(WM_FORWARD_SRC_SLOT_OFFSET));

	int scratch_data_off = (int)((unsigned int)vec_scratch_row - DATA_VA);

	int painted = 0;
	int row;
	for (row = 0; row < h; row++) {
		int dst_y = y + row;
		if (dst_y < 0 || dst_y >= CELL_AREA_H_PX) continue;

		int src_off = byte_offset + row * w;
		/* Window FB stride is USABLE_W_PX (full FB width including
		 * border ring); cell-area pixel (x, dst_y) lives at
		 * window-FB pixel (CONTENT_X_OFF_PX + x, CONTENT_Y_OFF_PX
		 * + dst_y). */
		int dst_off = (dst_y + CONTENT_Y_OFF_PX) * USABLE_W_PX
		            + (x + CONTENT_X_OFF_PX);

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

/* Phase 60 step 16 — window dragging helpers.
 *
 * raise_window(wid): rotate wid to the top of the z-stack and
 * re-blit its screen footprint so anything that was previously
 * above is overpainted.  No-op if wid isn't in the stack or is
 * already topmost.
 *
 * recompose_full_screen(): fill the entire screen FB with bg
 * and walk the z-stack to blit each window's full extent.  Used
 * by the drag path after a window's position changes: the
 * vacated pixels need bg + any windows beneath that show through,
 * and the new position needs a fresh paint at the destination.
 * Whole-screen recompose is simpler than tracking the union of
 * old and new rects; at ~1MB ObjBlitCopy per window the cost is
 * a few ms per motion event, well under Tk's event rate. */
static void
raise_window(int wid)
{
	int i, found = -1;
	for (i = 0; i < window_z_count; i++) {
		if (window_z[i] == wid) { found = i; break; }
	}
	if (found < 0) return;
	/* Always set focus to the just-clicked window, even if it was
	 * already topmost — focus may have drifted away (e.g., another
	 * window was destroyed and focus reverted to z-top, which is
	 * us, but only for one tick).  set_focus repaints both the
	 * old and new windows' title bars for the brightness indicator;
	 * it's a no-op (no repaint) when focus didn't actually move. */
	set_focus(wid);
	if (found == window_z_count - 1) return;   /* already top */
	for (i = found; i < window_z_count - 1; i++) {
		window_z[i] = window_z[i + 1];
	}
	window_z[window_z_count - 1] = wid;
	/* Repaint just this window's screen footprint — windows that
	 * were previously above it in z get overpainted by the new
	 * top of the stack. */
	int sx = window_pos_x[wid - 1];
	int sy = window_pos_y[wid - 1];
	composite_screen_rect(sx, sy, USABLE_W_PX, USABLE_H_PX);
}

static void
recompose_full_screen(void)
{
	int packed_xy = ((0 & 0xFFFF) << 16) | (0 & 0xFFFF);
	int packed_wh = ((FB_W & 0xFFFF) << 16) | (FB_H & 0xFFFF);
	fill_rect_packed(packed_xy, packed_wh, WM_WORKSPACE_COLOR);
	composite_screen_rect(0, 0, FB_W, FB_H);
}

/* Walk the z-stack top-to-bottom, return the topmost wid whose
 * screen footprint contains (px, py).  Returns 0 if nothing's
 * under the pointer. */
static int
topmost_window_at(int px, int py)
{
	int z;
	for (z = window_z_count - 1; z >= 0; z--) {
		int t = window_z[z];
		if (t < 1 || t > MAX_WINDOWS) continue;
		int wx = window_pos_x[t - 1];
		int wy = window_pos_y[t - 1];
		if (px >= wx && px < wx + USABLE_W_PX &&
		    py >= wy && py < wy + USABLE_H_PX) {
			return t;
		}
	}
	return 0;
}

/* Phase 60 step 17 — outline-drag primitives.  Paint / unpaint a
 * rectangular outline (4 thin strips: top, bottom, left, right)
 * directly on the screen FB.  Unpaint = bg-fill each strip then
 * re-composite the affected screen rect so any windows beneath
 * the outline come back unscathed. */
static void
draw_outline(int x, int y)
{
	int w = USABLE_W_PX;
	int h = USABLE_H_PX;
	int color = WM_OUTLINE_COLOR;
	/* Top strip. */
	fill_rect_packed(((x & 0xFFFF) << 16) | (y & 0xFFFF),
	                 ((w & 0xFFFF) << 16) | (OUTLINE_PX & 0xFFFF),
	                 color);
	/* Bottom strip. */
	fill_rect_packed(((x & 0xFFFF) << 16)
	                 | ((y + h - OUTLINE_PX) & 0xFFFF),
	                 ((w & 0xFFFF) << 16) | (OUTLINE_PX & 0xFFFF),
	                 color);
	/* Left strip. */
	fill_rect_packed(((x & 0xFFFF) << 16) | (y & 0xFFFF),
	                 ((OUTLINE_PX & 0xFFFF) << 16) | (h & 0xFFFF),
	                 color);
	/* Right strip. */
	fill_rect_packed((((x + w - OUTLINE_PX) & 0xFFFF) << 16)
	                 | (y & 0xFFFF),
	                 ((OUTLINE_PX & 0xFFFF) << 16) | (h & 0xFFFF),
	                 color);
}

static void
erase_screen_rect(int sx, int sy, int w, int h)
{
	int packed_xy = ((sx & 0xFFFF) << 16) | (sy & 0xFFFF);
	int packed_wh = ((w  & 0xFFFF) << 16) | (h  & 0xFFFF);
	fill_rect_packed(packed_xy, packed_wh, WM_WORKSPACE_COLOR);
	composite_screen_rect(sx, sy, w, h);
}

static void
erase_outline(int x, int y)
{
	int w = USABLE_W_PX;
	int h = USABLE_H_PX;
	erase_screen_rect(x, y, w, OUTLINE_PX);                   /* top */
	erase_screen_rect(x, y + h - OUTLINE_PX, w, OUTLINE_PX);  /* bot */
	erase_screen_rect(x, y, OUTLINE_PX, h);                   /* left */
	erase_screen_rect(x + w - OUTLINE_PX, y, OUTLINE_PX, h);  /* right */
}

/* True if (px, py) is inside wid's title bar (the area between
 * the border ring and the content cells — same rect ObjBlitGlyphs
 * paints title text into). */
static int
point_in_title_bar(int wid, int px, int py)
{
	int wx = window_pos_x[wid - 1];
	int wy = window_pos_y[wid - 1];
	int tx_lo = wx + TITLE_X_OFF_PX;
	int tx_hi = tx_lo + CELL_AREA_W_PX;
	int ty_lo = wy + TITLE_Y_OFF_PX;
	int ty_hi = ty_lo + TITLE_BAR_PX;
	return (px >= tx_lo && px < tx_hi && py >= ty_lo && py < ty_hi);
}

/* True if (px, py) is inside wid's window-menu button (the raised ▽ square
 * at the LEFT of the title bar — TITLE_X_OFF_PX..+MENU_BTN_W over the bar
 * height).  Checked before the general title-bar drag test so a button
 * click (SELECT) destroys the window rather than starting a drag.  (This
 * replaced the old rightmost-cells close box; SELECT keeps that close
 * behaviour for now — real OPEN LOOK SELECT iconifies.) */
static int
point_in_menu_button(int wid, int px, int py)
{
	int wx = window_pos_x[wid - 1];
	int wy = window_pos_y[wid - 1];
	int bx_lo = wx + MENU_BTN_X;
	int bx_hi = bx_lo + MENU_BTN_W;
	int by_lo = wy + TITLE_Y_OFF_PX;
	int by_hi = by_lo + TITLE_BAR_PX;
	return (px >= bx_lo && px < bx_hi && py >= by_lo && py < by_hi);
}

/* Phase 60 step 19 — lazy supervisor cap acquisition.  WM boots with
 * O8 = oriscdir cap (parked into BOOT_PARENT_SLOT_OFFSET / SUP_SLOT
 * at task_init).  The desktop menu wants to use libc's sup_spawn,
 * which reads its supervisor cap from SUP_SLOT_OFFSET — so we
 * dir-walk /sys/cpu/0/supervisor on first use and overwrite the
 * slot with the resolved cap.  Idempotent: subsequent calls early-
 * return after the flag check.
 *
 * Note: at WM boot the slot holds the DIR cap (not a supervisor cap)
 * — an OISN check alone can't distinguish, hence the file-scope
 * flag.  Flag must live at file scope, not as a function-static —
 * pcc-orisc emits the function into `.data` if a static-local
 * variable is declared mid-function.  Same trick wm_z_count etc.
 * already use.
 *
 * 0 on success (slot now holds the supervisor cap), negative on
 * failure (slot left untouched). */
static int sup_walked;

static int
sup_walk_to_slot(void)
{
	if (sup_walked) return 0;

	const char path[] = "/sys/cpu/0/supervisor";
	int kind;
	char rem[16];
	int rc = dir_walk(path, &kind, rem, sizeof(rem));
	if (rc != 0) return rc;
	if (kind != DIR_KIND_LEAF) return -1;
	asm volatile(
		"orefld o1, 616(o12)\n"             /* DIR_RESULT_SLOT */
		"orefst o1, %0(o12)"                /* → BOOT_PARENT / SUP_SLOT */
		:
		: "i"(BOOT_PARENT_SLOT_OFFSET)
		: "r1"
	);
	sup_walked = 1;
	return 0;
}

/* Desktop menu helpers.  All coords below are in CELL units except
 * where suffixed _px / when explicitly noted.  Menu position is
 * cell-aligned so ObjBlitGlyphs's cell-coord math works directly. */
static int
menu_width_cells(void)
{
	int max = 0;
	int i;
	for (i = 0; i < DESKTOP_MENU_N; i++) {
		int n = desktop_menu_label_lens[i];
		if (n > max) max = n;
	}
	return max + 2 * MENU_PAD_CELLS_X;
}

static void
menu_paint_item(int item_idx)
{
	if (item_idx < 0 || item_idx >= DESKTOP_MENU_N) return;
	int row_cell_y = menu_y_cell + item_idx;
	int is_hi = (item_idx == menu_highlighted);
	int bg    = is_hi ? MENU_HI_BG_COLOR : MENU_BG_COLOR;
	int fg    = is_hi ? MENU_HI_FG_COLOR : MENU_FG_COLOR;

	/* Fill the row bg first (covers hilite change). */
	int xy = ((menu_x_cell * CELL_W) & 0xFFFF) << 16;
	xy |= (row_cell_y * CELL_H) & 0xFFFF;
	int wh = ((menu_w_cells * CELL_W) & 0xFFFF) << 16;
	wh |= CELL_H & 0xFFFF;
	fill_rect_packed(xy, wh, bg);

	/* Then the label glyphs, indented by MENU_PAD_CELLS_X. */
	const char *label = desktop_menu_labels[item_idx];
	int label_len     = desktop_menu_label_lens[item_idx];
	int glyph_cx      = menu_x_cell + MENU_PAD_CELLS_X;
	int packed_xy_g   = ((glyph_cx & 0xFFFF) << 16) | (row_cell_y & 0xFFFF);
	int packed_shape  = ((label_len & 0xFFFF) << 16)
	                  | ((fg & 0xFF) << 8) | (bg & 0xFF);
	screen_blit_glyph_row(packed_xy_g, (const unsigned char *)label,
	                      packed_shape);
}

/* True if a cell coord falls inside the active menu rect. */
static int
menu_hit_cell(int cx, int cy)
{
	if (!menu_active) return -1;
	if (cx < menu_x_cell || cx >= menu_x_cell + menu_w_cells)
		return -1;
	if (cy < menu_y_cell || cy >= menu_y_cell + menu_h_cells)
		return -1;
	return cy - menu_y_cell;
}

static void
desktop_menu_show(int px, int py)
{
	if (menu_active) return;
	menu_w_cells = menu_width_cells();
	menu_h_cells = DESKTOP_MENU_N;
	int cx = px / CELL_W;
	int cy = py / CELL_H;
	/* Clamp so the menu fits fully on screen. */
	int max_cx = (FB_W / CELL_W) - menu_w_cells;
	int max_cy = (FB_H / CELL_H) - menu_h_cells;
	if (cx < 0) cx = 0;
	if (cy < 0) cy = 0;
	if (cx > max_cx) cx = max_cx;
	if (cy > max_cy) cy = max_cy;
	menu_x_cell = cx;
	menu_y_cell = cy;
	menu_highlighted = -1;
	menu_active = 1;

	int i;
	for (i = 0; i < DESKTOP_MENU_N; i++) menu_paint_item(i);
}

/* Erase the menu by bg-filling + recomposing the rect from the
 * window z-stack — same primitive recompose_after_destroy uses. */
static void
desktop_menu_dismiss(void)
{
	if (!menu_active) return;
	int sx = menu_x_cell * CELL_W;
	int sy = menu_y_cell * CELL_H;
	int sw = menu_w_cells * CELL_W;
	int sh = menu_h_cells * CELL_H;
	menu_active = 0;
	menu_highlighted = -1;
	recompose_after_destroy(sx, sy, sw, sh);
}

/* Update hover highlight based on the new cursor position.  Two
 * paints worst case: erase old, paint new (each is one row +
 * glyphs). */
static void
desktop_menu_update_highlight(int px, int py)
{
	int cx = px / CELL_W;
	int cy = py / CELL_H;
	int new_hi = menu_hit_cell(cx, cy);
	if (new_hi == menu_highlighted) return;
	int old_hi = menu_highlighted;
	menu_highlighted = new_hi;
	if (old_hi >= 0) menu_paint_item(old_hi);
	if (new_hi >= 0) menu_paint_item(new_hi);
}

/* User clicked an item — dismiss menu, then sup_spawn the program
 * if the entry has a spawn_path (Cancel has spawn_path = NULL).
 * sup_spawn uses round-robin (SUP_TARGET_ANY) the same way `run`
 * does in cmd_run's no-`@N` path. */
static void
desktop_menu_select(int item_idx)
{
	if (item_idx < 0 || item_idx >= DESKTOP_MENU_N) {
		desktop_menu_dismiss();
		return;
	}
	const char *path = desktop_menu_spawn_paths[item_idx];
	desktop_menu_dismiss();
	if (path == (const char *)0) return;

	int rc = sup_walk_to_slot();
	if (rc != 0) {
		WM_PRINT("oriscwm: menu spawn: supervisor walk failed: ");
		WM_PRINT_INT(rc);
		WM_PRINT("\n");
		return;
	}
	task_t t = sup_spawn(path, "", "/");
	wm_restore_boot_or();
	if (t < 0) {
		WM_PRINT("oriscwm: menu spawn failed: ");
		WM_PRINT_INT((int)t);
		WM_PRINT("\n");
	}
}

/* WM-side pointer event dispatch.  Called from poll_pointer_events
 * before forwarding to any subscriber.  Returns 1 if the event was
 * consumed (don't forward), 0 to let it through.
 *
 * Behaviour (outline-style drag — same model as Mac / Windows /
 * X11 c. 1986: the window stays put during the drag and only an
 * outline rectangle tracks the pointer; on button-up the window
 * snaps to the final outline position):
 *
 *   LEFT button DOWN on a title bar  → raise, snapshot drag state,
 *                                       paint outline at current
 *                                       window pos (consumed)
 *   LEFT button DOWN on a window     → raise; forward (not consumed)
 *   MOTION while dragging            → erase old outline, paint
 *                                       new outline at target pos
 *                                       (consumed)
 *   LEFT button UP while dragging    → erase outline, move window
 *                                       to final pos, recompose
 *                                       (consumed)
 *   anything else                    → forward */
static int
wm_handle_pointer(int evt_type, int packed_xy, int button, int btn_state)
{
	(void)btn_state;
	int px = (packed_xy >> 16) & 0xFFFF;
	int py = packed_xy & 0xFFFF;

	/* Modal desktop menu: while up, the WM owns the cursor.
	 * MOTION updates hover; LEFT DOWN inside menu = select +
	 * dismiss + spawn; LEFT DOWN outside = cancel + dismiss;
	 * other buttons / RIGHT clicks are swallowed silently so a
	 * second right-click doesn't open a second menu. */
	if (menu_active) {
		if (evt_type == PTR_EVT_MOTION) {
			desktop_menu_update_highlight(px, py);
			return 1;
		}
		if (evt_type == PTR_EVT_DOWN) {
			if (button != PTR_BTN_LEFT) return 1;
			int cx = px / CELL_W;
			int cy = py / CELL_H;
			int hit = menu_hit_cell(cx, cy);
			if (hit >= 0) desktop_menu_select(hit);
			else          desktop_menu_dismiss();
			return 1;
		}
		/* PTR_EVT_UP and anything else: swallow while modal. */
		return 1;
	}

	/* Right-click (or middle-click) on empty desktop summons the
	 * menu.  Tk on macOS Aqua swaps button-2 and button-3 vs
	 * X11 — the "secondary click" (ctrl-click / two-finger tap)
	 * arrives as PTR_BTN_MIDDLE there, while real three-button
	 * mice on X11 send PTR_BTN_RIGHT.  Accept either, since
	 * neither has any other meaning when no window is under the
	 * cursor (topmost_window_at returns 0).  Right-/middle-click
	 * on a window still falls through unconsumed. */
	if (evt_type == PTR_EVT_DOWN
	    && (button == PTR_BTN_RIGHT || button == PTR_BTN_MIDDLE)
	    && topmost_window_at(px, py) == 0) {
		desktop_menu_show(px, py);
		return 1;
	}

	if (evt_type == PTR_EVT_MOTION) {
		if (!drag_active) return 0;
		if (drag_wid < 1 || drag_wid > MAX_WINDOWS) {
			drag_active = 0;
			return 0;
		}
		int nx = drag_window_x + (px - drag_start_x);
		int ny = drag_window_y + (py - drag_start_y);
		/* Clamp so the outline (= future window pos) stays fully
		 * on screen — same constraint we'll enforce on the
		 * window at button-up. */
		int max_x = FB_W - USABLE_W_PX;
		int max_y = FB_H - USABLE_H_PX;
		if (nx < 0) nx = 0;
		if (ny < 0) ny = 0;
		if (nx > max_x) nx = max_x;
		if (ny > max_y) ny = max_y;
		if (nx == drag_outline_x && ny == drag_outline_y)
			return 1;
		erase_outline(drag_outline_x, drag_outline_y);
		draw_outline(nx, ny);
		drag_outline_x = nx;
		drag_outline_y = ny;
		return 1;
	}

	if (evt_type == PTR_EVT_DOWN) {
		if (button != PTR_BTN_LEFT) return 0;
		int t = topmost_window_at(px, py);
		if (t == 0) return 0;
		/* The window-menu button takes priority over raise/drag — a
		 * SELECT (left-click) on it destroys the window outright (the
		 * interim close behaviour).  Check before raise_window so we
		 * don't bother re-stacking a window we're about to tear down. */
		if (point_in_menu_button(t, px, py)) {
			window_teardown(t);
			return 1;
		}
		raise_window(t);
		if (point_in_title_bar(t, px, py)) {
			drag_active    = 1;
			drag_wid       = t;
			drag_start_x   = px;
			drag_start_y   = py;
			drag_window_x  = window_pos_x[t - 1];
			drag_window_y  = window_pos_y[t - 1];
			drag_outline_x = drag_window_x;
			drag_outline_y = drag_window_y;
			draw_outline(drag_outline_x, drag_outline_y);
			return 1;
		}
		/* Click in a non-title region: raise but let the
		 * subscriber see the click. */
		return 0;
	}

	if (evt_type == PTR_EVT_UP) {
		if (drag_active && button == PTR_BTN_LEFT) {
			erase_outline(drag_outline_x, drag_outline_y);
			int idx = drag_wid - 1;
			if (idx >= 0 && idx < MAX_WINDOWS
			    && (drag_outline_x != window_pos_x[idx]
			        || drag_outline_y != window_pos_y[idx])) {
				window_pos_x[idx] = drag_outline_x;
				window_pos_y[idx] = drag_outline_y;
				recompose_full_screen();
			}
			drag_active = 0;
			return 1;
		}
		return 0;
	}

	return 0;
}

/* Phase 60 step 18 — pointer mediation polls (focus-model version).
 *
 * poll_pointer_subscribes: drain subscribe SENDs queued on the
 * WM-side pointer service.  Wire: O2 = subscriber's reply ref (0
 * to unsubscribe).  Stashes into ptr_sub[focused_wid - 1] so the
 * subscribe associates with whichever window the caller is acting
 * on (typically the one they just wm_open_session'd, since
 * handle_new_window sets focus to it).
 *
 * poll_pointer_events: drain events from the local TAG_INPUT_SINK,
 * run WM-side handling (drag / raise / focus update), translate
 * surviving events into focused_wid's content-area-local coords,
 * and forward to ptr_sub[focused_wid - 1]. */
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
	 * overlay maps wire slot 1 → O2).  Stash into ptr_sub[focused_wid].
	 * Null O2 = unsubscribe; same path nulls the slot. */
	if (focused_wid < 1 || focused_wid > MAX_WINDOWS) return;
	asm volatile("omov o1, o2");
	stash_ptr_sub_o1(focused_wid);
}

static int _wm_ptr_evt_poll_status;

/* Dequeue one pointer event from the local TAG_INPUT_SINK.  Returns 0
 * and fills the out-params if an event was read, nonzero if the sink is
 * empty.  Factored out of poll_pointer_events so the drain loop below
 * can pull events one at a time and coalesce them. */
static int
poll_one_pointer_event(int *out_type, int *out_xy, int *out_btn, int *out_state)
{
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
	if (_wm_ptr_evt_poll_status != 0)
		return 1;
	*out_type  = evt_type;
	*out_xy    = packed_xy;
	*out_btn   = button;
	*out_state = btn_state;
	return 0;
}

/* Process one pointer event: WM-side handling (drag / raise / focus /
 * menu); if not consumed, translate to the focused window's content-
 * local coords and forward to its subscriber. */
static void
dispatch_pointer_event(int evt_type, int packed_xy, int button, int btn_state)
{
	/* WM-side handling first: drag / raise / focus update.  If
	 * consumed (drag in progress, title-bar grab) — swallow. */
	if (wm_handle_pointer(evt_type, packed_xy, button, btn_state))
		return;

	/* Translate to FOCUSED window's content-area-local coords.
	 * wm_handle_pointer has already moved focus to whatever
	 * window was just clicked (LEFT DOWN raises + focuses), so
	 * `focused_wid` is the right target for the forward.  Events
	 * outside the focused window's content area get dropped —
	 * the subscriber only cares about its own canvas. */
	if (focused_wid < 1 || focused_wid > MAX_WINDOWS) return;
	{
		int px = (packed_xy >> 16) & 0xFFFF;
		int py = packed_xy & 0xFFFF;
		int cx = px - window_pos_x[focused_wid - 1] - CONTENT_X_OFF_PX;
		int cy = py - window_pos_y[focused_wid - 1] - CONTENT_Y_OFF_PX;
		if (cx < 0 || cx >= CELL_AREA_W_PX
		    || cy < 0 || cy >= CELL_AREA_H_PX) return;
		packed_xy = ((cx & 0xFFFF) << 16) | (cy & 0xFFFF);
	}

	/* Load focused window's pointer subscriber and forward. */
	load_ptr_sub_to_o1(focused_wid);
	int sub_isn;
	asm volatile("oisn %0, o1" : "=r"(sub_isn));
	if (sub_isn) return;

	/* Same safe-temps dance as before to dodge pcc-orisc's r4..r7
	 * input clobber.  O1 already holds the sub-ref from
	 * load_ptr_sub_to_o1 above. */
	asm volatile(
		"addu   r8,  %0, r0\n"        /* save evt_type */
		"addu   r9,  %1, r0\n"        /* save packed_xy */
		"addu   r10, %2, r0\n"        /* save button */
		"addu   r11, %3, r0\n"        /* save btn_state */
		"onull  o2\n"
		"onull  o3\n"
		"addu   r4, r8,  r0\n"
		"addu   r5, r9,  r0\n"
		"addu   r6, r10, r0\n"
		"addu   r7, r11, r0\n"
		"send   o1"
		:
		: "r"(evt_type), "r"(packed_xy),
		  "r"(button),   "r"(btn_state)
		: "r1", "r4", "r5", "r6", "r7",
		  "r8", "r9", "r10", "r11"
	);
}

/* Drain ALL queued pointer events each pass — previously one per pass,
 * which made dragging crawl as motion events backed up in the depth-64
 * sink.  Consecutive WM-consumed drag-motions are coalesced to the
 * latest position (one outline erase/draw per batch instead of N); only
 * drag motions are coalesced, so non-drag motions still forward every
 * point and subscribers (mouse_paint et al.) keep their full stream.  A
 * button event (or non-drag motion) flushes the latest pending motion
 * first, so the outline is current before a button-up commits. */
static void
poll_pointer_events(void)
{
	int have_pending_motion = 0, pending_xy = 0;
	int evt_type, packed_xy, button, btn_state;
	for (;;) {
		if (poll_one_pointer_event(&evt_type, &packed_xy, &button, &btn_state))
			break;
		if (evt_type == PTR_EVT_MOTION && drag_active && !menu_active) {
			have_pending_motion = 1;
			pending_xy = packed_xy;
			continue;
		}
		if (have_pending_motion) {
			wm_handle_pointer(PTR_EVT_MOTION, pending_xy, 0, 0);
			have_pending_motion = 0;
		}
		dispatch_pointer_event(evt_type, packed_xy, button, btn_state);
	}
	if (have_pending_motion)
		wm_handle_pointer(PTR_EVT_MOTION, pending_xy, 0, 0);
}

/* Phase 60 step 18 — focus-model keyboard subscribe / forward.
 * Each subscribe SEND lands its sender's O2 (reply ref, or null
 * to unsubscribe) into kbd_sub[focused_wid - 1] — the slot for
 * whatever window is currently focused.  poll_keyboard_events
 * loads kbd_sub[focused_wid - 1] for every forward, so as focus
 * moves between windows the keyboard automatically follows. */
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

	/* The subscribe SEND landed sender's O2 → our O2.  Move it
	 * into kbd_sub[focused_wid - 1].  Null O2 (unsubscribe)
	 * clears the slot for the same wid. */
	if (focused_wid < 1 || focused_wid > MAX_WINDOWS) return;
	asm volatile("omov o1, o2");
	stash_kbd_sub_o1(focused_wid);
}

static int _wm_kbd_evt_poll_status;

static void
poll_keyboard_events(void)
{
	/* Always drain the sink first so WM-side consumers (e.g.,
	 * the desktop menu's ESC handler) can see events even when
	 * no focused-window subscriber is registered. */
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

	/* Modal menu: ESC dismisses; anything else gets swallowed. */
	if (menu_active) {
		if (code == TK_ESCAPE) desktop_menu_dismiss();
		return;
	}

	/* Forward to focused window's subscriber, if any. */
	if (focused_wid < 1 || focused_wid > MAX_WINDOWS) return;
	load_kbd_sub_to_o1(focused_wid);
	int sub_isn;
	asm volatile("oisn %0, o1" : "=r"(sub_isn));
	if (sub_isn) return;

	asm volatile(
		"addu   r8,  %0, r0\n"
		"addu   r9,  %1, r0\n"
		"onull  o2\n"
		"onull  o3\n"
		"addu   r4, r8, r0\n"
		"addu   r5, r9, r0\n"
		"addiu  r6, r0, 0\n"
		"addiu  r7, r0, 0\n"
		"send   o1"
		:
		: "r"(code), "r"(mods)
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
	 * queues), framebuffer via #0x10A ObjAllocFramebuffer.
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

	/* Phase 61 — allocate the wait-set objstore the event-driven main
	 * loop refills + hands to WaitAnyQueue each iteration.  Must come
	 * after the service mailbox (O9) and the input sinks exist; the
	 * per-window slots fill in as windows are created. */
	status = alloc_wait_set();
	if (status != 0) {
		WM_PRINT("oriscwm: alloc_wait_set failed: ");
		WM_PRINT_INT(status);
		WM_PRINT("\n");
		return 1;
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

	/* Dispatch loop (Phase 61 — event-driven).
	 *
	 * Each iteration:
	 *   1. Block in WaitAnyQueue (#0x206) over EVERY queue we own —
	 *      the main service queue, all per-window CONSOLE / GRID /
	 *      VECTOR / RASTER queues, and the keyboard / pointer service
	 *      + input-sink queues.  The firmware parks the CPU until ANY
	 *      of them is non-empty, so an idle WM burns no instructions
	 *      and a keystroke / console write / service request wakes us
	 *      with no timeout floor.
	 *   2. Drain everything with non-blocking polls (timeout=0): one
	 *      service request, then every per-window queue round-robin,
	 *      then the input sinks.  Empty queues return immediately.  If
	 *      a backlog remains, WaitAnyQueue returns immediately next
	 *      iteration (a queue is still non-empty) so we keep draining
	 *      without ever idling — no busy-wait, just work-driven loops.
	 *   3. When the service queue was empty this wake, run the
	 *      auto-destroy scan (task_query each window's owner; free
	 *      EXITed slots) — same trigger as the old timeout branch,
	 *      now fired on any wake with no pending service request. */
	for (;;) {
		int op, wid_or_zero, arg;
		int status;

		wm_block_until_ready();

		status = poll_one_request(&op, &wid_or_zero, &arg);
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
			/* No service request this wake — run the auto-destroy
			 * scan (replaces the old WM_POLL_TICKS-timeout trigger). */
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
