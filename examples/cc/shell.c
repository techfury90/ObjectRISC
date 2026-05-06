/*
 * shell.c — minimal interactive shell for the Object RISC graphical
 * terminal. The MVP supervisor: cmd_run loads a .orx from the host
 * filesystem and TaskCreates a child task on the same CPU
 * (Phase 30) — no spare-CPU pool, no linkbootd round-trip.
 *
 * Built-ins:
 *     help        — short list of commands
 *     cat <path>  — print the contents of a host file
 *     more <path> — like cat, but paginated (space/RET to advance, q to quit)
 *     view <path> — full-screen file viewer using the grid canvas (q to quit)
 *     edit <path> — full-screen text editor on the grid canvas (^S save, ^X quit)
 *     ls [path]   — list a host directory (default: cwd)
 *     cd [path]   — change working directory (no arg → "/")
 *     pwd         — print the current working directory
 *     echo <text> — print the rest of the line
 *     run <path>[&] — load and run another .orx as a child task
 *                     ('&' = background; shell's prompt returns
 *                     immediately, harvest exit code with `wait`)
 *     wait <task>   — block until backgrounded task exits, print code
 *     kill <task>   — externally terminate a backgrounded task (exit 137)
 *     jobs          — list backgrounded tasks + state
 *     cycles      — print the CPU's cycle counter
 *     time        — print microseconds since boot (wall clock, 32-bit)
 *     exit / quit — end the session
 *
 * Path handling: the shell maintains its own cwd (absolute, relative
 * to hostfsd's --root jail). cat / ls / run resolve relative args by
 * joining cwd + arg + collapsing "." / ".." components, then send
 * the result to the hostfsd / orx_run as an absolute path.
 *
 * The runner script (run_shell.sh) computes a build-date banner
 * and passes it through -DBUILD_BANNER so a fresh build always
 * announces itself with the current real-world date shifted back
 * 40 years (the alternate-history conceit).
 *
 * Boot ABI (set up by run_shell.sh via --service order):
 *     O5  = oriscterm console  (idx 1)
 *     O6  = oriscterm keyboard (idx 2)
 *     O7  = oriscterm grid     (idx 3)  — used by cmd_view (Phase 38);
 *                                          carries both paint + clear
 *     O10 = hostfsd            (pid 17, idx 1)
 *     O11 = boot stack ref     (parked by term_init)
 *     O14 = boot self-svc      (parked by term_init)
 *     O15 = boot data ref      (parked by term_init)
 */

#include "liborisc.h"

#ifndef BUILD_BANNER
#define BUILD_BANNER "Object RISC Shell"
#endif

#define LINE_MAX 256
#define READ_BUF 128
#define PATH_MAX 256

/* Lines per page for `more` and the paginated `help`. The terminal
 * is 24 rows; we leave ~4 for the page-prompt + room to breathe. */
#define PAGE_LINES 20

/* Command history. Sized for ~10 KB of stack — fits comfortably in
 * the default 64 KB stack alongside cwd / line / prompt. Bumping
 * either dimension is free until you start eating into it. */
#define HISTORY_SIZE 16

struct history {
	char buf[HISTORY_SIZE][LINE_MAX];
	int  head;    /* index of the next slot to write */
	int  count;   /* number of valid entries (≤ HISTORY_SIZE) */
};

const char banner[]  = BUILD_BANNER;
const char hello1[]  = "\nType 'help' for commands. End with 'exit'.\n";
const char help_msg[] =
    "Commands:\n"
    "  help            — this message\n"
    "  cat <path>      — print the contents of a host file\n"
    "  more <path>     — like cat, but paginated (space/RET to advance, q to quit)\n"
    "  view <path>     — full-screen file viewer on the grid canvas (q to quit)\n"
    "  edit <path>     — full-screen text editor (^S save, ^X quit)\n"
    "  ls [<path>]     — list a host directory (default: cwd)\n"
    "  cd [<path>]     — change working directory (no arg → '/')\n"
    "  pwd             — print the current working directory\n"
    "  echo <text>     — print the rest of the line\n"
    "  run <path>[&]   — load + run another .orx as a child task ('&' = background)\n"
    "  wait <task>     — block until backgrounded task exits; print its exit code\n"
    "  kill <task>     — externally terminate a backgrounded task (exit 137)\n"
    "  jobs            — list backgrounded tasks and their state\n"
    "  cycles          — print the CPU's cycle counter\n"
    "  time            — print microseconds since boot (wall clock)\n"
    "  exit | quit     — leave the shell\n";

