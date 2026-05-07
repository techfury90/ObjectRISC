/*
 * edit.c — standalone full-screen text editor.
 *
 * Same body as the shell's old cmd_edit builtin, repackaged as
 * its own .orx so it can be backgrounded with `run /programs/
 * edit.orx &`. Once running, oriscterm's F1 hotkey cycles
 * keyboard focus between the shell (upper text pane) and this
 * editor (lower grid canvas) — type to whichever pane has focus.
 *
 * The shell parses argv from `run /programs/edit.orx <path>`
 * and threads it down via orx_spawn → libc program_args(). In
 * Phase 41a the libc helper returns an empty string (the
 * actual ARGV_VA mapping is deferred to Phase 41b), so until
 * then the editor falls back to a fixed scratchpad path
 * (EDIT_DEFAULT_PATH). Save/quit work the same: ^S writes back
 * to the chosen path, ^X exits.
 *
 * Does its own term_init (full — including the kbd subscribe
 * SEND), which is what makes it show up as a second oriscterm
 * subscriber and what makes the F1 cycle have somewhere to
 * land. The shell's term_init already happened, so when this
 * program subscribes the terminal sees subscriber #2 and shows
 * "kbd focus 1/2 (F1 to cycle)" in its title bar.
 */

#include "liborisc.h"

#define EDIT_DEFAULT_PATH "/scratch.txt"
#define EDIT_PATH_MAX     128

#define EDIT_MAX_LINES   100
#define EDIT_LINE_MAX    96
#define EDIT_GRID_COLS   80
#define EDIT_CONTENT_ROWS 23
#define EDIT_STATUS_ROW  23

#define KEY_CTRL_S       0x13
#define KEY_CTRL_X       0x18

struct edit_state {
	char  lines[EDIT_MAX_LINES][EDIT_LINE_MAX];
	int   line_lens[EDIT_MAX_LINES];
	int   n_lines;
	int   cur_row;
	int   cur_col;
	int   top_row;
	int   dirty;
	int   truncated;
	char  path[EDIT_PATH_MAX];
};

static int
edit_status_append(char *status, int sp, const char *s)
{
	while (*s && sp < EDIT_GRID_COLS) status[sp++] = *s++;
	return sp;
}

static int
edit_status_append_int(char *status, int sp, int v)
{
	char numbuf[12];
	int nb = 0;
	int j;
	if (v == 0) numbuf[nb++] = '0';
	while (v > 0) { numbuf[nb++] = '0' + (v % 10); v /= 10; }
	for (j = nb - 1; j >= 0 && sp < EDIT_GRID_COLS; j--)
		status[sp++] = numbuf[j];
	return sp;
}

static void
edit_render(struct edit_state *es)
{
	int i;
	int row;
	int len;
	char status[EDIT_GRID_COLS];
	char cursor_glyph;
	int sp;

	grid_clear();

	for (i = 0; i < EDIT_CONTENT_ROWS; i++) {
		row = es->top_row + i;
		if (row >= es->n_lines) break;
		len = es->line_lens[row];
		if (len > EDIT_GRID_COLS) len = EDIT_GRID_COLS;
		if (len > 0)
			grid_print_n(0, i, es->lines[row], len);
	}

	cursor_glyph = '_';
	{
		int screen_row = es->cur_row - es->top_row;
		if (screen_row >= 0 && screen_row < EDIT_CONTENT_ROWS)
			grid_print_n(es->cur_col, screen_row, &cursor_glyph, 1);
	}

	sp = 0;
	sp = edit_status_append(status, sp, "edit: ");
	sp = edit_status_append(status, sp, es->path);
	if (es->dirty)
		sp = edit_status_append(status, sp, " *");
	if (es->truncated)
		sp = edit_status_append(status, sp, " (truncated)");
	sp = edit_status_append(status, sp, "  ");
	sp = edit_status_append_int(status, sp, es->cur_row + 1);
	sp = edit_status_append(status, sp, ",");
	sp = edit_status_append_int(status, sp, es->cur_col + 1);
	sp = edit_status_append(status, sp, "   ^S=save ^X=quit");
	grid_print_n(0, EDIT_STATUS_ROW, status, sp);
}

