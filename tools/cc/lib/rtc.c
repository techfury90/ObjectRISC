/*
 * rtc.c — Rich Text Control layout core (see rtc.h). Ordinary handle-based C
 * (no `void *__or`, hence no OBJSTORE frame): each block's bytes are fetched
 * into a stack buffer with obj_fetch_to_stack, then word-wrapped into the
 * module-global display list using the caller's advance table.
 */

#include "rtc.h"

#define STACK_BOTTOM  0x001f0000       /* obj_fetch_to_stack VA base (CONTRACT §2) */
#define RTC_TEXTMAX   1024             /* max text bytes laid out per block */

/* Per-kind line height (px). */
static int
line_height(int kind)
{
	if (kind == BLK_H1) return 24;
	if (kind == BLK_H2) return 20;
	if (kind == BLK_RULE) return 10;
	return 16;                         /* para / code / list */
}

/* Display list (persists across the call for the accessors). */
static int rl_block[RTC_MAX_LINES];
static int rl_off[RTC_MAX_LINES];
static int rl_len[RTC_MAX_LINES];
static int rl_y[RTC_MAX_LINES];
static int rl_kind[RTC_MAX_LINES];
static int rl_n;
static int rl_total_h;

int rtc_nlines(void)        { return rl_n; }
int rtc_total_height(void)  { return rl_total_h; }
int rtc_line_block(int i)   { return rl_block[i]; }
int rtc_line_off(int i)     { return rl_off[i]; }
int rtc_line_len(int i)     { return rl_len[i]; }
int rtc_line_y(int i)       { return rl_y[i]; }
int rtc_line_kind(int i)    { return rl_kind[i]; }

/* Emit one display line; returns 0 or -1 if the table is full. */
static int
emit_line(int bi, int off, int len, int y, int kind)
{
	if (rl_n >= RTC_MAX_LINES)
		return -1;
	rl_block[rl_n] = bi;
	rl_off[rl_n]   = off;
	rl_len[rl_n]   = len;
	rl_y[rl_n]     = y;
	rl_kind[rl_n]  = kind;
	rl_n++;
	return 0;
}

/* Word-wrap text[0..len) at `width` (px) using cw[]; emit lines for source
 * block `bi`/`kind` advancing *py by the line height each line. Breaks at the
 * last space that fits; a word longer than the line hard-breaks; a '\n' forces
 * a break. Returns 0, or -1 on table overflow. */
static int
wrap_block(int bi, int kind, const char *text, int len, int width,
           const int *cw, int *py)
{
	int i = 0;
	int lh = line_height(kind);

	while (i < len) {
		int w = 0;
		int j = i;
		int last_space = -1;
		int end;

		while (j < len) {
			unsigned char c = (unsigned char)text[j];
			if (c == '\n')
				break;                 /* hard line break */
			if (c == ' ')
				last_space = j;
			if (w + cw[c] > width && j > i)
				break;                 /* would overflow the line */
			w += cw[c];
			j++;
		}

		if (j < len && text[j] != '\n' && last_space > i)
			end = last_space;          /* break at the last fitting space */
		else
			end = j;                   /* hard break / newline / end of text */

		if (emit_line(bi, i, end - i, *py, kind) != 0)
			return -1;
		*py += lh;

		i = end;
		if (i < len && (text[i] == ' ' || text[i] == '\n'))
			i++;                       /* consume the break character */
	}
	return 0;
}

int
rtc_wrap_text(const char *text, int len, int width, const int *cw)
{
	int y = 0;

	rl_n = 0;
	if (wrap_block(0, BLK_PARA, text, len, width, cw, &y) != 0)
		return -1;
	rl_total_h = y;
	return rl_n;
}

int
rtc_layout(const obj_t *blocks, int nblocks, int width, const int *cw)
{
	/* Whole-block fetch buffer: header words + text. On the STACK (a local),
	 * because obj_fetch_to_stack targets the boot stack object. As int[] so
	 * the header words align; text bytes begin at (char*)buf + BLOCK_HDR. */
	int buf[(BLOCK_HDR + RTC_TEXTMAX) / 4];
	int bi;
	int y = 0;

	rl_n = 0;
	for (bi = 0; bi < nblocks; bi++) {
		obj_t h = blocks[bi];
		int bytelen = obj_len(h);
		int dst_off = (int)((unsigned int)buf - STACK_BOTTOM);
		int kind, text_len;
		char *text;

		if (bytelen < BLOCK_HDR)
			return -1;
		if (bytelen > BLOCK_HDR + RTC_TEXTMAX)
			bytelen = BLOCK_HDR + RTC_TEXTMAX;
		if (obj_fetch_to_stack(h, dst_off, bytelen) != 0)
			return -1;

		kind     = buf[0];                 /* header word 0 */
		text_len = buf[2];                 /* header word 2 */
		if (text_len > bytelen - BLOCK_HDR)
			text_len = bytelen - BLOCK_HDR;
		text = (char *)buf + BLOCK_HDR;

		if (text_len <= 0) {
			/* textless block (e.g. a rule) still occupies one line */
			if (emit_line(bi, 0, 0, y, kind) != 0)
				return -1;
			y += line_height(kind);
		} else if (wrap_block(bi, kind, text, text_len, width, cw, &y) != 0) {
			return -1;
		}
	}
	rl_total_h = y;
	return rl_n;
}
