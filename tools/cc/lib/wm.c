/*
 * wm.c — libc wrapper for the oriscwm window-manager wire protocol.
 *
 * Phase 4: migrated onto the handle-based object API (obj.h). The WM
 * main-service cap (was WM_SLOT, copied from the dir-walk result) and the
 * per-program reply mailbox (was REPLY_MB_SLOT, a raw O12 slot shared
 * with dir.c / sup.c) are now `obj_t` handles in the O12 handle table;
 * this client gives itself its own mailbox handle (the old sharing was an
 * alloc optimisation, and all three were strictly synchronous SEND-and-
 * poll anyway). Each op is one obj_send_bytes (O3 = the reply mailbox
 * cap) followed by obj_recv_full for the int reply — except
 * wm_bind_surface, whose reply carries a CAPABILITY in O2, received via
 * the OR-receive keystone obj_recv_cap.
 *
 * Public API:
 *
 *   int wm_init(void);
 *       Lazy: adopts the WM service from dir_walk("/sys/wm/<term>/0").
 *       Returns 0 OK, WM_NO_DIRECTORY (-6) if dir_walk can't bootstrap,
 *       WM_NO_WM (-2) if the path doesn't resolve to a LEAF.
 *
 *   int wm_new_window(int type, int *out_wid,
 *                     int *out_w_cells, int *out_h_cells);
 *       SENDs OP_NEW_WINDOW. (The owner-task auto-destroy ref — formerly
 *       O2, stashed from the caller's O1 — is dropped: it was never wired
 *       on the WM side and every caller passes a null O1. Re-add a
 *       two-cap send when auto-destroy is actually implemented.)
 *
 *   int wm_bind_surface(int wid, int kind);
 *       SENDs OP_BIND_SURFACE. The resolved surface cap (reply O2) is
 *       received with obj_recv_cap and mirrored into DIR_RESULT_SLOT (=
 *       616) via obj_park_dir_result, so the legacy direct-616 consumers
 *       (wm_open_session's orefld, and vec/raster/pointer's
 *       obj_adopt_dir_result) keep working unchanged.
 *
 *   int wm_destroy_window(int wid);
 *   int wm_get_geometry(int wid, int *out);
 *   int wm_set_title(int wid, const char *title);
 *   int wm_subscribe_events(int wid, int notify_op);   (stub, no callers)
 *   int wm_open_session(const char *title, int *out_wid);
 *
 * No-WM fallback unchanged: wm_init returns WM_NO_WM and subsequent calls
 * short-circuit; callers fall back to direct boot-OPR surfaces.
 *
 * Boot-OR hygiene: obj_send_bytes / obj_recv_* use O2/O3/O4 as scratch,
 * so each helper restores O2 = boot stack (O11) and O3 = boot data (O15)
 * on the way out (the same O2/O3 restore wm_open_session always did). O4
 * (the boot self-svc) is deliberately NOT restored: it is vestigial on
 * this path — dir.c routinely clobbers its O14 save-slot during dir_walk
 * and nothing consumes it (term.c allocates its own mailbox rather than
 * reusing the self-svc).
 */

#include "liborisc.h"
#include "obj.h"

/* Wire ops on the WM main service (must match oriscwm.c). */
#define WM_OP_NEW_WINDOW         1
#define WM_OP_BIND_SURFACE       2
#define WM_OP_DESTROY_WINDOW     3
#define WM_OP_SUBSCRIBE_EVENTS   4
#define WM_OP_QUERY_GEOMETRY     5
#define WM_OP_SET_TITLE          6
#define WM_OP_FONT_OPEN          7
#define WM_OP_MEASURE_TEXT       8
#define WM_OP_FONT_WIDTHS        9
#define WM_OP_SET_SCROLL         10

/* WM error codes — also surface in liborisc.h. Mirrored locally for
 * the no-WM fallback short-circuit. */
#define WM_NO_WM         (-2)     /* /sys/wm/0 didn't resolve */
#define WM_NO_DIRECTORY  (-6)     /* no oriscdir to walk through */