const char run_done_pre[] = "[exited ";
const char run_done_post[] = "]\n";
const char run_bg_pre[] = "[bg task ";
const char run_bg_post[] = "]\n";
const char more_prompt[] = "--More-- (space/RET, q to quit)";

/* The shell maintains an absolute, normalized cwd ("/", "/foo",
 * "/a/b" etc.) and threads a pointer to it through every command
 * that touches it. We can't put it in a global because Object RISC's
 * data segment is mapped R-only by init_cpu — `cmd_cd` would fault
 * on the assignment. So cwd lives on main()'s stack along with a
 * scratch buffer for building the prompt string. */

/* --- input helpers ---------------------------------------------------- */

/* Replace the on-screen line + buffer contents with `src`. Erases
 * the current `*n` chars via backspace echoes (oriscterm interprets
 * `\b` as delete-prev-char), then copies `src` into `buf` and
 * echoes it. Used by the up/down arrow handlers in read_line. */
static void
replace_line(char *buf, int *n, const char *src, int max)
{
	int i;
	while (*n > 0) {
		(*n)--;
		term_print_char('\b');
	}
	for (i = 0; src[i] && i < max - 1; i++) {
		buf[i] = src[i];
		term_print_char(src[i]);
	}
	*n = i;
}

/* Read a line from the keyboard. Echoes printable chars as they
 * arrive. Backspace deletes the previous char (visually too — see
 * oriscterm's `\b` handling). Up / Down arrows cycle through
 * history. Enter (TK_RETURN) terminates the line, saves it into
 * history if non-empty, and returns its length. */
static int
read_line(char *buf, int max, struct history *h)
{
	int n = 0;
	int pos = 0;     /* 0 = current/empty; k = k entries back */
	while (1) {
		int mods;
		int c = term_getkey(&mods);
		if (c < 0) { buf[0] = 0; return 0; }
		if (c == TK_RETURN) {
			int i;
			term_print("\n");
			buf[n] = 0;
			if (n > 0) {
				/* Save into the circular history. */
				for (i = 0; i <= n && i < LINE_MAX; i++)
					h->buf[h->head][i] = buf[i];
				h->head = (h->head + 1) % HISTORY_SIZE;
				if (h->count < HISTORY_SIZE) h->count++;
			}
			return n;
		}
		if (c == TK_BACKSPACE) {
			if (n > 0) {
				n--;
				/* Echo a literal '\b' (0x08) — oriscterm's
				 * _append interprets it as "delete the
				 * previous character", giving us visual
				 * undo. We only send when the buffer was
				 * non-empty so a backspace at column 0
				 * doesn't chew into the prompt. */
				term_print_char('\b');
			}
			continue;
		}
		if (c == TK_UP) {
			if (pos < h->count) {
				int idx;
				pos++;
				idx = (h->head - pos + HISTORY_SIZE) % HISTORY_SIZE;
				replace_line(buf, &n, h->buf[idx], max);
			}
			continue;
		}
		if (c == TK_DOWN) {
			if (pos > 0) {
				pos--;
				if (pos == 0) {
					replace_line(buf, &n, "", max);
				} else {
					int idx = (h->head - pos + HISTORY_SIZE)
					          % HISTORY_SIZE;
					replace_line(buf, &n, h->buf[idx], max);
				}
			}
			continue;
		}
		if (c >= 32 && c < 127 && n < max - 1) {
			buf[n++] = (char)c;
			term_print_char((char)c);
		}
	}
}

/* In-place split on first run of whitespace. Returns pointer to
 * first arg (possibly empty); first word becomes its own NUL-
 * terminated string at buf. */
static char *
split_arg(char *buf)
{
	char *p = buf;
	while (*p && *p != ' ' && *p != '\t') p++;
	if (*p == 0) return p;     /* no arg */
	*p++ = 0;
	while (*p == ' ' || *p == '\t') p++;
	return p;
}

/* --- path resolution -------------------------------------------------- */

/* Build `out` from cwd and arg, then collapse "." / ".." components
 * in place. Result is absolute, normalized, never has trailing '/'
 * (except at root). On overflow `out` is truncated to PATH_MAX-1
 * and the result may be invalid; callers don't bother checking
 * because PATH_MAX is generous for the shell's needs. */
