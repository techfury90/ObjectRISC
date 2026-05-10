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

/* Cell dims set by gen_wm_font.py — don't change without regenerating
 * the embedded font_8x16 array. */
#define CELL_W   8
#define CELL_H   16

/* 160 cols × 48 rows × 8×16 cells = 1280 × 768 framebuffer.  Text
 * stays at native font size; the larger grid gives the leader's
 * 80-col session room to breathe and leaves space on the right /
 * bottom for future multi-window tiling.  oriscterm displays the
 * framebuffer 1:1 — no scaling anywhere. */
#define N_COLS   160
#define N_ROWS   48
#define FB_W    (CELL_W * N_COLS)   /* 1280 */
#define FB_H    (CELL_H * N_ROWS)   /* 768  */

/* Palette indices (matching VEC_PALETTE in tools/devices/oriscterm). */
#define WM_BG_COLOR  0    /* dark navy background */
#define WM_FG_COLOR  1    /* light gray foreground */

/* Stack VA layout (CONTRACT.md §2).  Used for stack-relative offsets
 * passed to ObjFetchBytes / ObjStoreBytes — the boot stack ref lives
 * in O11 after task_init, and we compute byte offsets by subtracting
 * STACK_BOTTOM from a stack-local buffer's VA. */
#define STACK_BOTTOM 0x001f0000

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

/* Flush a strip of `n_glyphs` printable chars at cell (row, col_start)
 * to the framebuffer.  All glyphs in the strip share the same cell
 * row, so they project to CELL_H contiguous pixel rows in the
 * framebuffer; each pixel row is the byte-concatenation of their
 * per-row pixels.  Pushing one ObjStoreBytes per pixel row
 * (CELL_H = 16 calls, regardless of strip width) is the WM γ.4
 * trick — drops the wire RTT count from 16N (one per glyph row)
 * to 16 for any run of printable chars on the same cell row.
 *
 * Off-screen rows (>= N_ROWS) drop silently — no scroll yet. */
static void
flush_strip(const unsigned char *glyphs, int n_glyphs,
            int cell_row, int col_start)
{
	if (n_glyphs <= 0)        return;
	if (cell_row < 0)         return;
	if (cell_row >= N_ROWS)   return;

	int cell_x        = col_start * CELL_W;
	int cell_y        = cell_row  * CELL_H;
	int strip_pixels  = n_glyphs  * CELL_W;
	unsigned char pixel_row[N_COLS * CELL_W];   /* 1280-byte scratch */

	int r;
	for (r = 0; r < CELL_H; r++) {
		int g;
		for (g = 0; g < n_glyphs; g++) {
			/* font_8x16 is uint32[4] per glyph: 4 big-endian
			 * words holding 4 pixel rows each, MSB = earlier row.
			 * Cast to byte pointer and index by row; the BE
			 * packing maps byte layout 1:1 to row order. */
			const unsigned char *glyph =
				(const unsigned char *)font_8x16[glyphs[g] - 32];
			unsigned char glyph_byte = glyph[r];
			int base = g * CELL_W;
			int x;
			for (x = 0; x < CELL_W; x++) {
				int bit = (glyph_byte >> (7 - x)) & 1;
				pixel_row[base + x] = bit ? WM_FG_COLOR
				                          : WM_BG_COLOR;
			}
		}
		int src_off = (int)((unsigned int)pixel_row - STACK_BOTTOM);
		int dst_off = (cell_y + r) * FB_W + cell_x;
		/* pcc-orisc doesn't always honor r4/r5/r6 clobbers when
		 * picking input regs — if it places one of our %1/%2/%3
		 * inputs in (say) r4, then the first `addu r4, %x, r0`
		 * stomps that input before the later `addu r6, %3, r0`
		 * reads it.  Capture all three inputs into safe temps
		 * (r7..r9) FIRST, then move them into the ABI registers
		 * for the call.  r7..r9 added to clobbers. */
		asm volatile(
			"addu  r7, %1, r0\n"        /* save src_off */
			"addu  r8, %2, r0\n"        /* save dst_off */
			"addu  r9, %3, r0\n"        /* save strip_pixels */
			"omov  o1, o11\n"           /* boot stack ref */
			"orefld o2, %0(o12)\n"      /* framebuffer cap */
			"addu  r4, r7, r0\n"
			"addu  r5, r8, r0\n"
			"addu  r6, r9, r0\n"
			"call  #0x109\n"            /* ObjStoreBytes */
			"nop"
			:
			: "i"(WM_SURF_FRAMEBUFFER_SLOT_OFFSET),
			  "r"(src_off), "r"(dst_off), "r"(strip_pixels)
			: "r1", "r2", "r3", "r4", "r5", "r6",
			  "r7", "r8", "r9"
		);
	}
}

/* Render `count` bytes from `buf` to window `wid`'s framebuffer
 * surface, accumulating runs of printable chars on the same cell
 * row into strips and flushing each strip in 16 wire RTTs total.
 * Control chars ('\n' / '\r' / '\b') boundary the strip and adjust
 * the cursor; non-printable chars (other than those three) advance
 * the cursor without rendering, also boundarying the strip.
 *
 * No scroll — rows past N_ROWS stop rendering, but the cursor
 * still advances so subsequent newlines bring future content back
 * on screen if some clears arrive (future milestone work).
 *
 * Cursor state in window_cur_col/row[] is updated on return. */
static void
render_buffer(int wid, const unsigned char *buf, int count)
{
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
			if (col >= N_COLS) { col = 0; row += 1; }
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

	/* Now forward the SEND.  The terminal does its own OBJ_READ_REQ
	 * to the leader's source — the leader's still blocked, so the
	 * source is valid.  After we forward, oriscterm will process the
	 * SEND_DELIVER, then process the OBJ_WRITE_REQs from
	 * render_buffer below — console pane updates before the
	 * framebuffer pane fills in. */
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

	/* Render against our private buf copy.  The source ref may
	 * already be racing the leader's stack reuse here, but we
	 * don't care — we're working from buf, which is on our stack. */
	if (fetch_count > 0) {
		render_buffer(wid, buf, fetch_count);
	}
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
