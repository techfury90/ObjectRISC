/*
 * rtc_layout_smoke.c — exercises rtc_layout's OBJECT path end to end: build a
 * Document of blocks, bridge each __or block ref to an obj.h handle
 * (objor_stash_o7 + obj_adopt_o7), and lay the handles out. This validates the
 * novel part — walking the doc, fetching each block's bytes into a stack
 * buffer (obj_fetch_to_stack), parsing the header (kind/style/text_len), and
 * emitting the display list. Text is short (<= one line) so the assertions
 * pin down per-block placement rather than wrap (wrap is covered by
 * rtc_wrap_smoke).
 *
 * Returns 42 on success; a smaller code marks the first failed step.
 */

#include "liborisc.h"
#include "obj.h"
#include "obj_or.h"
#include "doc.h"
#include "rtc.h"

#define NB  4

static obj_t g_handles[NB];   /* global -> absolute address (safe from an __or frame) */
static int   cw[256];

static int
run(void)
{
	void *__or doc;
	void *__or blocks;
	void *__or src;
	void *__or b;
	int i, count, n;
	int kinds[NB];

	kinds[0] = BLK_H1;
	kinds[1] = BLK_PARA;
	kinds[2] = BLK_RULE;      /* no text */
	kinds[3] = BLK_PARA;

	doc = doc_new(2);
	if (objor_isnull(doc))
		return 2;

	/* build NB blocks: short 4-byte text (0 for the rule), style-id = i */
	count = 0;
	for (i = 0; i < NB; i++) {
		int tlen = (kinds[i] == BLK_RULE) ? 0 : 4;
		src = objor_alloc(8, OBJ_TAG_DATA,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(src))
			return 3;
		objor_storew(src, 0x41424344);      /* "ABCD" — 4 text bytes */
		b = block_new(kinds[i], i, src, 0, tlen);
		if (objor_isnull(b))
			return 4;
		objor_free(src);
		blocks = orvec_push(doc_blocks(doc), b, count);
		if (objor_isnull(blocks))
			return 5;
		doc_set_blocks(doc, blocks);
		count++;
	}

	/* bridge each block ref (__or) to a handle for the handle-based layout */
	blocks = doc_blocks(doc);
	for (i = 0; i < count; i++) {
		objor_stash_o7(objor_vget(blocks, i));
		g_handles[i] = obj_adopt_o7();
		if (obj_isnull(g_handles[i]))
			return 6;
	}

	for (i = 0; i < 256; i++)
		cw[i] = 8;

	n = rtc_layout(g_handles, count, 80, cw);
	if (n != NB)                       /* each short/rule block -> exactly 1 line */
		return 7;

	/* per-line: source block index, kind, text length, and stacked y */
	if (rtc_line_block(0) != 0 || rtc_line_kind(0) != BLK_H1  || rtc_line_len(0) != 4) return 8;
	if (rtc_line_block(1) != 1 || rtc_line_kind(1) != BLK_PARA || rtc_line_len(1) != 4) return 9;
	if (rtc_line_block(2) != 2 || rtc_line_kind(2) != BLK_RULE || rtc_line_len(2) != 0) return 10;
	if (rtc_line_block(3) != 3 || rtc_line_kind(3) != BLK_PARA || rtc_line_len(3) != 4) return 11;

	/* y stacks by per-kind height: H1 24, PARA 16, RULE 10 */
	if (rtc_line_y(0) != 0)  return 12;   /* H1   at 0,  h=24 */
	if (rtc_line_y(1) != 24) return 13;   /* PARA at 24, h=16 */
	if (rtc_line_y(2) != 40) return 14;   /* RULE at 40, h=10 */
	if (rtc_line_y(3) != 50) return 15;   /* PARA at 50 */
	if (rtc_total_height() != 66) return 16;

	return 42;
}

int
main(void)
{
	task_init();
	obj_init();                        /* handle table for the __or->handle bridge */
	return run();
}
