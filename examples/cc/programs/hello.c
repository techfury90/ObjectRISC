/*
 * hello.c — the smallest possible "run me from the shell" demo.
 *
 * print_str writes to firmware-side ConsoleWrite, which lands on
 * the shell's stdout (visible in the terminal where you launched
 * run_shell.sh, NOT inside the oriscterm window). For Tk-window
 * output use term_print, but note the keyboard-subscription
 * conflict described in the Phase 30 HISTORY entry.
 */

#include "liborisc.h"

int
main(void)
{
	print_str("hello from a child task\n");
	return 0;
}