static int
edit_insert_char(struct edit_state *es, int c)
{
	char *line = es->lines[es->cur_row];
	int   len  = es->line_lens[es->cur_row];
	int   i;
	if (len >= EDIT_LINE_MAX - 1) return 0;
	for (i = len; i > es->cur_col; i--) line[i] = line[i - 1];
	line[es->cur_col] = (char)c;
	es->line_lens[es->cur_row] = len + 1;
	es->cur_col++;
	es->dirty = 1;
	return 1;
}

static void
edit_backspace(struct edit_state *es)
{
	char *line;
	int   len;
	int   i;

	if (es->cur_col > 0) {
		line = es->lines[es->cur_row];
		len  = es->line_lens[es->cur_row];
		for (i = es->cur_col - 1; i < len - 1; i++) line[i] = line[i + 1];
		es->line_lens[es->cur_row] = len - 1;
		es->cur_col--;
		es->dirty = 1;
		return;
	}
	if (es->cur_row == 0) return;

	{
		int prev = es->cur_row - 1;
		int prev_len = es->line_lens[prev];
		int cur_len  = es->line_lens[es->cur_row];
		int copy = cur_len;
		if (prev_len + copy > EDIT_LINE_MAX - 1)
			copy = EDIT_LINE_MAX - 1 - prev_len;
		for (i = 0; i < copy; i++)
			es->lines[prev][prev_len + i] = es->lines[es->cur_row][i];
		es->line_lens[prev] = prev_len + copy;
		for (i = es->cur_row; i < es->n_lines - 1; i++) {
			int j;
			int next_len = es->line_lens[i + 1];
			for (j = 0; j < next_len; j++)
				es->lines[i][j] = es->lines[i + 1][j];
			es->line_lens[i] = next_len;
		}
		es->n_lines--;
		es->cur_row = prev;
		es->cur_col = prev_len;
		es->dirty = 1;
	}
}

static void
edit_newline(struct edit_state *es)
{
	int i;
	int j;
	int cur_len;
	int tail_len;

	if (es->n_lines >= EDIT_MAX_LINES) return;

	for (i = es->n_lines; i > es->cur_row + 1; i--) {
		int prev_len = es->line_lens[i - 1];
		for (j = 0; j < prev_len; j++)
			es->lines[i][j] = es->lines[i - 1][j];
		es->line_lens[i] = prev_len;
	}
	es->n_lines++;

	cur_len  = es->line_lens[es->cur_row];
	tail_len = cur_len - es->cur_col;
	for (j = 0; j < tail_len; j++)
		es->lines[es->cur_row + 1][j] =
		    es->lines[es->cur_row][es->cur_col + j];
	es->line_lens[es->cur_row + 1] = tail_len;
	es->line_lens[es->cur_row]     = es->cur_col;

	es->cur_row++;
	es->cur_col = 0;
	es->dirty = 1;
}

static void
edit_clamp_cur_col(struct edit_state *es)
{
	int len = es->line_lens[es->cur_row];
	if (es->cur_col > len) es->cur_col = len;
	if (es->cur_col < 0) es->cur_col = 0;
}

static void
edit_scroll_into_view(struct edit_state *es)
{
	if (es->cur_row < es->top_row) es->top_row = es->cur_row;
	if (es->cur_row >= es->top_row + EDIT_CONTENT_ROWS)
		es->top_row = es->cur_row - EDIT_CONTENT_ROWS + 1;
	if (es->top_row < 0) es->top_row = 0;
}

static int
edit_save(struct edit_state *es)
{
	int fd;
	int i;
	char nl = '\n';

	fd = hf_open(es->path,
	             HF_O_WRONLY | HF_O_CREAT | HF_O_TRUNC);
	if (fd < 0) return -1;
	for (i = 0; i < es->n_lines; i++) {
		if (es->line_lens[i] > 0
		    && hf_write(fd, es->lines[i], es->line_lens[i]) < 0) {
			hf_close(fd);
			return -1;
		}
		if (hf_write(fd, &nl, 1) < 0) {
			hf_close(fd);
			return -1;
		}
	}
	hf_close(fd);
	es->dirty = 0;
	return 0;
}

