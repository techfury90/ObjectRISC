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
 *     ps            — list tasks across all live CPUs (Phase 52)
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
    "  ls [<path>]     — list a host directory (default: cwd)\n"
    "  cd [<path>]     — change working directory (no arg → '/')\n"
    "  pwd             — print the current working directory\n"
    "  mkdir <path>    — create a directory (parents must already exist)\n"
    "  rm <path>       — remove a file (refuses on directories)\n"
    "  touch <path>    — create an empty file or no-op if it exists\n"
    "  echo <text>     — print the rest of the line\n"
    "  title <text>    — set this window's title bar text\n"
    "  run <path>[&]   — load + run another .orx as a child task ('&' = background)\n"
    "  edit [<path>]   — shorthand for `run /programs/edit.orx <path> &`\n"
    "  wait <task>     — block until backgrounded task exits; print its exit code\n"
    "  kill <task>     — externally terminate a backgrounded task (exit 137)\n"
    "  jobs            — list backgrounded tasks and their state\n"
    "  ps              — list tasks across all live CPUs (cross-supervisor)\n"
    "  cycles          — print the CPU's cycle counter\n"
    "  time            — print microseconds since boot (wall clock)\n"
    "  logout          — end this session; login.orx welcomes the next user\n"
    "  exit | quit     — halt this CPU's supervisor (system shutdown)\n";

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

/* Phase 50: filesystem mutation builtins. All three take a single
 * path argument, run a one-shot vfs_* call, and print a short error
 * line on failure. No -p / -r flags — those would push us toward a
 * proper getopt-style parser, out of scope here.
 *
 * Errors we surface explicitly (vs. a generic "failed (-N)"):
 *   -3 (E_NOENT) = parent missing  → "no such file or directory"
 *   -4 (E_ACCES) = path escapes jail / no perm
 *                                   → "permission denied"
 *   -7 (E_EXIST) = entry exists (mkdir) / target is a directory
 *                  (rm)             → "file exists" / "is a directory" */
static void
cmd_print_fs_error(const char *cmd, const char *arg, int rc)
{
	term_print(cmd);
	term_print(": ");
	if      (rc == -3) term_print("no such file or directory: '");
	else if (rc == -4) term_print("permission denied: '");
	else if (rc == -7) term_print("file exists: '");
	else               term_print("error: '");
	term_print(arg);
	term_print("'");
	if (rc != -3 && rc != -4 && rc != -7) {
		term_print(" (");
		term_print_int(rc);
		term_print(")");
	}
	term_print("\n");
}

static void
cmd_mkdir(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	if (!*arg) { term_print("mkdir: missing operand\n"); return; }
	resolve_path(cwd, arg, path);
	int rc = vfs_mkdir(path);
	if (rc < 0) cmd_print_fs_error("mkdir", arg, rc);
}

static void
cmd_rm(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	if (!*arg) { term_print("rm: missing operand\n"); return; }
	resolve_path(cwd, arg, path);
	int rc = vfs_unlink(path);
	if (rc < 0) {
		/* Use a different label for "is a directory" — vfs_unlink
		 * returns E_EXIST in that case, but the user-facing message
		 * is more natural as "is a directory" since they're trying
		 * to delete it, not create it. */
		if (rc == -7) {
			term_print("rm: '");
			term_print(arg);
			term_print("' is a directory\n");
		} else {
			cmd_print_fs_error("rm", arg, rc);
		}
	}
}

static void
cmd_touch(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	if (!*arg) { term_print("touch: missing operand\n"); return; }
	resolve_path(cwd, arg, path);
	/* O_WRONLY | O_CREAT — create if missing, succeed if exists.
	 * No O_TRUNC: don't clobber existing content. The fd we get
	 * back is immediately closed; the side-effect is the file's
	 * existence with mtime updated by hostfsd's open. */
	int fd = vfs_open(path, HF_O_WRONLY | HF_O_CREAT);
	if (fd < 0) { cmd_print_fs_error("touch", arg, fd); return; }
	vfs_close(fd);
}

static void
cmd_cat(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	char buf[READ_BUF];
	resolve_path(cwd, arg, path);
	int fd = vfs_open(path, HF_O_RDONLY);
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
	 * receiver a window before our next vfs_read overwrote things;
	 * the sync variant closes that race entirely. */
	while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
		term_print_n_sync(buf, n);
	}
	vfs_close(fd);
}

