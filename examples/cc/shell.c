/*
 * shell.c — minimal interactive shell for the Object RISC graphical
 * terminal. MVP: prompt, line input, a handful of built-ins.
 *
 * Built-ins:
 *     help        — short list of commands
 *     cat <path>  — print the contents of a host file
 *     ls [path]   — list a host directory (default: ".")
 *     exit / quit — end the session
 *
 * The runner script (run_shell.sh) computes a build-date banner
 * and passes it through -DBUILD_BANNER so a fresh build always
 * announces itself with the current real-world date shifted back
 * 40 years (the alternate-history conceit).
 *
 * Boot ABI (set up by run_shell.sh via --service order):
 *     O5  = oriscterm console  (idx 1)
 *     O6  = oriscterm keyboard (idx 2)
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

const char banner[]  = BUILD_BANNER;
const char prompt[]  = "orisc> ";
const char hello1[]  = "\nType 'help' for commands. End with 'exit'.\n";
const char help_msg[] =
    "Commands:\n"
    "  help            — this message\n"
    "  cat <path>      — print the contents of a host file\n"
    "  ls [<path>]     — list a host directory (default: '.')\n"
    "  exit | quit     — leave the shell\n"
    "\n"
    "Backspace edits the line buffer but the terminal display is\n"
    "append-only, so corrections aren't visually undone yet.\n";

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

/* --- commands --------------------------------------------------------- */

static void
cmd_help(void)
{
	term_print(help_msg);
}

static void
cmd_cat(const char *path)
{
	char buf[READ_BUF];
	int fd = hf_open(path, HF_O_RDONLY);
	int n, i;
	if (fd < 0) {
		term_print("cat: cannot open '");
		term_print(path);
		term_print("'\n");
		return;
	}
	while ((n = hf_read(fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < n; i++) term_print_char(buf[i]);
	}
	hf_close(fd);
}

static void
cmd_ls(const char *path)
{
	char buf[READ_BUF];
	int fd = hf_opendir(path);
	int n, i;
	if (fd < 0) {
		term_print("ls: cannot open '");
		term_print(path);
		term_print("'\n");
		return;
	}
	while ((n = hf_read(fd, buf, sizeof(buf))) > 0) {
		for (i = 0; i < n; i++) term_print_char(buf[i]);
	}
	hf_close(fd);
}

/* --- main ------------------------------------------------------------- */

int
main(void)
{
	char line[LINE_MAX];

	/* term_init parks boot O2/O3/O4 into O11/O14/O15; we don't need
	 * to touch them ourselves here. (See term.c on why we avoid
	 * `register __or __asm__("oN")` declarations for the saves.) */
	term_init();
	hf_init();

	term_print(banner);
	term_print(hello1);

	while (1) {
		int len;
		char *arg;

		term_print(prompt);
		len = read_line(line, sizeof(line));
		if (len == 0) continue;

		arg = split_arg(line);

		if (strcmp(line, "help") == 0) {
			cmd_help();
		} else if (strcmp(line, "cat") == 0) {
			if (*arg == 0) term_print("usage: cat <path>\n");
			else cmd_cat(arg);
		} else if (strcmp(line, "ls") == 0) {
			cmd_ls(*arg ? arg : ".");
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
