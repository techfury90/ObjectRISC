/*
 * rtc_wrap_smoke.c — exercises the Rich Text Control's word-wrap algorithm
 * (rtc_wrap_text, the object-free heart of rtc_layout). No objects, no WM: it
 * wraps hand-picked strings with a stub advance table (every byte = 8px, so a
 * width of 80 fits exactly 10 chars) and checks the resulting display-list
 * lines — word breaks, hard breaks of an over-long word, an explicit newline,
 * and the empty case. Returns 42 on success; a smaller code marks the first
 * failed check.
 */

#include "liborisc.h"
#include "rtc.h"

static int cw[256];

int
main(void)
{
	int i;

	for (i = 0; i < 256; i++)
		cw[i] = 8;                 /* 8px/char -> width 80 fits 10 chars */

	/* 1. three 10-char words -> break at each space, 3 lines */
	if (rtc_wrap_text("aaaaaaaaaa bbbbbbbbbb cccccccccc", 32, 80, cw) != 3)
		return 2;
	if (rtc_line_off(0) != 0  || rtc_line_len(0) != 10) return 3;
	if (rtc_line_off(1) != 11 || rtc_line_len(1) != 10) return 4;
	if (rtc_line_off(2) != 22 || rtc_line_len(2) != 10) return 5;
	if (rtc_line_y(0) != 0 || rtc_line_y(1) != 16 || rtc_line_y(2) != 32)
		return 6;                  /* PARA line height 16 */

	/* 2. short text -> a single line */
	if (rtc_wrap_text("short", 5, 80, cw) != 1) return 7;
	if (rtc_line_off(0) != 0 || rtc_line_len(0) != 5) return 8;

	/* 3. one un-spaced 20-char word -> hard break every 10 chars */
	if (rtc_wrap_text("aaaaaaaaaaaaaaaaaaaa", 20, 80, cw) != 2) return 9;
	if (rtc_line_off(0) != 0  || rtc_line_len(0) != 10) return 10;
	if (rtc_line_off(1) != 10 || rtc_line_len(1) != 10) return 11;

	/* 4. an explicit newline forces a break (the newline is consumed) */
	if (rtc_wrap_text("ab\ncd", 5, 80, cw) != 2) return 12;
	if (rtc_line_off(0) != 0 || rtc_line_len(0) != 2) return 13;
	if (rtc_line_off(1) != 3 || rtc_line_len(1) != 2) return 14;

	/* 5. empty text -> no lines */
	if (rtc_wrap_text("", 0, 80, cw) != 0) return 15;

	return 42;
}