/* DIR_RESULT slot in the O12 task table — wm_bind_surface mirrors the
 * resolved surface cap here, and wm_open_session OREFLDs it into
 * O5/O6/O7. Must match task.c's slot map (also obj.c's
 * OBJ_DIR_RESULT_OFFSET). */
#define DIR_RESULT_SLOT_OFFSET    616

/* Stack base VA — wm_set_title computes the byte offset of the title
 * bytes inside the caller's boot stack ref (O11). */
#define STACK_BOTTOM              0x001f0000U

/* wm_set_title hard cap. Must match oriscwm.c's MAX_TITLE_LEN (= N_COLS;
 * Phase 60 step 14 dropped N_COLS to 80). */
#define WM_MAX_TITLE_LEN          80

/* font_open name cap — must match oriscwm.c's FONT_NAME_MAX. */
#define WM_FONT_NAME_MAX          32

/* wm_measure_text run cap — must match oriscwm.c's WM_MEASURE_MAX. */
#define WM_MEASURE_MAX            128

/* Mailbox caps: R|W|S|V|C (== 0x5b) — same as the other clients' service
 * mailboxes. */
#define WM_MBOX_CAPS \
	(OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_S | OBJ_CAP_V | OBJ_CAP_C)

/* The WM main service (adopted from the dir-walk result) and our private
 * reply mailbox, both as object handles. */
static obj_t wm_svc_h  = OBJ_NULL;
static obj_t wm_mbox_h = OBJ_NULL;

/* Restore the boot O2/O3 the SEND/poll clobbered (see file header on why
 * O4 is intentionally left alone). */
static void
_wm_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

/* Compose /sys/wm/<my_term>/0 into buf. Multi-WM (one instance per
 * terminal, Phase 59 / WM γ.15) means each caller walks the WM that
 * serves its own terminal. task_my_terminal_idx() returns -1 when the
 * caller has no terminal info; fall back to terminal 0 for back-compat. */
static void
wm_render_path(char *buf)
{
	int idx = task_my_terminal_idx();
	if (idx < 0) idx = 0;
	const char prefix[] = "/sys/wm/";
	const char suffix[] = "/0";
	int p = 0, i;
	for (i = 0; prefix[i]; i++) buf[p++] = prefix[i];
	if (idx >= 100) {
		buf[p++] = '0' + (idx / 100);
		buf[p++] = '0' + ((idx / 10) % 10);
		buf[p++] = '0' + (idx % 10);
	} else if (idx >= 10) {
		buf[p++] = '0' + (idx / 10);
		buf[p++] = '0' + (idx % 10);
	} else {
		buf[p++] = '0' + idx;
	}
	for (i = 0; suffix[i]; i++) buf[p++] = suffix[i];
	buf[p] = '\0';
}

/* Lazy: adopt the WM service into wm_svc_h.
 *   1. If already adopted, fast-return.
 *   2. dir_walk("/sys/wm/<my_term>/0"); on LEAF success the resolved ref
 *      is in DIR_RESULT_SLOT — adopt it into the handle table.
 *   3. dir_walk failure: -6 (no directory) / path-not-found map to
 *      WM_NO_DIRECTORY / WM_NO_WM. */
int
wm_init(void)
{
	if (wm_svc_h >= 0)
		return 0;                  /* already adopted */
	if (obj_init() != 0)
		return WM_NO_DIRECTORY;    /* O12 not up (task_init not run) */

	int kind;
	char rem[16];
	char path[24];
	wm_render_path(path);
	int rc = dir_walk(path, &kind, rem, sizeof(rem));
	if (rc == -6) return WM_NO_DIRECTORY;
	if (rc < 0)   return WM_NO_WM;
	if (kind != DIR_KIND_LEAF) return WM_NO_WM;

	wm_svc_h = obj_adopt_dir_result();
	return (wm_svc_h < 0) ? WM_NO_WM : 0;
}

/* Allocate our private reply mailbox + queue if not already up.
 * Idempotent. */
static int
wm_reply_mailbox_init(void)
{
	if (wm_mbox_h >= 0)
		return 0;
	wm_mbox_h = obj_alloc(16, OBJ_TAG_SERVICE, WM_MBOX_CAPS);
	if (wm_mbox_h < 0)
		return -1;
	return obj_queue_attach(wm_mbox_h, 4);
}

