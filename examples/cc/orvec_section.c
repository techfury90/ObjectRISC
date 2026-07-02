/*
 * orvec_section.c — proves orvec (tools/cc/lib/orvec.{h,c}) against the
 * document model's Section->Blocks shape, and against the freeze-compatible
 * design. A "section" holds a growable array of "block" objects:
 *
 *   1. push NBLOCKS blocks through an orvec that STARTS small (cap 2) so
 *      growth (2 -> 4 -> 8 -> 16) is exercised; each block is a data object
 *      carrying its ordinal;
 *   2. read every block back by index and verify content survived the
 *      reallocs, in order;
 *   3. FREEZE a middle block (free the object, null its slot) and confirm the
 *      slot reads null while neighbours are intact and the length is
 *      unchanged (frozen positions stay addressable);
 *   4. THAW it (store a fresh block) and confirm it reads back.
 *
 * Returns 42 on success; a smaller code marks the first failed step. This is
 * the real-program exercise of the objor_* value API called for by the
 * roadmap (capabilities held as C values across many calls, including the
 * grow/copy loop inside orvec_push).
 */

#include "liborisc.h"
#include "orvec.h"

#define NBLOCKS  10
#define BASEVAL  700

/* Blocks are created INLINE below rather than via a make_block(int) helper: a
 * C function that takes an int parameter while holding an `__or` auto/return
 * miscompiles under pcc-orisc (the int param is homed at the wrong frame
 * offset once the OBJSTORE prologue is present). Doing the alloc+store inline
 * in run() — where the same BASEVAL+i pattern already works, cf.
 * orvec_smoke.c — sidesteps that. */

static int
run(void)
{
	void *__or blocks;
	void *__or nblk;
	void *__or b;
	int len, i;

	blocks = orvec_new(2);              /* start small -> force growth */
	if (objor_isnull(blocks))
		return 2;
	len = 0;

	/* 1. append NBLOCKS blocks (grows 2 -> 4 -> 8 -> 16). The caller owns
	 *    `len`: push places at index len, we adopt the (maybe grown) store
	 *    and increment. */
	for (i = 0; i < NBLOCKS; i++) {
		b = objor_alloc(8, OBJ_TAG_DATA,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(b))
			return 3;
		objor_storew(b, BASEVAL + i);
		nblk = orvec_push(blocks, b, len);
		if (objor_isnull(nblk))
			return 4;
		blocks = nblk;
		len++;
	}
	if (len != NBLOCKS)
		return 5;
	if (orvec_cap(blocks) < NBLOCKS)
		return 6;

	/* 2. every block survived the reallocs, in order */
	for (i = 0; i < NBLOCKS; i++) {
		b = objor_vget(blocks, i);
		if (objor_isnull(b))
			return 7;
		if (objor_loadw(b) != BASEVAL + i)
			return 8;
	}

	/* 3. freeze block 4: free the object, null the slot */
	b = objor_vget(blocks, 4);
	objor_free(b);
	objor_vclear(blocks, 4);
	b = objor_vget(blocks, 4);
	if (!objor_isnull(b))               /* frozen slot must read null */
		return 9;
	b = objor_vget(blocks, 3);          /* neighbours intact */
	if (objor_loadw(b) != BASEVAL + 3)
		return 10;
	b = objor_vget(blocks, 5);
	if (objor_loadw(b) != BASEVAL + 5)
		return 11;
	if (len != NBLOCKS)                 /* freeze does not change length */
		return 12;

	/* 4. thaw: store a fresh block back into the frozen slot */
	b = objor_alloc(8, OBJ_TAG_DATA, OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (objor_isnull(b))
		return 13;
	objor_storew(b, BASEVAL + 4);
	objor_vset(blocks, b, 4);
	b = objor_vget(blocks, 4);
	if (objor_loadw(b) != BASEVAL + 4)
		return 14;

	orvec_free(blocks);
	return 42;
}

int
main(void)
{
	task_init();
	return run();
}
