/*
 * rtc_doc_smoke.c — the doc producer + layout, end to end on REAL text. Builds
 * a Document from C strings via block_from_mem (normal memory -> Block), bridges
 * each block to a handle, and lays it out with rtc_layout + a stub advance
 * table (8px/char, so width 80 fits 10 chars). Asserts the display list — a
 * heading, a paragraph that wraps into three lines, and a short paragraph —
 * with correct source block, byte spans, and stacked y. Exercises the whole
 * object stack: producer -> orvec -> doc -> __or->handle bridge -> fetch ->
 * wrap. Returns 42 on success; a smaller code marks the first failed step.
 */

#include "liborisc.h"
#include "obj.h"
#include "obj_or.h"
#include "doc.h"
#include "rtc.h"

#define NB  3

static obj_t g_handles[NB];
static int   cw[256];

static int
run(void)
{
	void *__or doc;
	void *__or blocks;
	void *__or b;
	int i, count, n;

	doc = doc_new(2);
	if (objor_isnull(doc))
		return 2;

	/* Build the blocks INLINE (no int-param helper — that would trip the
	 * pcc-or-frame miscompile); block_from_mem is asm, so passing the
	 * literal kind/style/len to it directly is fine. */
	count = 0;
	b = block_from_mem(BLK_H1, 0, "Title", 5);
	if (objor_isnull(b)) return 3;
	blocks = orvec_push(doc_blocks(doc), b, count);
	if (objor_isnull(blocks)) return 4;
	doc_set_blocks(doc, blocks); count++;

	b = block_from_mem(BLK_PARA, 1, "aaaaaaaaaa bbbbbbbbbb cccccccccc", 32);
	if (objor_isnull(b)) return 3;
	blocks = orvec_push(doc_blocks(doc), b, count);
	if (objor_isnull(blocks)) return 4;
	doc_set_blocks(doc, blocks); count++;

	b = block_from_mem(BLK_PARA, 2, "short one", 9);
	if (objor_isnull(b)) return 3;
	blocks = orvec_push(doc_blocks(doc), b, count);
	if (objor_isnull(blocks)) return 4;
	doc_set_blocks(doc, blocks); count++;

	/* bridge each block ref to a handle for the handle-based layout */
	blocks = doc_blocks(doc);
	for (i = 0; i < count; i++) {
		objor_stash_o7(objor_vget(blocks, i));
		g_handles[i] = obj_adopt_o7();
		if (obj_isnull(g_handles[i]))
			return 4;
	}
	for (i = 0; i < 256; i++)
		cw[i] = 8;

	n = rtc_layout(g_handles, count, 80, cw);
	if (n != 5)                        /* 1 (H1) + 3 (wrapped para) + 1 (short) */
		return 5;

	/* H1 "Title" */
	if (rtc_line_block(0)!=0 || rtc_line_kind(0)!=BLK_H1 ||
	    rtc_line_off(0)!=0 || rtc_line_len(0)!=5 || rtc_line_y(0)!=0)
		return 6;
	/* paragraph wraps into three 10-char lines (h=16 each, after H1's 24) */
	if (rtc_line_block(1)!=1 || rtc_line_off(1)!=0  || rtc_line_len(1)!=10 || rtc_line_y(1)!=24)
		return 7;
	if (rtc_line_block(2)!=1 || rtc_line_off(2)!=11 || rtc_line_len(2)!=10 || rtc_line_y(2)!=40)
		return 8;
	if (rtc_line_block(3)!=1 || rtc_line_off(3)!=22 || rtc_line_len(3)!=10 || rtc_line_y(3)!=56)
		return 9;
	/* short paragraph -> one line */
	if (rtc_line_block(4)!=2 || rtc_line_off(4)!=0 || rtc_line_len(4)!=9 || rtc_line_y(4)!=72)
		return 10;
	if (rtc_total_height() != 88)
		return 11;

	return 42;
}

int
main(void)
{
	task_init();
	obj_init();
	return run();
}
