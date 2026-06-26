/*
 * vector.c — WM-mediated vector graphics client (Phase 59 / WM γ.11).
 *
 * Phase 4: migrated onto the handle-based object API (obj.h). The
 * WM-mediated VECTOR service cap is now an `obj_t` handle in the O12
 * handle table (it used to be hand-copied from DIR_RESULT into
 * WM_VECTOR_CAP_SLOT and OREFLDed into O1 on every SEND), adopted
 * straight from the dir-walk result, and the draws go through obj_send.
 * All the raw orefld / oisn / send inline asm is gone.
 *
 * Each vec_*() helper builds a SEND with int_payload[0..2] = (op,
 * packed1, packed2), addressed to the adopted VECTOR cap.  The WM polls
 * its per-window VECTOR queue, dispatches on op, and rasterises the
 * result into the framebuffer.  The SEND itself is fire-and-forget; the
 * WM polls and rasterises asynchronously.  (Vector ops carry their whole
 * payload in the int words — there is no byte-data source ref — so this
 * client is immune to the async-buffer-lifetime trap that byte-data
 * clients like grid / raster must watch.)
 *
 * All helpers return 0 on success, -1 if no surface has been bound
 * (the handle is still OBJ_NULL).
 *
 * Boot OPR restoration: SEND clobbers O2..O4 with a reply-queue overlay.
 * We restore O2 = boot stack (O11) and O3 = boot data (O15) after every
 * SEND — otherwise a following print_str would read its string through a
 * clobbered O2.  Same idiom as pointer.c / term.c.
 *
 * Wire op codes match oriscterm's VEC_* and oriscwm's
 * forward_vector_write dispatch.
 */

#include "liborisc.h"
#include "obj.h"

/* The WM-mediated VECTOR service, adopted from the dir-walk result. */
static obj_t wm_vec_h = OBJ_NULL;

/* Restore the boot OPRs the SEND clobbered. */
static void
_vec_restore_or(void)
{
	asm volatile("omov o2, o11");
	asm volatile("omov o3, o15");
}

/* Pack two 16-bit values into one 32-bit word — used for (x, y)
 * and (w, h) pairs.  Sign-extending halves keep negative
 * coordinates representable; the WM does its own clipping. */
static int
_vec_pack(int hi, int lo)
{
	return ((hi & 0xFFFF) << 16) | (lo & 0xFFFF);
}

/* Common SEND.  Returns -1 if no surface is bound, 0 otherwise.  No
 * source ref — vector ops carry their entire payload in int_payload
 * (R4..R6 = op, packed1, packed2; R7 unused). */
static int
_vec_send(int op, int packed1, int packed2)
{
	if (wm_vec_h < 0)
		return -1;
	obj_send(wm_vec_h, op, packed1, packed2, 0);
	_vec_restore_or();
	return 0;
}

/* Public: adopt the cap that wm_bind_surface(WSURF_VECTOR) resolved into
 * the dir-walk result slot into the VECTOR handle.  Call once after a
 * successful wm_bind_surface(wid, WSURF_VECTOR).  Returns 0 if the
 * dir-result slot was non-null, -1 otherwise. */
int
vec_init_from_dir_result(void)
{
	if (obj_init() != 0)
		return -1;
	wm_vec_h = obj_adopt_dir_result();
	return (wm_vec_h < 0) ? -1 : 0;
}

int
vec_line(int x1, int y1, int x2, int y2)
{
	return _vec_send(VEC_OP_LINE,
	                 _vec_pack(x1, y1),
	                 _vec_pack(x2, y2));
}

int
vec_rect_fill(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_RECT_FILL,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_rect_outline(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_RECT_OUTLINE,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_oval_fill(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_OVAL_FILL,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_oval_outline(int x, int y, int w, int h)
{
	return _vec_send(VEC_OP_OVAL_OUTLINE,
	                 _vec_pack(x, y),
	                 _vec_pack(w, h));
}

int
vec_clear(void)
{
	return _vec_send(VEC_OP_CLEAR, 0, 0);
}

int
vec_set_color(int palette_idx)
{
	return _vec_send(VEC_OP_SET_COLOR, palette_idx, 0);
}

/* vec_text_move — position the per-surface text pen at pixel (x, y) and select
 * a font face (FONT_FACE_*).  Subsequent vec_text_char draws advance the pen. */
int
vec_text_move(int face, int x, int y)
{
	return _vec_send(VEC_OP_TEXT_MOVE, _vec_pack(x, y), face);
}

/* vec_text_char — draw one glyph `c` at the text pen in the current pen color
 * (vec_set_color), transparently, then advance the pen by the glyph's width.
 * Only the WM knows the proportional advance, so it advances the pen for us. */
int
vec_text_char(int c)
{
	return _vec_send(VEC_OP_TEXT_CHAR, c & 0xFF, 0);
}

/* vec_text — convenience: move the pen to (x, y) in `face`, then stream the
 * whole NUL-terminated string.  Color is whatever vec_set_color last set.
 * (One SEND per glyph for now — fine for labels/specimens; a batched path can
 * land later behind this same API when the Markdown viewer needs throughput.) */
int
vec_text(int face, int x, int y, const char *s)
{
	if (vec_text_move(face, x, y) != 0)
		return -1;
	/* Batch up to 8 codepoints per SEND (MSB-first into two payload words),
	 * ~8x fewer cross-CPU round-trips than one VEC_OP_TEXT_CHAR per glyph.
	 * A partial last batch leaves trailing 0 bytes, which the WM reads as the
	 * run terminator (text has no embedded NUL). */
	while (*s) {
		unsigned int w0 = 0, w1 = 0;
		int n = 0;
		while (n < 8 && *s) {
			unsigned int b = (unsigned char)*s++;
			if (n < 4) w0 |= b << (8 * (3 - n));
			else       w1 |= b << (8 * (3 - (n - 4)));
			n++;
		}
		if (_vec_send(VEC_OP_TEXT_RUN, (int)w0, (int)w1) != 0)
			return -1;
	}
	return 0;
}