static void
edit_load(struct edit_state *es)
{
	int fd;
	char rdbuf[256];
	int n;
	int i;
	int line_len = 0;

	es->n_lines  = 1;
	es->line_lens[0] = 0;
	es->cur_row  = 0;
	es->cur_col  = 0;
	es->top_row  = 0;
	es->dirty    = 0;
	es->truncated = 0;

	fd = hf_open(es->path, HF_O_RDONLY);
	if (fd < 0) return;

	while ((n = hf_read(fd, rdbuf, sizeof(rdbuf))) > 0) {
		for (i = 0; i < n; i++) {
			if (es->n_lines > EDIT_MAX_LINES) {
				es->truncated = 1;
				break;
			}
			if (rdbuf[i] == '\n') {
				es->line_lens[es->n_lines - 1] = line_len;
				line_len = 0;
				if (es->n_lines >= EDIT_MAX_LINES) {
					es->truncated = 1;
					break;
				}
				es->n_lines++;
				es->line_lens[es->n_lines - 1] = 0;
				continue;
			}
			if (line_len < EDIT_LINE_MAX - 1) {
				es->lines[es->n_lines - 1][line_len++] = rdbuf[i];
			}
		}
		if (es->truncated) break;
	}
	es->line_lens[es->n_lines - 1] = line_len;
	if (es->n_lines > 1 && es->line_lens[es->n_lines - 1] == 0)
		es->n_lines--;
	hf_close(fd);
}

int
main(void)
{
	struct edit_state es;
	int key;
	int mods;

	/* Full term_init — subscribes to the keyboard. The shell's
	 * subscribe already happened; ours lands as subscriber #2,
	 * and oriscterm flips its title bar to advertise the F1
	 * focus-cycle hotkey. */
	term_init();
	hf_init();

	/* Pick the file to edit from the args the shell handed us
	 * (program_args() returns whatever the launcher buffered —
	 * empty string in Phase 41a, since the actual ARGV_VA
	 * mapping isn't wired yet). First whitespace-token wins;
	 * empty / blank → default scratchpad. */
	{
		const char *args = program_args();
		int dst = 0;
		while (*args == ' ' || *args == '\t') args++;
		while (*args && *args != ' ' && *args != '\t'
		       && dst < EDIT_PATH_MAX - 1) {
			es.path[dst++] = *args++;
		}
		es.path[dst] = '\0';
		if (es.path[0] == '\0') {
			const char *def = EDIT_DEFAULT_PATH;
			int i;
			for (i = 0; def[i] && i < EDIT_PATH_MAX - 1; i++)
				es.path[i] = def[i];
			es.path[i] = '\0';
		}
	}

	edit_load(&es);

	for (;;) {
		edit_scroll_into_view(&es);
		edit_render(&es);
		key = term_getkey(&mods);

		if (key == KEY_CTRL_X) {
			break;
		} else if (key == KEY_CTRL_S) {
			edit_save(&es);
		} else if (key == TK_RETURN) {
			edit_newline(&es);
		} else if (key == TK_BACKSPACE) {
			edit_backspace(&es);
		} else if (key == TK_LEFT) {
			if (es.cur_col > 0) es.cur_col--;
			else if (es.cur_row > 0) {
				es.cur_row--;
				es.cur_col = es.line_lens[es.cur_row];
			}
		} else if (key == TK_RIGHT) {
			if (es.cur_col < es.line_lens[es.cur_row]) es.cur_col++;
			else if (es.cur_row < es.n_lines - 1) {
				es.cur_row++;
				es.cur_col = 0;
			}
		} else if (key == TK_UP) {
			if (es.cur_row > 0) {
				es.cur_row--;
				edit_clamp_cur_col(&es);
			}
		} else if (key == TK_DOWN) {
			if (es.cur_row < es.n_lines - 1) {
				es.cur_row++;
				edit_clamp_cur_col(&es);
			}
		} else if (key >= 0x20 && key < 0x7F) {
			edit_insert_char(&es, key);
		}
	}

	grid_clear();
	return 0;
}
