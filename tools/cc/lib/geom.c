/*
 * geom.c — integer geometry operations (see geom.h). Pure value arithmetic;
 * no objects, no firmware calls. Structs are read/written through pointers
 * (no struct-by-value); field-wise copies avoid relying on a struct-copy ABI.
 */

#include "geom.h"

int
rect_empty(const struct Rect *r)
{
	return r->w <= 0 || r->h <= 0;
}

int
rect_contains(const struct Rect *r, int px, int py)
{
	if (rect_empty(r))
		return 0;
	return px >= r->x && px < r->x + r->w &&
	       py >= r->y && py < r->y + r->h;
}

int
rect_eq(const struct Rect *a, const struct Rect *b)
{
	return a->x == b->x && a->y == b->y &&
	       a->w == b->w && a->h == b->h;
}

static void
set_empty(struct Rect *out)
{
	out->x = 0;
	out->y = 0;
	out->w = 0;
	out->h = 0;
}

int
rect_intersect(const struct Rect *a, const struct Rect *b, struct Rect *out)
{
	int x0, y0, x1, y1;

	if (rect_empty(a) || rect_empty(b)) {
		set_empty(out);
		return 0;
	}
	x0 = a->x > b->x ? a->x : b->x;
	y0 = a->y > b->y ? a->y : b->y;
	x1 = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
	y1 = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
	if (x1 <= x0 || y1 <= y0) {
		set_empty(out);
		return 0;
	}
	out->x = x0;
	out->y = y0;
	out->w = x1 - x0;
	out->h = y1 - y0;
	return 1;
}

void
rect_union(const struct Rect *a, const struct Rect *b, struct Rect *out)
{
	int x0, y0, x1, y1;

	if (rect_empty(a)) {
		out->x = b->x; out->y = b->y; out->w = b->w; out->h = b->h;
		return;
	}
	if (rect_empty(b)) {
		out->x = a->x; out->y = a->y; out->w = a->w; out->h = a->h;
		return;
	}
	x0 = a->x < b->x ? a->x : b->x;
	y0 = a->y < b->y ? a->y : b->y;
	x1 = (a->x + a->w) > (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
	y1 = (a->y + a->h) > (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);
	out->x = x0;
	out->y = y0;
	out->w = x1 - x0;
	out->h = y1 - y0;
}