/* Ensure the WM service + reply mailbox are both up. Returns 0, or the
 * wm_init / mailbox error. */
static int
wm_ensure(void)
{
	int rc = wm_init();
	if (rc != 0) return rc;
	return wm_reply_mailbox_init();
}

/* Common send + int-reply poll: SEND op with R5=a1, R6=a2 (R7=0), source
 * segment `src`, reply cap = our mailbox; block for the reply and fill
 * rep[0..3] = R3..R6. Restores boot O2/O3 on the way out. Returns 0, or
 * -1 on poll failure. */
static int
wm_send_recv(int src, int op, int a1, int a2, int rep[4])
{
	obj_send_bytes(wm_svc_h, src, wm_mbox_h, op, a1, a2, 0);
	int rc = obj_recv_full(wm_mbox_h, rep);
	_wm_restore_or();
	return rc;
}

/* OP_NEW_WINDOW. Reply: R3=status, R4=geom_a, R5=geom_b (w_cells:h_cells),
 * R6=wid. */
int
wm_new_window(int type, int *out_wid,
              int *out_w_cells, int *out_h_cells)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	int rep[4];
	if (wm_send_recv(OBJ_SRC_NONE, WM_OP_NEW_WINDOW, 0, type, rep) != 0)
		return -1;
	int status = rep[0];
	int geom_b = rep[2];
	int wid    = rep[3];
	if (status != 0) return status;
	if (out_wid)     *out_wid     = wid;
	if (out_w_cells) *out_w_cells = (geom_b >> 16) & 0xFFFF;
	if (out_h_cells) *out_h_cells = geom_b & 0xFFFF;
	return 0;
}

/* OP_BIND_SURFACE. Reply: R3=status, O2=resolved surface cap. */
int
wm_bind_surface(int wid, int kind)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	obj_send_bytes(wm_svc_h, OBJ_SRC_NONE, wm_mbox_h,
	               WM_OP_BIND_SURFACE, wid, kind, 0);

	/* The resolved surface cap rides the reply's O2. obj_recv_cap lands
	 * it in a handle; mirror it into DIR_RESULT_SLOT for the legacy
	 * consumers, then drop our transient handle. */
	int status = 0;
	obj_t surf = obj_recv_cap(wm_mbox_h, &status);
	if (surf >= 0) {
		obj_park_dir_result(surf);
		obj_drop(surf);
	} else if (status == 0) {
		status = WM_NO_WM;         /* reply ok but no cap / table full */
	}
	_wm_restore_or();
	return status;
}

/* Phase 60 step 12 — open a brand-new WM-mediated session: replace the
 * inherited parent-window CONSOLE / KEYBOARD / GRID caps (O5/O6/O7) with
 * caps for a freshly-allocated window. After this returns, term_print /
 * term_read / grid_write land in the new window. Owner ref is null (no
 * auto-destroy); callers wm_destroy_window before exit.
 *
 * `title` may be NULL to skip the wm_set_title round-trip; "" sets an
 * empty (visible blank) bar. */
