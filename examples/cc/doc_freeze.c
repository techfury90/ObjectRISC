/*
 * doc_freeze.c — proves the document model (doc.h) end to end, including the
 * freeze-on-scroll-away lifecycle that is the whole point of the design.
 *
 *   1. build a Document of N blocks (each a self-contained byte Block whose
 *      style-id = its index and whose 4-byte text = the word BASE+i), growing
 *      the blocks orvec (start cap 2 -> 4 -> 8);
 *   2. verify all N read back (style + text) and count LIVE blocks == N;
 *   3. FREEZE the first K (the "scrolled-away" blocks): append each block's
 *      bytes to the text-log, free the object, null its orvec slot — and
 *      confirm the live-object count drops to N-K while the sequence length is
 *      unchanged;
 *   4. THAW those K by WALKING the log (each block is self-describing: read its
 *      16-byte header to learn its size, alloc, copy the span back, re-seat the
 *      slot) — proving blocks reconstruct from the log alone;
 *   5. verify all N again (thawed + never-frozen) still carry the right style
 *      and text.
 *
 * Returns 42 on success; a smaller code marks the first failed step. Everything
 * is INLINE in run() (no int-param helpers) per the pcc-or-frame note; the
 * int-parameterised work is all in the asm ops (doc/orvec/orbuf/obj_or).
 */

#include "liborisc.h"
#include "doc.h"

#define N     6
#define K     3       /* freeze the first K blocks (simulating a scrolled view) */
#define BASE  600

static int
run(void)
{
	void *__or doc;
	void *__or blocks;
	void *__or textlog;
	void *__or src;
	void *__or dst;
	void *__or hdr;
	void *__or b;
	int count, i, tl_len, off, tl, sz, live;

	doc = doc_new(2);                    /* start small -> exercise growth */
	if (objor_isnull(doc))
		return 2;

	/* 1. add N blocks: style-id = i, text = the word BASE+i */
	count = 0;
	for (i = 0; i < N; i++) {
		src = objor_alloc(8, OBJ_TAG_DATA,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(src))
			return 3;
		objor_storew(src, BASE + i);
		b = block_new(BLK_PARA, i, src, 0, 4);
		if (objor_isnull(b))
			return 4;
		objor_free(src);
		blocks = orvec_push(doc_blocks(doc), b, count);
		if (objor_isnull(blocks))
			return 5;
		doc_set_blocks(doc, blocks);
		count++;
	}
	if (count != N)
		return 6;

	/* 2. verify all present + correct, and live count == N */
	blocks = doc_blocks(doc);
	dst = objor_alloc(8, OBJ_TAG_DATA, OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (objor_isnull(dst))
		return 7;
	for (i = 0; i < N; i++) {
		b = objor_vget(blocks, i);
		if (objor_isnull(b))
			return 8;
		if (block_style(b) != i)
			return 9;
		block_text(b, dst, 0);
		if (objor_loadw(dst) != BASE + i)
			return 10;
	}
	live = 0;
	for (i = 0; i < N; i++)
		if (!objor_isnull(objor_vget(blocks, i)))
			live++;
	if (live != N)
		return 11;

	/* 3. freeze the first K: append bytes to the log, free, null the slot */
	textlog = doc_textlog(doc);
	tl_len = 0;
	for (i = 0; i < K; i++) {
		b = objor_vget(blocks, i);
		sz = block_bytelen(b);
		textlog = orbuf_append(textlog, tl_len, b, 0, sz);
		if (objor_isnull(textlog))
			return 12;
		tl_len += sz;
		objor_free(b);
		objor_vclear(blocks, i);
	}
	doc_set_textlog(doc, textlog);       /* adopt the (grown) log */

	live = 0;
	for (i = 0; i < N; i++)
		if (!objor_isnull(objor_vget(blocks, i)))
			live++;
	if (live != N - K)                   /* live descriptors dropped */
		return 13;

	/* 4. thaw the first K by walking the self-describing log */
	hdr = objor_alloc(BLOCK_HDR, OBJ_TAG_DATA,
	    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (objor_isnull(hdr))
		return 14;
	off = 0;
	for (i = 0; i < K; i++) {
		orbuf_read(textlog, off, hdr, 0, BLOCK_HDR);
		tl = block_textlen(hdr);         /* size is in the header */
		sz = BLOCK_HDR + tl;
		b = objor_alloc(sz, TAG_BLOCK,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(b))
			return 15;
		orbuf_read(textlog, off, b, 0, sz);
		objor_vset(blocks, b, i);        /* (vec, ref, index) */
		off += sz;
	}

	/* 5. verify all N again — thawed blocks match the never-frozen ones */
	for (i = 0; i < N; i++) {
		b = objor_vget(blocks, i);
		if (objor_isnull(b))
			return 16;
		if (block_style(b) != i)
			return 17;
		block_text(b, dst, 0);
		if (objor_loadw(dst) != BASE + i)
			return 18;
	}

	return 42;
}

int
main(void)
{
	task_init();
	return run();
}
