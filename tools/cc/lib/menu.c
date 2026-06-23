/*
 * menu.c — reusable modal pop-up menu for WM client programs.
 *
 * Mirrors the interaction model of the WM's desktop root menu (see
 * oriscwm.c desktop_menu_*): mouse motion highlights the item under
 * the cursor, left-click selects, a click outside the menu or Esc
 * cancels.  Arrow keys + Enter work too, so the menu is usable with
 * or without a mouse.
 *
 * Where the desktop menu draws on the screen FB with ObjBlitGlyphs
 * (a WM-local primitive with fg/bg color), a client program can only
 * reach its window through the wire-mediated grid service — which
 * paints monochrome text at (col, row) cells and carries no color.
 * So the highlight here is a leading marker ("> " on the selected
 * row, "  " on the rest) rather than an inverse-video bar.  Same
 * model, the rendering the client surface can express.
 *
 * Items are passed as a single flat buffer of `n` NUL-terminated
 * strings laid end to end:
 *
 *     static const char items[] = "Red\0Green\0Blue\0Quit";
 *     int pick = menu_run(col, row, items, 4);
 *
 * This flat shape (rather than the conventional `const char **`)
 * sidesteps a pcc-orisc backend bug: indexing an array-of-pointers
 * parameter ("items[i]") makes its register allocator choke
 * ("Coalesce: src class 3, dst class 1").  Scanning a flat char
 * buffer only ever loads single bytes, which it compiles cleanly —
 * the same reason oriscwm.c's desktop menu uses a packed label_buf.
 *
 * menu_run is modal and blocking: it draws the menu, polls keyboard +
 * (if subscribed) pointer until the user picks or cancels, and
 * returns the chosen index [0, n) or -1 on cancel.  It does NOT save
 * and restore the cells it draws over — the caller repaints its
 * content after menu_run returns.
 *
 * Prerequisites:
 *   - term_init() has run (keyboard mailbox in O9 + boot-OR parking,
 *     which grid_print also relies on).
 *   - For mouse control: pointer_init_from_dir_result() +
 *     pointer_subscribe().  Without a subscription menu_run silently
 *     falls back to keyboard-only.
 */

#include "liborisc.h"

/* Cell metrics — mirror oriscwm.c's CELL_W / CELL_H.  Pointer events
 * arrive in content-area-local pixels (the focus-model WM translates
 * screen → window-content coords before forwarding), so dividing by
 * the cell size maps a pointer position to a (col, row) cell. */
#define MENU_CELL_W   8
#define MENU_CELL_H   16

#define MENU_MARK_W   2            /* width of the "> " / "  " marker */

/* Return a pointer to item `idx` within the flat NUL-separated
 * buffer, by skipping `idx` strings.  Only single-byte derefs, which
 * pcc-orisc handles (unlike an array-of-pointers subscript). */
static const char *
menu_item(const char *items, int idx)
{
	const char *p = items;
	while (idx > 0) {
		while (*p) p++;        /* to this item's NUL */
		p++;                   /* past it, to the next item */
		idx--;
	}
	return p;
}

/* Longest item, in cells — bounds the clickable width. */
static int
menu_max_label(const char *items, int n)
{
	int max = 0;
	int i;
	for (i = 0; i < n; i++) {
		int len = (int)strlen(menu_item(items, i));
		if (len > max) max = len;
	}
	return max;
}

/* Menu geometry, stashed at menu_run entry so menu_draw stays a
 * 1-arg call — pcc-orisc trips ("adrput: illegal op 57") on calls
 * with 5+ args, and (items, col, row, n, sel) is five.  menu_run is
 * modal + single-threaded, so file-scope state is safe. */
static const char *m_items;
static int m_col, m_row, m_n;

/* Repaint every row: marker (selected vs not) + label.  Cheap — two
 * grid sends per row, and n is small for any real menu. */
static void
menu_draw(int sel)
{
	int i;
	for (i = 0; i < m_n; i++) {
		const char *label = menu_item(m_items, i);
		if (i == sel)
			grid_print(m_col, m_row + i, "> ");
		else
			grid_print(m_col, m_row + i, "  ");
		grid_print(m_col + MENU_MARK_W, m_row + i, label);
	}
}

int
menu_run(int col, int row, const char *items, int n)
{
	if (n <= 0) return -1;

	m_items = items;
	m_col = col;
	m_row = row;
	m_n = n;

	int width = MENU_MARK_W + menu_max_label(items, n);
	int have_mouse = pointer_subscribed();
	int sel = 0;

	menu_draw(sel);

	for (;;) {
		/* Keyboard (non-blocking): arrows move, Enter selects,
		 * Esc cancels.  Wrap-around on up/down so the list is a
		 * ring like most menu UIs. */
		int code = 0, mods = 0;
		if (term_pollkey(&code, &mods) == 0) {
			if (code == TK_UP) {
				sel = (sel + n - 1) % n;
				menu_draw(sel);
			} else if (code == TK_DOWN) {
				sel = (sel + 1) % n;
				menu_draw(sel);
			} else if (code == TK_RETURN || code == '\r'
			           || code == '\n') {
				return sel;
			} else if (code == TK_ESCAPE) {
				return -1;
			}
			/* Other keys ignored — stay modal. */
		}

		/* Mouse (non-blocking, only if subscribed): motion moves
		 * the highlight to the hovered row; left-down selects the
		 * hovered row or cancels on a click outside the menu. */
		if (have_mouse) {
			int et, xy, btn, mask;
			if (pointer_getevent(&et, &xy, &btn, &mask) == 0) {
				int px = (xy >> 16) & 0xFFFF;
				int py = xy & 0xFFFF;
				int ccol = px / MENU_CELL_W;
				int crow = py / MENU_CELL_H;
				int idx = crow - row;
				int inside = (idx >= 0 && idx < n
				              && ccol >= col && ccol < col + width);
				if (et == PTR_EVT_MOTION) {
					if (inside && idx != sel) {
						sel = idx;
						menu_draw(sel);
					}
				} else if (et == PTR_EVT_DOWN
				           && btn == PTR_BTN_LEFT) {
					if (inside) return idx;
					return -1;   /* click-off cancels */
				}
			}
		}

		task_yield();
	}
}