int
wm_open_session(const char *title, int *out_wid)
{
	int wid = 0, w_cells = 0, h_cells = 0;
	int rc;

	rc = wm_new_window(WIN_TYPE_CONSOLE, &wid, &w_cells, &h_cells);
	if (rc != 0) goto restore_or;

	rc = wm_bind_surface(wid, 1 /*WSURF_CONSOLE*/);
	if (rc != 0) goto destroy_and_restore;
	asm volatile("orefld o5, %0(o12)"
	             :: "i"(DIR_RESULT_SLOT_OFFSET) : "r1");

	rc = wm_bind_surface(wid, 2 /*WSURF_KEYBOARD*/);
	if (rc != 0) goto destroy_and_restore;
	asm volatile("orefld o6, %0(o12)"
	             :: "i"(DIR_RESULT_SLOT_OFFSET) : "r1");

	rc = wm_bind_surface(wid, 3 /*WSURF_GRID*/);
	if (rc != 0) goto destroy_and_restore;
	asm volatile("orefld o7, %0(o12)"
	             :: "i"(DIR_RESULT_SLOT_OFFSET) : "r1");

	if (title) {
		/* Best-effort: failure here doesn't undo the bind+install. */
		wm_set_title(wid, title);
	}

	if (out_wid) *out_wid = wid;
	rc = 0;
	goto restore_or;

destroy_and_restore:
	wm_destroy_window(wid);
	/* fall through */
restore_or:
	/* Restore boot O2 (stack) / O3 (data) from O11 / O15 before
	 * returning. EVERY exit path runs this — the wm_* round-trips
	 * clobbered O2/O3 with SEND/poll scratch even on failure paths. If
	 * we left them clobbered, a caller running term_init next would
	 * copy the garbage back into O11/O15 (term_init re-saves from
	 * O2/O3/O4 blindly), corrupting the boot-OR save. Requires the
	 * caller to have run task_init first (any program here has). */
	_wm_restore_or();
	return rc;
}

/* OP_DESTROY_WINDOW. */
int
wm_destroy_window(int wid)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	int rep[4];
	if (wm_send_recv(OBJ_SRC_NONE, WM_OP_DESTROY_WINDOW, wid, 0, rep) != 0)
		return -1;
	return rep[0];
}

/* OP_QUERY_GEOMETRY (wid 0 = first live window). Reply: R3=status,
 * R4=geom_a (w_px:h_px), R5=geom_b (w_cells:h_cells), R6=resolved_wid.
 * Packs into out[4] = {w_cells, h_cells, w_px, h_px}. */
int
wm_get_geometry(int wid, int *out)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	int rep[4];
	if (wm_send_recv(OBJ_SRC_NONE, WM_OP_QUERY_GEOMETRY, wid, 0, rep) != 0)
		return -1;
	int status = rep[0];
	int geom_a = rep[1];
	int geom_b = rep[2];
	if (status != 0) return status;
	if (out) {
		out[0] = (geom_b >> 16) & 0xFFFF;   /* w_cells */
		out[1] = geom_b & 0xFFFF;           /* h_cells */
		out[2] = (geom_a >> 16) & 0xFFFF;   /* w_px */
		out[3] = geom_a & 0xFFFF;           /* h_px */
	}
	return 0;
}

/* OP_SET_TITLE: byte-data SEND from a stack-local copy of `title`
 * (R6 = packed len:src_off, O2 = boot stack). The blocking reply poll is
 * a barrier — the WM ObjFetchBytes the title before replying — so the
 * stack buffer reliably outlives the fetch (no async-buffer race). */
int
wm_set_title(int wid, const char *title)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	char buf[WM_MAX_TITLE_LEN];
	int len = 0;
	while (len < WM_MAX_TITLE_LEN && title[len] != '\0') {
		buf[len] = title[len];
		len++;
	}
	int stack_off = (int)((unsigned int)buf - STACK_BOTTOM);
	int packed = ((len & 0xFFFF) << 16) | (stack_off & 0xFFFF);

	int rep[4];
	if (wm_send_recv(OBJ_SRC_STACK, WM_OP_SET_TITLE, wid, packed, rep) != 0)
		return -1;
	return rep[0];
}

/* font_open: ask the WM to load /fonts/<name>.wmf and return a face id usable
 * as the `face` arg to vec_text / vec_text_move.  The four built-in faces
 * resolve to ids 0..3; any other name loads into a fresh WM slot and returns a
 * new id (>=4).  Re-opening an already-loaded name returns its existing id.
 * Returns a face id >=0, or a negative WM error (e.g. -2 if /fonts/<name>.wmf
 * doesn't exist, -7 if the WM's dynamic-font table is full).  Same stack-copy +
 * blocking-reply shape as wm_set_title — the name reaches the WM via O2. */
