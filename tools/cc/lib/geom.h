/*
 * geom.h — plain integer geometry primitives (Point / Size / Rect) and the
 * common operations on them.
 *
 * These are ORDINARY C values, NOT capability objects: a rectangle is data,
 * not an authority, so making it an object (a firmware descriptor per rect)
 * would be absurd. Everything that lays out chrome — the WM, widgets, the
 * document viewport — speaks in these. This is the value-type half of the
 * base-type vocabulary; orvec (orvec.h) is the object-collection half.
 *
 * Structs are passed BY POINTER — the codebase's convention (see
 * `struct task_info`); there is no struct-by-value ABI in use. Coordinates
 * are signed; a rect is x,y (top-left) plus w,h (extent). A rect with w<=0
 * or h<=0 is empty (encloses nothing).
 */

#ifndef GEOM_H
#define GEOM_H

struct Point { int x, y; };
struct Size  { int w, h; };
struct Rect  { int x, y, w, h; };

/* 1 if the rect encloses no pixels (w<=0 or h<=0). */
int rect_empty(const struct Rect *r);

/* 1 if point (px,py) lies inside r. An empty rect contains nothing. */
int rect_contains(const struct Rect *r, int px, int py);

/* 1 if a and b denote the same rectangle. */
int rect_eq(const struct Rect *a, const struct Rect *b);

/* Intersection of a and b into *out. Returns 1 if the overlap is non-empty
 * (out is the overlap), 0 if they do not overlap (out is set empty). out may
 * alias a or b. */
int rect_intersect(const struct Rect *a, const struct Rect *b,
                   struct Rect *out);

/* Smallest rect covering both a and b into *out. An empty operand is
 * ignored (the union of an empty rect and r is r). out may alias a or b. */
void rect_union(const struct Rect *a, const struct Rect *b,
                struct Rect *out);

#endif /* GEOM_H */