static void
resolve_path(const char *cwd, const char *arg, char *out)
{
	int n = 0;

	/* Step 1: build the unnormalized join into `out`. */
	if (arg[0] == '/') {
		while (arg[n] && n < PATH_MAX - 1) {
			out[n] = arg[n];
			n++;
		}
		out[n] = 0;
	} else {
		int i = 0;
		while (cwd[i] && n < PATH_MAX - 1) out[n++] = cwd[i++];
		if (n > 0 && out[n-1] != '/' && n < PATH_MAX - 1)
			out[n++] = '/';
		i = 0;
		while (arg[i] && n < PATH_MAX - 1) out[n++] = arg[i++];
		out[n] = 0;
	}

	/* Step 2: collapse to a normalized form in tmp, then copy back.
	 * Walk components, skipping "." / empty, popping last on "..". */
	char tmp[PATH_MAX];
	int t = 0;
	tmp[t++] = '/';

	int i = (out[0] == '/') ? 1 : 0;
	while (out[i]) {
		/* Find next component. */
		int seg_start = i;
		while (out[i] && out[i] != '/') i++;
		int seg_len = i - seg_start;
		if (out[i] == '/') i++;

		if (seg_len == 0) continue;             /* "//" */
		if (seg_len == 1 && out[seg_start] == '.') continue;
		if (seg_len == 2
		    && out[seg_start] == '.'
		    && out[seg_start + 1] == '.') {
			/* Pop the last component (and its trailing '/'). */
			if (t > 1) {
				t--;                /* drop trailing '/' if any */
				while (t > 0 && tmp[t-1] != '/') t--;
				if (t == 0) tmp[t++] = '/';
			}
			continue;
		}
		/* Append component, ensuring exactly one '/' between. */
		if (t == 0 || tmp[t-1] != '/') {
			if (t < PATH_MAX - 1) tmp[t++] = '/';
		}
		int k;
		for (k = 0; k < seg_len && t < PATH_MAX - 1; k++)
			tmp[t++] = out[seg_start + k];
		/* Add trailing '/' for the next iteration's join. */
		if (t < PATH_MAX - 1) tmp[t++] = '/';
	}
	/* Strip trailing '/' unless we're at root. */
	if (t > 1 && tmp[t-1] == '/') t--;
	if (t == 0) tmp[t++] = '/';
	tmp[t] = 0;

	/* Copy tmp back into out. */
	int j;
	for (j = 0; j <= t && j < PATH_MAX; j++) out[j] = tmp[j];
}

/* --- prompt ----------------------------------------------------------- */

/* Build "$cwd> " into the caller's prompt_buf, then term_print it.
 * Called fresh before each line so cd updates show up immediately. */
static void
print_prompt(const char *cwd, char *prompt_buf)
{
	int i = 0;
	while (cwd[i] && i < PATH_MAX) {
		prompt_buf[i] = cwd[i];
		i++;
	}
	prompt_buf[i++] = '>';
	prompt_buf[i++] = ' ';
	prompt_buf[i] = 0;
	term_print(prompt_buf);
}

/* --- commands --------------------------------------------------------- */

/* Print the "--More-- " prompt, block on the keyboard, return 1 if
 * the user wants to abort (q/Q), 0 if they want to continue
 * (space/RET, or anything else we treat as "go"). The display is
 * append-only — we just newline past the prompt instead of erasing
 * it. */
static int
pause_for_key(void)
{
	int mods;
	int c;
	term_print(more_prompt);
	while (1) {
		c = term_getkey(&mods);
		if (c == 'q' || c == 'Q') { term_print("\n"); return 1; }
		if (c == ' ' || c == TK_RETURN) {
			term_print("\n");
			return 0;
		}
	}
}

/* Print a NUL-terminated string with pagination — pauses every
 * PAGE_LINES newlines until the user presses space/RET. Used by
 * `help` and could trivially be reused for any other long text. */
static void
print_paginated(const char *s)
{
	int line_count = 0;
	int seg_start = 0;
	int i = 0;
	while (s[i]) {
		if (s[i] == '\n') {
			term_print_n(s + seg_start, i - seg_start + 1);
			seg_start = i + 1;
			line_count++;
			if (line_count >= PAGE_LINES) {
				if (pause_for_key()) return;
				line_count = 0;
			}
		}
		i++;
	}
	if (i > seg_start) term_print_n(s + seg_start, i - seg_start);
}

static void
cmd_help(void)
{
	print_paginated(help_msg);
}