static void
cmd_more(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	char buf[READ_BUF];
	int line_count = 0;
	resolve_path(cwd, arg, path);
	int fd = vfs_open(path, HF_O_RDONLY);
	int n;
	if (fd < 0) {
		term_print("more: cannot open '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
		/* Print the chunk in newline-bounded slices so we can
		 * paginate at line boundaries without splitting bytes.
		 * Each slice goes through term_print_n_sync so the
		 * receiver drains before we overwrite the buffer on the
		 * next vfs_read. */
		int seg_start = 0;
		int i;
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				term_print_n_sync(buf + seg_start, i - seg_start + 1);
				seg_start = i + 1;
				line_count++;
				if (line_count >= PAGE_LINES) {
					if (pause_for_key()) {
						vfs_close(fd);
						return;
					}
					line_count = 0;
				}
			}
		}
		if (i > seg_start) term_print_n_sync(buf + seg_start, i - seg_start);
	}
	vfs_close(fd);
}

/* Sized for vfs_list — has to hold the full directory listing in
 * one buffer (vfs_list doesn't stream). 1 KiB is plenty for the
 * current /programs and /sys/cpu listings; bumping it is free
 * until we start eating into the 64 KiB shell stack. */
#define LIST_BUF 1024

static void
cmd_ls(const char *cwd, const char *arg)
{
	char path[PATH_MAX];
	char buf[LIST_BUF];
	resolve_path(cwd, arg, path);
	int n = vfs_list(path, buf, sizeof(buf));
	if (n < 0) {
		term_print("ls: cannot list '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	if (n > 0) term_print_n_sync(buf, n);
}

static void
cmd_cd(char *cwd, const char *arg)
{
	char path[PATH_MAX];
	int kind;
	resolve_path(cwd, arg, path);
	/* Verify the path resolves to a navigable node. DIR (in-memory
	 * oriscdir tree, e.g. `/sys/cpu`) and MOUNT (path-translated to
	 * a backend, e.g. `/programs`) are both valid cwd targets;
	 * LEAF (a service ref like `/sys/cpu/0/supervisor`) and
	 * NOT_FOUND are not. vfs_walk_kind avoids the hostfsd
	 * round-trip the old hf_opendir+close did for pure DIRs. */
	int rc = vfs_walk_kind(path, &kind);
	if (rc < 0
	    || (kind != DIR_KIND_DIR && kind != DIR_KIND_MOUNT)) {
		term_print("cd: cannot enter '");
		term_print(arg);
		term_print("'\n");
		return;
	}
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

/* Phase 60 step 9 — set the WM-mediated window's title bar text.
 * `title foo bar baz` passes the rest-of-line literally (no quoting,
 * trailing whitespace preserved as-is — same shape as `echo`).  The
 * libc helper truncates to WM_MAX_TITLE_LEN characters; longer titles
 * silently lose the tail.  No-op (with a soft note) when the WM isn't
 * mediating the session. */
static void
cmd_title(const char *arg)
{
	int rc = wm_set_title(0, arg);
	if (rc != 0) {
		term_print("title: WM unavailable\n");
	}
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

	/* Phase 45e: parse a leading `@N ` (any single decimal digit
	 * 0-9) as an explicit target-CPU specifier for the spawn. The
	 * rest of `arg` proceeds through the existing path/args split
	 * unchanged. Without `@`, target_pid stays SUP_TARGET_ANY
	 * (Phase 51) so the supervisor round-robins the spawn across
	 * live CPUs; pass-through routes the child's terminal output
	 * back to OUR terminal regardless of where it lands. */
	int target_pid = SUP_TARGET_ANY;
	int p = 0;
	while (arg[p] == ' ' || arg[p] == '\t') p++;
	if (arg[p] == '@') {
		int q = p + 1;
		int n = 0, digits = 0;
		while (arg[q] >= '0' && arg[q] <= '9') {
			n = n * 10 + (arg[q] - '0');
			q++; digits++;
		}
		if (digits == 0
		    || (arg[q] != ' ' && arg[q] != '\t' && arg[q] != '\0')
		    || n > 254) {
			term_print("usage: run [@N] <path> [args] [&]\n");
			return;
		}
		target_pid = n;
		p = q;
		while (arg[p] == ' ' || arg[p] == '\t') p++;
	}
	arg = arg + p;     /* arg now points past the @N specifier */

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
	 * so we can mutate (NUL-terminate at the path/args split) and
	 * pass slices to orx. resolve_path doesn't see the trailing
	 * '&' or any args. */
	char arg_copy[PATH_MAX];
	if (alen >= PATH_MAX) alen = PATH_MAX - 1;
	memcpy(arg_copy, arg, (unsigned int)alen);
	arg_copy[alen] = '\0';

	/* Split path from args at the first whitespace. Everything
	 * after the first word goes through program_args() to the
	 * spawned program. */
	int psplit = 0;
	while (psplit < alen
	       && arg_copy[psplit] != ' ' && arg_copy[psplit] != '\t')
		psplit++;
	int args_start = psplit;
	while (args_start < alen
	       && (arg_copy[args_start] == ' '
	           || arg_copy[args_start] == '\t'))
		args_start++;
	arg_copy[psplit] = '\0';
	resolve_path(cwd, arg_copy, path);

	if (background) {
		task_t t = sup_spawn_at(target_pid, path,
		                        arg_copy + args_start, cwd);
		if (t < 0) {
			term_print("spawn failed: ");
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
		task_t t = sup_spawn_at(target_pid, path,
		                        arg_copy + args_start, cwd);
		if (t < 0) {
			term_print("spawn failed: ");
			term_print_int(t);
			term_print("\n");
			return;
		}
		int code = orx_unload(t);
		/* Phase 60 step 18 — no resubscribe needed.  The
		 * focus-model WM auto-reverts focus to the topmost
		 * remaining window when the foreground program destroys
		 * its own; our shell-side keyboard subscription is still
		 * parked in our window's per-wid slot. */
		term_print(run_done_pre);
		term_print_int(code);
		term_print(run_done_post);
	}
}

/* `edit <path>` is a thin builtin: hardcodes /programs/edit.orx as
 * the binary and threads the user's path through as args, always
 * backgrounded. Saves typing `run /programs/edit.orx foo.c &` —
 * which is the entire psychological win, since edit is the most
 * common bg-spawn the shell does. The args/cwd handoff matches
 * cmd_run's & path: sup_spawn fills both fields of the shared
 * argv buffer, edit reads them via program_args() / program_cwd()
 * and resolves relative paths against cwd just like the cmd_run
 * path does.
 *
 * Phase 59 / WM γ.14: round-robin spawn is fine again — workers
 * now lazy-acquire the leader's published WM-mediated GRID cap on
 * first spawn (see populate_child_term_slots in supervisor.c), so
 * edit on a worker rasterises through the framebuffer just like
 * on the leader.  Was pinned to SUP_TARGET_LOCAL in γ.9 because
 * worker WM mediation didn't exist yet. */
static void
cmd_edit(const char *cwd, const char *arg)
{
	task_t t = sup_spawn("/programs/edit.orx", arg, cwd);
	if (t < 0) {
		term_print("spawn failed: ");
		term_print_int(t);
		term_print("\n");
		return;
	}
	/* Phase 60 step 18 — back to backgrounded by default.  Step 13
	 * forced this foreground (orx_unload + term_resubscribe) because
	 * single-subscriber keyboard meant the shell had to know when
	 * edit released focus; with per-wid subscribers the WM auto-
	 * reverts focus to whichever window the user clicks (or to the
	 * new topmost when edit destroys its window), so edit can run
	 * concurrently with the shell again. */
	term_print(run_bg_pre);
	term_print_int(t);
	term_print(run_bg_post);
	task_yield();
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

/* `status` is supplied by the caller and MUST outlive this call's async
 * grid SENDs: grid_print_n is fire-and-forget (the terminal OBJ_READ_REQs
 * the bytes only after the CPU moves on — see grid.c), so a buffer in
 * view_render's own frame would be overwritten by the next term_getkey
 * before the fetch. The caller (cmd_view) keeps it in its persistent
 * loop frame. */
static void
view_render(struct view_state *vs, char *status)
{
	int i;
	int row;
	int len;
	int start;
	int end;
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
	/* Status-line buffer kept in cmd_view's frame, not view_render's:
	 * grid_print_n's async SEND outlives the call, and a view_render-
	 * local would be clobbered by the next term_getkey before the
	 * terminal fetches it (the bug that looked like a codegen issue). */
	char status[VIEW_GRID_COLS];
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
	fd = vfs_open(path, HF_O_RDONLY);
	if (fd < 0) {
		term_print("view: cannot open '");
		term_print(arg);
		term_print("'\n");
		return;
	}
	while (buf_len < VIEW_BUF_BYTES
	       && (n = vfs_read(fd, buf + buf_len,
	                        VIEW_BUF_BYTES - buf_len)) > 0) {
		buf_len += n;
	}
	vfs_close(fd);
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
		view_render(&vs, status);

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

/* Phase 52: cross-CPU `ps`. Asks each peer supervisor to list its
 * task table and prints the result with a "CPU N:" prefix.
 *
 * Discovery: walk /sys/cpu/<N>/supervisor for procids 0..MAX_PS_CPU.
 * Each registered supervisor publishes a R+S sub-cap to its spawn
 * mailbox at that path; we OREFLD the resolved ref into O1 and call
 * sup_list_tasks(), which SENDs op=5 and blocks on our reply mailbox
 * for the supervisor's text response.
 *
 * Skips procids that don't have a registered supervisor (e.g.,
 * single-CPU configs where only /sys/cpu/0 exists). Prints
 * "(no supervisors found)" if NONE respond — usually means the
 * shell wasn't launched under a supervisor at all (oriscrun
 * --service degenerate test config). */
#define PS_PATH_BUF 32
#define PS_LIST_BUF 1024
#define MAX_PS_CPU 8

/* Render "/sys/cpu/<n>/supervisor" into buf. */
static void
ps_render_path(int n, char *buf)
{
	const char prefix[] = "/sys/cpu/";
	const char suffix[] = "/supervisor";
	int i, p = 0;
	for (i = 0; prefix[i]; i++) buf[p++] = prefix[i];
	if (n >= 10) {
		buf[p++] = '0' + (n / 10);
		buf[p++] = '0' + (n % 10);
	} else {
		buf[p++] = '0' + n;
	}
	for (i = 0; suffix[i]; i++) buf[p++] = suffix[i];
	buf[p] = '\0';
}

/* Print `n` bytes of the supervisor's reply to the terminal. The
 * data is in `buf` already, just term_print one line at a time so
 * the print machinery doesn't need a NUL. */
static void
ps_print_reply(const char *buf, int n)
{
	int i;
	for (i = 0; i < n; i++) term_print_char(buf[i]);
}

static void
cmd_ps(void)
{
	char path[PS_PATH_BUF];
	char reply_buf[PS_LIST_BUF];
	int kind;
	char remainder[16];
	int found_any = 0;
	int n;
	int procid;

	for (procid = 0; procid < MAX_PS_CPU; procid++) {
		ps_render_path(procid, path);
		int rc = dir_walk(path, &kind, remainder, sizeof(remainder));
		if (rc < 0 || kind != DIR_KIND_LEAF) continue;
		/* dir_walk leaves the resolved ref in DIR_RESULT_SLOT
		 * (offset 616). Pull it into O1 so sup_list_tasks's
		 * entry asm can stash it to O14 as the recipient. */
		asm volatile(
			"orefld o1, 616(o12)"
			: : : "r1"
		);
		n = sup_list_tasks(reply_buf, sizeof(reply_buf));
		if (n < 0) {
			term_print("CPU ");
			term_print_int(procid);
			term_print(": (error ");
			term_print_int(n);
			term_print(")\n");
			continue;
		}
		term_print("CPU ");
		term_print_int(procid);
		term_print(":\n");
		if (n == 0) {
			term_print("  (no live tasks)\n");
		} else {
			ps_print_reply(reply_buf, n);
		}
		found_any = 1;
	}
	if (!found_any)
		term_print("(no supervisors found)\n");
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

	/* Phase 60 step 20 — every shell opens its own WM window.  Both
	 * the boot shell (spawned by login) and menu-spawned shells go
	 * through this; each gets its own wid and its own keyboard /
	 * pointer subscriber slots under the focus model.  Soft-fail
	 * when no WM is reachable (legacy test environments running
	 * shell against bare oriscterm) — we fall back to the caps the
	 * supervisor / parent already left in O5/O6/O7.  has_window
	 * gates the matching wm_destroy_window on exit. */
	int wm_wid = 0;
	int wm_has_window = 0;
	if (wm_open_session("shell", &wm_wid) == 0) {
		wm_has_window = 1;
	}

	/* term_init parks boot O2/O3/O4 into O11/O14/O15; we don't need
	 * to touch them ourselves here. (See term.c on why we avoid
	 * `register __or __asm__("oN")` declarations for the saves.) */
	term_init();
	hf_init();

	/* Pre-allocate orx's shared argv buffer so the per-spawn cost
	 * shrinks to a memcpy. Keeps orx_spawn predictable and avoids
	 * any first-spawn tax on the shell's preempt-driven scheduling. */
	orx_init();

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
		/* Second reap pass: a bg task may have exited WHILE we were
		 * blocked in read_line. Catching it here, between the input
		 * and the dispatch, lets the auto-reap message land before
		 * the user-typed command's output (so e.g. `wait 0` after a
		 * naturally-completing bg task prints "[task 0 done 0]"
		 * from us, then "[task 0 exited 0]" — wait succeeds with
		 * code 0 vs failing on a missing task). */
		reap_exited_tasks();
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
		} else if (strcmp(line, "ls") == 0) {
			cmd_ls(cwd, *arg ? arg : ".");
		} else if (strcmp(line, "cd") == 0) {
			cmd_cd(cwd, *arg ? arg : "/");
		} else if (strcmp(line, "pwd") == 0) {
			cmd_pwd(cwd);
		} else if (strcmp(line, "mkdir") == 0) {
			cmd_mkdir(cwd, arg);
		} else if (strcmp(line, "rm") == 0) {
			cmd_rm(cwd, arg);
		} else if (strcmp(line, "touch") == 0) {
			cmd_touch(cwd, arg);
		} else if (strcmp(line, "echo") == 0) {
			cmd_echo(arg);
		} else if (strcmp(line, "title") == 0) {
			cmd_title(arg);
		} else if (strcmp(line, "run") == 0) {
			if (*arg == 0) term_print("usage: run <path> [&]\n");
			else cmd_run(cwd, arg);
		} else if (strcmp(line, "edit") == 0) {
			cmd_edit(cwd, arg);
		} else if (strcmp(line, "wait") == 0) {
			if (*arg == 0) term_print("usage: wait <task>\n");
			else cmd_wait(arg);
		} else if (strcmp(line, "kill") == 0) {
			cmd_kill(arg);
		} else if (strcmp(line, "jobs") == 0) {
			cmd_jobs();
		} else if (strcmp(line, "ps") == 0) {
			cmd_ps();
		} else if (strcmp(line, "cycles") == 0) {
			cmd_cycles();
		} else if (strcmp(line, "time") == 0) {
			cmd_time();
		} else if (strcmp(line, "logout") == 0) {
			/* Phase 48: end this shell session, but DON'T halt
			 * the supervisor. login.orx (our parent) sees us
			 * exit cleanly via task_wait and loops back to its
			 * welcome banner, ready for another session. The
			 * supervisor stays up, oriscdir stays up, /programs
			 * stays mounted — it's just THIS shell that ends.
			 *
			 * term_shutdown unsubscribes our mailbox from the
			 * keyboard service so login.orx can take focus
			 * cleanly when it re-subscribes; without it,
			 * oriscterm would keep our dead sub-cap at idx 0
			 * and login's keystrokes would silently disappear
			 * into a stale queue. (exit/quit doesn't bother —
			 * sup_shutdown is about to halt everything.) */
			term_print("logged out\n");
			term_shutdown();
			if (wm_has_window) wm_destroy_window(wm_wid);
			return 0;
		} else if (strcmp(line, "exit") == 0
		           || strcmp(line, "quit") == 0) {
			term_print("bye!\n");
			term_shutdown();
			if (wm_has_window) wm_destroy_window(wm_wid);
			/* Phase 48: SEND op=2 to halt the supervisor.
			 * When a supervisor IS present, also yield
			 * forever — returning would cause login.task_wait
			 * (us) to wake, and login would race the
			 * supervisor's op=2 task_kill cascade, running
			 * its term_clear+banner once before being killed
			 * and leaving the screen flickering. Yielding
			 * keeps login BLOCKED in task_wait until the
			 * supervisor processes op=2 and task_kills both
			 * of us atomically.
			 *
			 * In the no-supervisor case (e.g. shell launched
			 * directly by oriscrun for test_shell.sh), there's
			 * no one to ever task_kill us — yielding forever
			 * would hang the CPU. Just return cleanly so
			 * TaskExit fires and the simulator unwinds. */
			int have_sup = sup_have_supervisor();
			sup_shutdown();
			if (have_sup) {
				for (;;) task_yield();
			}
			return 0;
		} else {
			term_print("unknown command: '");
			term_print(line);
			term_print("'  (try 'help')\n");
		}
	}
}
