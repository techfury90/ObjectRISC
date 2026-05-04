/*
 * shell.c — minimal interactive shell for the Object RISC graphical
 * terminal. MVP: prompt, line input, a handful of built-ins.
 *
 * Built-ins:
 *     help        — short list of commands
 *     cat <path>  — print the contents of a host file
 *     more <path> — like cat, but paginated (space/RET to advance, q to quit)
 *     ls [path]   — list a host directory (default: cwd)
 *     cd [path]   — change working directory (no arg → "/")
 *     pwd         — print the current working directory
 *     echo <text> — print the rest of the line
 *     run <path>  — load and run another .orx via linkbootd
 *     cycles      — print the CPU's cycle counter
 *     exit / quit — end the session
 *
 * Path handling: the shell maintains its own cwd (absolute, relative
 * to hostfsd's --root jail). cat / ls / run resolve relative args by
 * joining cwd + arg + collapsing "." / ".." components, then send
 * the result to the hostfsd / linkbootd as an absolute path.
 *
 * The runner script (run_shell.sh) computes a build-date banner
 * and passes it through -DBUILD_BANNER so a fresh build always
 * announces itself with the current real-world date shifted back
 * 40 years (the alternate-history conceit).
 *
 * Boot ABI (set up by run_shell.sh via --service order):
 *     O5  = oriscterm console  (idx 1)
 *     O6  = oriscterm keyboard (idx 2)
 *     O7  = linkbootd          (pid 18, idx 1)
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

const char banner[]  = BUILD_BANNER;
const char hello1[]  = "\nType 'help' for commands. End with 'exit'.\n";
const char help_msg[] =
    "Commands:\n"
    "  help            — this message\n"
    "  cat <path>      — print the contents of a host file\n"
    "  more <path>     — like cat, but paginated (space/RET to advance, q to quit)\n"
    "  ls [<path>]     — list a host directory (default: cwd)\n"
    "  cd [<path>]     — change working directory (no arg → '/')\n"
    "  pwd             — print the current working directory\n"
    "  echo <text>     — print the rest of the line\n"
    "  run <path>      — load and run another .orx via linkbootd\n"
    "  cycles          — print the CPU's cycle counter\n"
    "  exit | quit     — leave the shell\n"
    "\n"
    "Backspace edits the line buffer but the terminal display is\n"
    "append-only, so corrections aren't visually undone yet.\n";

const char run_done_pre[] = "[exited ";
const char run_done_post[] = "]\n";
const char more_prompt[] = "--More-- (space/RET, q to quit)";

/* The shell maintains an absolute, normalized cwd ("/", "/foo",
 * "/a/b" etc.) and threads a pointer to it through every command
 * that touches it. We can't put it in a global because Object RISC's
 * data segment is mapped R-only by init_cpu — `cmd_cd` would fault
 * on the assignment. So cwd lives on main()'s stack along with a
 * scratch buffer for building the prompt string. */

/* --- input helpers ---------------------------------------------------- */

/* Read a line from the keyboard. Echoes printable chars to the
 * terminal as they come in. Backspace adjusts the buffer (no visual
 * undo). Enter (TK_RETURN) terminates the line; we print a newline
 * and NUL-terminate the buffer. Returns line length. */
static int
read_line(char *buf, int max)
{
	int n = 0;
	while (1) {
		int mods;
		int c = term_getkey(&mods);
		if (c < 0) { buf[0] = 0; return 0; }
		if (c == TK_RETURN) {
			term_print("\n");
			buf[n] = 0;
			return n;
		}
		if (c == TK_BACKSPACE) {
			if (n > 0) n--;
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
	/* One SEND per chunk (term_print_n), not per byte. The
	 * single-byte path through term_print_char issues a SEND +
	 * OBJ_READ round-trip per byte and overruns oriscterm's
	 * receive socket on any non-trivial file. */
	while ((n = hf_read(fd, buf, sizeof(buf))) > 0) {
		term_print_n(buf, n);
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
		 * paginate at line boundaries without splitting bytes. */
		int seg_start = 0;
		int i;
		for (i = 0; i < n; i++) {
			if (buf[i] == '\n') {
				term_print_n(buf + seg_start, i - seg_start + 1);
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
		if (i > seg_start) term_print_n(buf + seg_start, i - seg_start);
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
		term_print_n(buf, n);
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
	char path[PATH_MAX];
	resolve_path(cwd, arg, path);
	int code = lb_spawn(path);
	term_print(run_done_pre);
	term_print_int(code);
	term_print(run_done_post);
}

static void
cmd_cycles(void)
{
	int n;
	asm volatile(
		"call  #0x301\n"               /* ReadCycles → R3 */
		"nop\n"
		"addu  %0, r3, r0"
		: "=r"(n)
		:
		: "r2", "r3"
	);
	term_print_int(n);
	term_print("\n");
}

/* --- main ------------------------------------------------------------- */

int
main(void)
{
	char line[LINE_MAX];
	char cwd[PATH_MAX];
	char prompt_buf[PATH_MAX + 16];

	cwd[0] = '/';
	cwd[1] = 0;

	/* term_init parks boot O2/O3/O4 into O11/O14/O15; we don't need
	 * to touch them ourselves here. (See term.c on why we avoid
	 * `register __or __asm__("oN")` declarations for the saves.) */
	term_init();
	hf_init();
	lb_init();

	term_print(banner);
	term_print(hello1);

	while (1) {
		int len;
		char *arg;

		print_prompt(cwd, prompt_buf);
		len = read_line(line, sizeof(line));
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
		} else if (strcmp(line, "ls") == 0) {
			cmd_ls(cwd, *arg ? arg : ".");
		} else if (strcmp(line, "cd") == 0) {
			cmd_cd(cwd, *arg ? arg : "/");
		} else if (strcmp(line, "pwd") == 0) {
			cmd_pwd(cwd);
		} else if (strcmp(line, "echo") == 0) {
			cmd_echo(arg);
		} else if (strcmp(line, "run") == 0) {
			if (*arg == 0) term_print("usage: run <path>\n");
			else cmd_run(cwd, arg);
		} else if (strcmp(line, "cycles") == 0) {
			cmd_cycles();
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
