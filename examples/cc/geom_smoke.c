/*
 * geom_smoke.c — exercises the plain-C geometry ops (geom.h). No objects, so
 * no task_init: it just checks rect_empty / contains / eq / intersect / union
 * on hand-picked rectangles. Returns 42 on success; a smaller code marks the
 * first failed check.
 */

#include "liborisc.h"
#include "geom.h"

int
main(void)
{
	struct Rect a, b, out, r;

	/* empty: zero width/height encloses nothing */
	r.x = 0; r.y = 0; r.w = 0; r.h = 5;
	if (!rect_empty(&r)) return 2;
	r.w = 10;
	if (rect_empty(&r)) return 3;

	/* contains: r = (0,0,10,10) — top-left inclusive, far edges exclusive */
	r.x = 0; r.y = 0; r.w = 10; r.h = 10;
	if (!rect_contains(&r, 0, 0)) return 4;
	if (!rect_contains(&r, 9, 9)) return 5;
	if (rect_contains(&r, 10, 5)) return 6;
	if (rect_contains(&r, 5, 10)) return 7;
	if (rect_contains(&r, -1, 5)) return 8;

	/* eq */
	a.x = 1; a.y = 2; a.w = 3; a.h = 4;
	b.x = 1; b.y = 2; b.w = 3; b.h = 4;
	if (!rect_eq(&a, &b)) return 9;
	b.w = 5;
	if (rect_eq(&a, &b)) return 10;

	/* intersect overlap: a=(0,0,10,10), b=(5,5,10,10) -> (5,5,5,5) */
	a.x = 0; a.y = 0; a.w = 10; a.h = 10;
	b.x = 5; b.y = 5; b.w = 10; b.h = 10;
	if (rect_intersect(&a, &b, &out) != 1) return 11;
	if (out.x != 5 || out.y != 5 || out.w != 5 || out.h != 5) return 12;

	/* intersect disjoint: -> empty, returns 0 */
	b.x = 20; b.y = 20; b.w = 5; b.h = 5;
	if (rect_intersect(&a, &b, &out) != 0) return 13;
	if (!rect_empty(&out)) return 14;

	/* union: a=(0,0,10,10) + b=(20,20,5,5) -> (0,0,25,25) */
	rect_union(&a, &b, &out);
	if (out.x != 0 || out.y != 0 || out.w != 25 || out.h != 25) return 15;

	/* union ignores an empty operand */
	r.x = 0; r.y = 0; r.w = 0; r.h = 0;
	rect_union(&a, &r, &out);
	if (!rect_eq(&out, &a)) return 16;

	return 42;
}