int
font_open(const char *name)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	char buf[WM_FONT_NAME_MAX];
	int len = 0;
	while (len < WM_FONT_NAME_MAX && name[len] != '\0') {
		buf[len] = name[len];
		len++;
	}
	if (len == 0 || len >= WM_FONT_NAME_MAX) return -1;   /* empty / too long */
	int stack_off = (int)((unsigned int)buf - STACK_BOTTOM);
	int packed = ((len & 0xFFFF) << 16) | (stack_off & 0xFFFF);

	int rep[4];
	if (wm_send_recv(OBJ_SRC_STACK, WM_OP_FONT_OPEN, 0, packed, rep) != 0)
		return -1;
	return rep[0];   /* R3 = id (>=0) or -errno */
}

/* wm_measure_text: ask the WM for the pixel width of `s` in face `face` (a
 * FONT_FACE_* id or a font_open id).  Lets a client lay out / wrap PROPORTIONAL
 * text, which it can't measure itself — only the WM holds the width tables.
 * Same stack-copy + blocking-reply shape as font_open.  Returns the width in px
 * (>=0), or a negative WM error. */
int
wm_measure_text(int face, const char *s)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	char buf[WM_MEASURE_MAX];
	int len = 0;
	while (len < WM_MEASURE_MAX && s[len] != '\0') {
		buf[len] = s[len];
		len++;
	}
	if (len >= WM_MEASURE_MAX) return -1;   /* too long to measure in one call */
	if (len == 0) return 0;                  /* empty run = 0 px */
	int stack_off = (int)((unsigned int)buf - STACK_BOTTOM);
	int packed = ((len & 0xFFFF) << 16) | (stack_off & 0xFFFF);

	int rep[4];
	if (wm_send_recv(OBJ_SRC_STACK, WM_OP_MEASURE_TEXT, face, packed, rep) != 0)
		return -1;
	return rep[0];   /* R3 = width in px (>=0) or -errno */
}

/* wm_font_widths: fetch the glyph advances of 16 codepoints (start_cp ..
 * start_cp+15) for `face` into out16[].  Lets a client build a LOCAL width table
 * and wrap proportional text with no further WM round-trips — measure each
 * face's printable range in ~6 calls instead of one measure_text per word.
 * Returns 0, or a negative WM error. */
int
wm_font_widths(int face, int start_cp, unsigned char *out16)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;
	int rep[4];
	if (wm_send_recv(OBJ_SRC_NONE, WM_OP_FONT_WIDTHS, face, start_cp, rep) != 0)
		return -1;
	int j, i;
	for (j = 0; j < 4; j++)
		for (i = 0; i < 4; i++)
			out16[j * 4 + i] = (unsigned char)((rep[j] >> (i * 8)) & 0xFF);
	return 0;
}

/* wm_set_scroll: report a window's content height and current scroll offset to
 * the WM so its OPEN LOOK scrollbar elevator (the "cable car") rides the cable
 * to the matching position.  total_px = full content height, offset = pixels
 * scrolled past the top; both are clamped to 16 bits (the wire packs them 16:16,
 * good for content up to 65535 px).  Returns 0, or a negative WM error. */
int
wm_set_scroll(int wid, int total_px, int offset)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;
	if (total_px < 0)      total_px = 0;
	if (total_px > 0xFFFF) total_px = 0xFFFF;
	if (offset < 0)        offset = 0;
	if (offset > 0xFFFF)   offset = 0xFFFF;
	int packed = ((total_px & 0xFFFF) << 16) | (offset & 0xFFFF);
	int rep[4];
	if (wm_send_recv(OBJ_SRC_NONE, WM_OP_SET_SCROLL, wid, packed, rep) != 0)
		return -1;
	return rep[0];
}

/* OP_SUBSCRIBE_EVENTS. Milestone-2 stub with no callers; events don't
 * fire yet. The notify cap (formerly passed in O4 from the caller's O1)
 * is dropped — re-add a notify-cap send variant when event delivery is
 * actually implemented. */
int
wm_subscribe_events(int wid, int notify_op)
{
	int rc = wm_ensure();
	if (rc != 0) return rc;

	int rep[4];
	if (wm_send_recv(OBJ_SRC_NONE, WM_OP_SUBSCRIBE_EVENTS,
	                 wid, notify_op, rep) != 0)
		return -1;
	return rep[0];
}
