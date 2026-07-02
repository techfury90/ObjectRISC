/*
 * orbuf_smoke.c — proves the growable byte buffer orbuf (tools/cc/lib/orbuf).
 *
 * Appends N 8-byte spans (each a distinct word) into a buffer that STARTS at
 * one word so growth (8 -> 16 -> 32 -> 64) is exercised; the source objects
 * are freed right after each append to prove orbuf keeps its OWN copy of the
 * bytes. Then reads each span back out (into a scratch object) and verifies
 * the word survived, in order and at the right offset.
 *
 * Returns 42 on success; a smaller code marks the first failed step. Block/
 * source creation is INLINE (no int-param helper) — see pcc-or-frame note.
 */

#include "liborisc.h"
#include "orbuf.h"

#define N     6
#define BASE  500

static int
run(void)
{
	void *__or buf;
	void *__or nbuf;
	void *__or src;
	void *__or dst;
	int len, i;

	buf = orbuf_new(8);                 /* one word -> force growth */
	if (objor_isnull(buf))
		return 2;
	len = 0;

	/* Append N words; free each source immediately (orbuf copied it). */
	for (i = 0; i < N; i++) {
		src = objor_alloc(8, OBJ_TAG_DATA,
		    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
		if (objor_isnull(src))
			return 3;
		objor_storew(src, BASE + i);
		nbuf = orbuf_append(buf, len, src, 0, 8);
		if (objor_isnull(nbuf))
			return 4;
		buf = nbuf;
		len += 8;
		objor_free(src);
	}
	if (len != N * 8)
		return 5;
	if (orbuf_cap(buf) < N * 8)
		return 6;

	/* Read each 8-byte span back and verify the word. */
	dst = objor_alloc(8, OBJ_TAG_DATA, OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (objor_isnull(dst))
		return 7;
	for (i = 0; i < N; i++) {
		orbuf_read(buf, i * 8, dst, 0, 8);
		if (objor_loadw(dst) != BASE + i)
			return 8;
	}

	orbuf_free(buf);
	objor_free(dst);
	return 42;
}

int
main(void)
{
	task_init();
	return run();
}