static void
cmd_cat(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	char buf[READ_BUF];
	resolve_path(cwd, arg, path);
	int fd = hf_open(path, HF_O_RDONLY);
	int n;
	if (fd < 0) {
		term_print("cat: cannot open '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	/* term_print_n_sync blocks until the receiver has pulled the
	 * bytes — safe to reuse `buf` immediately after. With the
	 * async term_print_n we needed double-buffering to give the
	 * receiver a window before our next hf_read overwrote things;
	 * the sync variant closes that race entirely. */
	while ((n = hf_read(fd, buf, sizeof(buf))) > 0) {
		term_print_n_sync(buf, n);
	}
	hf_close(fd);
}

static void
cmd_more(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	char buf[READ_BUF];
	int line_count = 0;
	resolve_path(cwd, arg, path);
	int fd = hf_open(path, HF_O_RDONLY);
	int n;
	if (fd < 0) {
		term_print("more: cannot open '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	while ((n = hf_read(fd, buf, sizeof(buf))) > 0) {
		/* Print the chunk in newline-bounded slices so we can
		 * paginate at line boundaries without splitting bytes.
		 * Each slice goes through term_print_n_sync so the
		 * receiver drains before we overwrite the buffer on the
		 * next hf_read. */
		int seg_start = 0;
		int i;
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				term_print_n_sync(buf + seg_start, i - seg_start + 1);
				seg_start = i + 1;
				line_count++;
				if (line_count >= PAGE_LINES) {
					if (pause_for_key()) {
						hf_close(fd);
						return;
					}
					line_count = 0;
				}
			}
		}
		if (i > seg_start) term_print_n_sync(buf + seg_start, i - seg_start);
	}
	hf_close(fd);
}

static void
cmd_ls(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	char buf[READ_BUF];
	resolve_path(cwd, arg, path);
	int fd = hf_opendir(path);
	int n;
	if (fd < 0) {
		term_print("ls: cannot open '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	while ((n = hf_read(fd, buf, sizeof(buf))) > 0) {
		term_print_n_sync(buf, n);
	}
	hf_close(fd);
}

static void
cmd_cd(char *cwd, const char *arg)
{
	char path[PATH_MAX];
	resolve_path(cwd, arg, path);
	/* Verify the new directory exists by opening + closing it.
	 * Avoids the trap of cd'ing into a non-existent path and only
	 * finding out on the next ls / cat. */
	int fd = hf_opendir(path);
	if (fd < 0) {
		term_print("cd: cannot enter '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	hf_close(fd);
	int i = 0;
	while (path[i] && i < PATH_MAX - 1) {
		cwd[i] = path[i];
		i++;
	}
	cwd[i] = 0;
}

static void
cmd_pwd(const char *cwd)
{
	term_print(cwd);
	term_print("\n");
}

static void
cmd_echo(const char *arg)
{
	term_print(arg);
	term_print("\n");
}

static void
cmd_run(const char *cwd, const char *arg)
{
	/* Detect a trailing '&' (with optional whitespace before it).
	 * If present: background the spawn — print [bg task N], then
	 * task_yield once to give the child a quantum to run. The
	 * shell's prompt comes back without waiting; user can `wait N`
	 * later to harvest the exit code (or never, in which case the
	 * task descriptor sits in the libc task table EXITED until the
	 * shell exits). */
	char path[PATH_MAX];
	int  background = 0;

	int alen = (int)strlen(arg);
	while (alen > 0 && (arg[alen - 1] == ' ' || arg[alen - 1] == '\t'))
		alen--;
	if (alen > 0 && arg[alen - 1] == '&') {
		background = 1;
		alen--;
		while (alen > 0 && (arg[alen - 1] == ' ' || arg[alen - 1] == '\t'))
			alen--;
	}

	/* Copy the trimmed-and-de-ampersanded arg into a stack buffer
	 * so resolve_path doesn't see the trailing '&'. */
	char arg_copy[PATH_MAX];
	if (alen >= PATH_MAX) alen = PATH_MAX - 1;
	memcpy(arg_copy, arg, (unsigned int)alen);
	arg_copy[alen] = '\0';

	resolve_path(cwd, arg_copy, path);

	if (background) {
		task_t t = orx_spawn(path);
		if (t < 0) {
			term_print("orx_spawn failed: ");
			term_print_int(t);
			term_print("\n");
			return;
		}
		term_print(run_bg_pre);
		term_print_int(t);
		term_print(run_bg_post);
		/* Hand the child its first quantum. With cooperative
		 * scheduling and no preemption, this is the only chance
		 * a CPU-bound child has to make progress before the
		 * shell blocks again on the next keystroke. */
		task_yield();
	} else {
		int code = orx_run(path);
		term_print(run_done_pre);
		term_print_int(code);
		term_print(run_done_post);
	}
}

static void
cmd_wait(const char *arg)
{
	int t = atoi(arg);
	/* orx_unload = task_wait + ObjFreeDeferred(code/data/stack) +
	 * task_free. Safe on tasks that weren't orx-spawned (manifest is
	 * empty → the deferred-frees noop). */
	int code = orx_unload((task_t)t);
	if (code < 0) {
		term_print("wait: bad task or task_wait error\n");
		return;
	}
	term_print("[task ");
	term_print_int(t);
	term_print(" exited ");
	term_print_int(code);
	term_print("]\n");
}

/* --- view: full-screen file viewer on the grid canvas (Phase 38) -----
 *
 * Reads the whole file into a stack buffer (cap: VIEW_BUF_BYTES),
 * indexes its line starts (cap: VIEW_MAX_LINES), and paints a
 * window of it onto the oriscterm grid canvas (80×24). Bottom row
 * is a status line; the remaining 23 are content. Arrow keys / vi
 * keys scroll; q or ESC quit. Files larger than the buffer get
 * truncated with a "(truncated)" suffix in the status; lines
 * longer than the canvas get clipped at column 80.
 *
 * No pager-style backwards reads — we hold the whole buffer in
 * memory. That's good enough for a Phase 38 demo on the typical
 * 4-32KB shell scripts and source files we might want to read.
 * Bigger files want a paging design; out of scope for now. */

#define VIEW_BUF_BYTES   8192
#define VIEW_MAX_LINES   512
#define VIEW_GRID_COLS   80
#define VIEW_GRID_ROWS   24
#define VIEW_CONTENT_ROWS 23

/* Bundled so view_render fits in pcc's 4-register arg limit. */
struct view_state {
	const char *buf;
	const int  *line_off;
	int         n_lines;
	int         top_row;
	const char *path;
	int         truncated;
};

/* Append 1+ chars to a status-line buffer, advancing *sp. Caps at
 * the grid column count so we never overrun the row. */
static int
view_status_append(char *status, int sp, const char *s)
{
	while (*s && sp < VIEW_GRID_COLS) status[sp++] = *s++;
	return sp;
}

static int
view_status_append_int(char *status, int sp, int v)
{
	char numbuf[12];
	int nb = 0;
	int j;
	if (v == 0) numbuf[nb++] = '0';
	while (v > 0) { numbuf[nb++] = '0' + (v % 10); v /= 10; }
	for (j = nb - 1; j >= 0 && sp < VIEW_GRID_COLS; j--)
		status[sp++] = numbuf[j];
	return sp;
}

static void
view_render(struct view_state *vs)
{
	int i;
	int row;
	int len;
	int start;
	int end;
	char status[VIEW_GRID_COLS];
	int sp;

	grid_clear();

	for (i = 0; i < VIEW_CONTENT_ROWS; i++) {
		row = vs->top_row + i;
		if (row >= vs->n_lines) break;
		start = vs->line_off[row];
		end   = (row + 1 < vs->n_lines)
		        ? vs->line_off[row + 1]
		        : vs->line_off[vs->n_lines];
		/* Drop the trailing \n so it doesn't render as a glyph. */
		if (end > start && vs->buf[end - 1] == '\n') end--;
		len = end - start;
		if (len > VIEW_GRID_COLS) len = VIEW_GRID_COLS;
		if (len > 0)
			grid_print_n(0, i, vs->buf + start, len);
	}

	sp = 0;
	sp = view_status_append(status, sp, "view: ");
	sp = view_status_append(status, sp, vs->path);
	if (vs->truncated)
		sp = view_status_append(status, sp, " (truncated)");
	sp = view_status_append(status, sp, "  ");
	sp = view_status_append_int(status, sp, vs->top_row + 1);
	sp = view_status_append(status, sp, "/");
	sp = view_status_append_int(status, sp, vs->n_lines);
	sp = view_status_append(status, sp, "  q=quit");
	grid_print_n(0, VIEW_CONTENT_ROWS, status, sp);
}

static void
cmd_view(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	char buf[VIEW_BUF_BYTES];
	int  line_off[VIEW_MAX_LINES + 1];
	int  buf_len = 0;
	int  n_lines = 0;
	int  top_row = 0;
	int  truncated = 0;
	int  fd;
	int  n;
	int  i;
	int  key;
	int  mods;
	int  max_top;

	resolve_path(cwd, arg, path);
	fd = hf_open(path, HF_O_RDONLY);
	if (fd < 0) {
		term_print("view: cannot open '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	while (buf_len < VIEW_BUF_BYTES
	       && (n = hf_read(fd, buf + buf_len,
	                       VIEW_BUF_BYTES - buf_len)) > 0) {
		buf_len += n;
	}
	hf_close(fd);
	/* Best-effort detection of "we capped out": if a final read filled
	 * us to the brim, the file MIGHT be longer. We don't know without
	 * another read; mark truncated to be honest about it. */
	if (buf_len >= VIEW_BUF_BYTES) truncated = 1;

	/* Index line starts. line_off[i] is the byte offset of line i;
	 * line_off[n_lines] is the end-of-buffer sentinel. */
	line_off[0] = 0;
	n_lines = 0;
	for (i = 0; i < buf_len; i++) {
		if (buf[i] == '\n') {
			n_lines++;
			if (n_lines >= VIEW_MAX_LINES) {
				truncated = 1;
				break;
			}
			line_off[n_lines] = i + 1;
		}
	}
	/* Tail line without a trailing \n. */
	if (n_lines < VIEW_MAX_LINES
	    && (n_lines == 0 || line_off[n_lines] < buf_len)) {
		n_lines++;
	}
	line_off[n_lines] = buf_len;

	for (;;) {
		struct view_state vs;
		max_top = n_lines - VIEW_CONTENT_ROWS;
		if (max_top < 0) max_top = 0;
		if (top_row > max_top) top_row = max_top;
		if (top_row < 0) top_row = 0;

		vs.buf       = buf;
		vs.line_off  = line_off;
		vs.n_lines   = n_lines;
		vs.top_row   = top_row;
		vs.path      = path;
		vs.truncated = truncated;
		view_render(&vs);

		key = term_getkey(&mods);
		if (key == 'q' || key == 'Q' || key == TK_ESCAPE) {
			break;
		} else if (key == TK_UP || key == 'k') {
			top_row--;
		} else if (key == TK_DOWN || key == 'j') {
			top_row++;
		} else if (key == ' ') {
			top_row += VIEW_CONTENT_ROWS;
		} else if (key == 'b' || key == TK_BACKSPACE) {
			top_row -= VIEW_CONTENT_ROWS;
		} else if (key == 'g') {
			top_row = 0;
		} else if (key == 'G') {
			top_row = max_top;
		}
	}

	/* Wipe the canvas before returning so the shell prompt doesn't
	 * sit alongside the last viewed frame. */
	grid_clear();
}

/* --- edit: a small full-screen editor on the grid canvas (Phase 39) --
 *
 * Loads <path> if it exists (or starts empty), keeps the buffer
 * in a 100×96 char array on the stack, repaints the visible
 * window after every keystroke. Modeless editing — printable
 * keys insert at the cursor, ENTER splits a line, BACKSPACE
 * deletes (and merges with the previous line at column 0),
 * arrow keys move the cursor, Ctrl-S saves to <path>, Ctrl-X
 * quits. A '_' is painted at the cursor cell so you can see
 * where you are; it overlays whatever character is under the
 * cursor (acceptable for a first cut).
 *
 * No undo, no search, no horizontal scroll, no dirty-prompt on
 * exit. Lines longer than EDIT_LINE_MAX get clipped at load
 * time; lines longer than the visible 80 cols are clipped at
 * render time. Files larger than the buffer get the tail
 * dropped with a "(truncated)" tag in the status line — same
 * convention as cmd_view.
 *
 * Status line: "edit: PATH [*] L,C   ^S=save ^X=quit". The '*'
 * appears once the buffer has been modified since load (not
 * since save — set, never cleared until quit, intentionally
 * simple).
 */

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
	char  path[PATH_MAX];
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

	/* Cursor: an underscore at the cursor cell. Overlays whatever
	 * char is there. The user knows what they typed; this just
	 * shows where the next insert will land. */
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

/* Insert one printable char at the cursor. Returns 1 if accepted,
 * 0 if dropped (line at capacity). */
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

/* Backspace at cursor. Three cases:
 *   - cur_col > 0:  delete the char to the left, shift tail.
 *   - cur_col == 0 && cur_row > 0:  merge with previous line. The
 *     previous line absorbs the current one's contents, the
 *     cursor lands at the join point.
 *   - cur_col == 0 && cur_row == 0:  no-op.
 */
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
		/* Shift remaining lines up. */
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

/* ENTER at cursor. Split current line at cur_col; everything from
 * cur_col onwards becomes a new line below. No-op if the buffer
 * is at line capacity. */
static void
edit_newline(struct edit_state *es)
{
	int i;
	int j;
	int cur_len;
	int tail_len;

	if (es->n_lines >= EDIT_MAX_LINES) return;

	/* Shift later lines down by one. */
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

/* Clamp the cursor column to the current line's length. Called
 * after vertical moves so the cursor doesn't sit past EOL. */
static void
edit_clamp_cur_col(struct edit_state *es)
{
	int len = es->line_lens[es->cur_row];
	if (es->cur_col > len) es->cur_col = len;
	if (es->cur_col < 0) es->cur_col = 0;
}

/* Bring the cursor row into view by adjusting top_row. */
static void
edit_scroll_into_view(struct edit_state *es)
{
	if (es->cur_row < es->top_row) es->top_row = es->cur_row;
	if (es->cur_row >= es->top_row + EDIT_CONTENT_ROWS)
		es->top_row = es->cur_row - EDIT_CONTENT_ROWS + 1;
	if (es->top_row < 0) es->top_row = 0;
}

/* Save buffer to the named path. Returns 0 OK, negative on error. */
static int
edit_save(struct edit_state *es)
{
	int fd;
	int i;
	char nl = '\n';

	fd = hf_open(es->path, HF_O_WRONLY | HF_O_CREAT | HF_O_TRUNC);
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

/* Load <path> into the buffer. Missing file → empty buffer. Lines
 * longer than EDIT_LINE_MAX-1 get clipped; line count past
 * EDIT_MAX_LINES sets es->truncated. */
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
	/* Strip a trailing empty line that came from a final '\n' on the
	 * file — most editors don't surface that as a separate row. */
	if (es->n_lines > 1 && es->line_lens[es->n_lines - 1] == 0)
		es->n_lines--;
	hf_close(fd);
}

static void
cmd_edit(const char *cwd, const char *arg)
{
	struct edit_state es;
	int key;
	int mods;

	resolve_path(cwd, arg, es.path);
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
		/* Anything else (ESC, special keys we don't handle, ctrl
		 * combos) is silently ignored. */
	}

	grid_clear();
}

/* SIGKILL-equivalent exit code. POSIX shells report 128 + signum
 * for signal-killed children (137 = 128 + 9 = SIGKILL); we mirror
 * the convention so `kill N` followed by a `wait N` shows 137. */
#define KILL_EXIT_CODE 137

static void
cmd_kill(const char *arg)
{
	int t;
	int rc;

	if (*arg == 0) {
		term_print("usage: kill <task>\n");
		return;
	}
	t = atoi(arg);
	rc = task_kill((task_t)t, KILL_EXIT_CODE);
	if (rc < 0) {
		term_print("kill: bad task or task_kill error\n");
		return;
	}
	/* Don't print anything on success — the auto-reaper at the
	 * top of the next prompt iteration will print the standard
	 * "[task N done 137]" line. */
}

/* Map TASK_STATE_* to a short label printed by cmd_jobs. */
static const char *
task_state_label(int state)
{
	switch (state) {
	case TASK_STATE_NEW:       return "new";
	case TASK_STATE_RUNNABLE:  return "runnable";
	case TASK_STATE_RUNNING:   return "running";
	case TASK_STATE_SUSPENDED: return "suspended";
	case TASK_STATE_BLOCKED:   return "blocked";
	case TASK_STATE_EXITED:    return "exited";
	}
	return "?";
}

static void
cmd_jobs(void)
{
	unsigned int mask = task_active_mask();
	int t;
	int found = 0;
	for (t = 0; t < TASK_MAX_CONCURRENT; t++) {
		if (!(mask & (1 << t))) continue;
		struct task_info info;
		if (task_query((task_t)t, &info) != 0) continue;
		term_print("[task ");
		term_print_int(t);
		term_print("] ");
		term_print(task_state_label(info.state));
		if (info.state == TASK_STATE_EXITED) {
			term_print(" (exit ");
			term_print_int(info.exit_code);
			term_print(")");
		}
		term_print("\n");
		found = 1;
	}
	if (!found)
		term_print("(no live tasks)\n");
}

/* Auto-reaper. Called each prompt iteration: scans the libc task
 * table, prints "[task N done CODE]" for any EXITED entries, and
 * orx_unloads them so the slot frees up for the next spawn. */
static void
reap_exited_tasks(void)
{
	unsigned int mask = task_active_mask();
	int t;
	for (t = 0; t < TASK_MAX_CONCURRENT; t++) {
		if (!(mask & (1 << t))) continue;
		struct task_info info;
		if (task_query((task_t)t, &info) != 0) continue;
		if (info.state != TASK_STATE_EXITED) continue;
		int code = orx_unload((task_t)t);
		term_print("[task ");
		term_print_int(t);
		term_print(" done ");
		term_print_int(code);
		term_print("]\n");
	}
}

static void
cmd_cycles(void)
{
	term_print_int((int)read_cycles());
	term_print("\n");
}

static void
cmd_time(void)
{
	/* Microseconds since boot (32-bit, wraps at ~71 min). Useful
	 * mostly as a wall-clock companion to `cycles`: cycles tells
	 * you how much CPU work happened; time tells you how much
	 * wall-clock time elapsed. */
	term_print_int((int)time_now_us());
	term_print(" us\n");
}

/* --- main ------------------------------------------------------------- */

int
main(void)
{
	char line[LINE_MAX];
	char cwd[PATH_MAX];
	char prompt_buf[PATH_MAX + 16];
	struct history h;

	cwd[0] = '/';
	cwd[1] = 0;
	h.head = 0;
	h.count = 0;

	/* task_init MUST run first — it parks the boot O1 (code ref) into
	 * O13 before main clobbers O1. Term/hf inits happen after; their
	 * boot-saves (O11/O14/O15) overlap by design (matching values).
	 * orx_spawn uses the libc task table parked in O12 by task_init. */
	task_init();

	/* term_init parks boot O2/O3/O4 into O11/O14/O15; we don't need
	 * to touch them ourselves here. (See term.c on why we avoid
	 * `register __or __asm__("oN")` declarations for the saves.) */
	term_init();
	hf_init();

	/* Wire the preemption timer. From here on, a CPU-bound bg task
	 * spawned via `run cmd &` can't starve the shell — the handler
	 * fires every 5000 cycles, calls TaskYield (deferred), and ERET
	 * picks the next runnable. Phase 36. */
	task_install_preempt_timer(5000);

	term_print(banner);
	term_print(hello1);

	while (1) {
		int len;
		char *arg;

		/* Auto-reap any background tasks that exited since the last
		 * iteration. Prints "[task N done CODE]" before the prompt
		 * if there's anything to harvest, mirroring the bash style. */
		reap_exited_tasks();

		print_prompt(cwd, prompt_buf);
		len = read_line(line, sizeof(line), &h);
		if (len == 0) continue;

		arg = split_arg(line);

		if (strcmp(line, "help") == 0) {
			cmd_help();
		} else if (strcmp(line, "cat") == 0) {
			if (*arg == 0) term_print("usage: cat <path>\n");
			else cmd_cat(cwd, arg);
		} else if (strcmp(line, "more") == 0) {
			if (*arg == 0) term_print("usage: more <path>\n");
			else cmd_more(cwd, arg);
		} else if (strcmp(line, "view") == 0) {
			if (*arg == 0) term_print("usage: view <path>\n");
			else cmd_view(cwd, arg);
		} else if (strcmp(line, "edit") == 0) {
			if (*arg == 0) term_print("usage: edit <path>\n");
			else cmd_edit(cwd, arg);
		} else if (strcmp(line, "ls") == 0) {
			cmd_ls(cwd, *arg ? arg : ".");
		} else if (strcmp(line, "cd") == 0) {
			cmd_cd(cwd, *arg ? arg : "/");
		} else if (strcmp(line, "pwd") == 0) {
			cmd_pwd(cwd);
		} else if (strcmp(line, "echo") == 0) {
			cmd_echo(arg);
		} else if (strcmp(line, "run") == 0) {
			if (*arg == 0) term_print("usage: run <path> [&]\n");
			else cmd_run(cwd, arg);
		} else if (strcmp(line, "wait") == 0) {
			if (*arg == 0) term_print("usage: wait <task>\n");
			else cmd_wait(arg);
		} else if (strcmp(line, "kill") == 0) {
			cmd_kill(arg);
		} else if (strcmp(line, "jobs") == 0) {
			cmd_jobs();
		} else if (strcmp(line, "cycles") == 0) {
			cmd_cycles();
		} else if (strcmp(line, "time") == 0) {
			cmd_time();
		} else if (strcmp(line, "exit") == 0
		           || strcmp(line, "quit") == 0) {
			term_print("bye!\n");
			return 0;
		} else {
			term_print("unknown command: '");
			term_print(line);
			term_print("'  (try 'help')\n");
		}
	}
}
