/*
 * hello_term.c — guest that prints to the Tk oriscterm window.
 *
 * term_print_only_init parks the boot O2/O3/O4 into O11/O14/O15
 * (the same slots term.c expects) but skips the receive-queue
 * attach and keyboard-subscribe SEND that term_init does — those
 * would compete with the parent shell's keyboard subscription on
 * the same CPU. The child still inherits O5 (console service) via
 * TaskCreate's OPR copy, so term_print* lands on the same Tk
 * window the shell prints to.
 *
 * Output appears in the oriscterm window between the shell's
 * `[exited 0]` line and the next prompt.
 */

#include "liborisc.h"

int
main(void)
{
	term_print_only_init();
	term_print("hello from inside the Tk window\n");
	return 0;
}
