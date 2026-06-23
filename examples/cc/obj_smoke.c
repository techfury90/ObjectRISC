/*
 * obj_smoke.c — end-to-end proof of the handle-based object API (obj.h).
 *
 * Self-contained (no services/peers): allocate an object, round-trip a
 * marker word through it, inspect it, derive a sub-capability, then free
 * everything — all via opaque `obj_t` handles, so no capability ever
 * lives in a C `void *__or` value. main() returns 42 on success; a
 * smaller code says which step failed (see tools/devices/tests/
 * test_obj_smoke.sh).
 */

#include "liborisc.h"
#include "obj.h"

int
main(void)
{
	obj_t h, h2;

	task_init();
	if (obj_init() != 0)
		return 1;

	/* Allocate a byte-addressable object. V lets us free it, C lets us
	 * derive a sub-cap, R|W lets us round-trip a marker word. */
	h = obj_alloc(64, OBJ_TAG_DATA,
	              OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_C | OBJ_CAP_V);
	if (h < 0)
		return 2;

	/* Round-trip a marker word through the handle's object. This is the
	 * core proof: the handle resolves to the right capability across the
	 * obj_storew / obj_loadw helper calls. */
	obj_storew(h, 2748);                 /* 0xABC */
	if (obj_loadw(h) != 2748)
		return 3;

	/* Inspection. */
	if (obj_len(h) != 64)
		return 4;
	if (obj_tag(h) != OBJ_TAG_DATA)
		return 5;
	if (obj_isnull(h))
		return 6;
	if ((obj_caps(h) & OBJ_CAP_W) == 0)
		return 7;

	/* obj_eq across the two-operand load path (same handle is equal). */
	if (!obj_eq(h, h))
		return 8;

	/* Derive a read-only sub-capability into a second handle. */
	h2 = obj_derive(h, OBJ_CAP_R);
	if (h2 < 0)
		return 9;
	if (obj_loadw(h2) != 2748)           /* sub-cap still sees the marker */
		return 10;
	if (!obj_eq(h, h2))                  /* sub-ref names the same object */
		return 11;

	/* Drop the borrowed handle (no free — it shares h's object); then
	 * free the object we own. */
	obj_drop(h2);
	if (obj_free(h) != 0)
		return 12;

	/* After free, the handle is reusable: a fresh alloc reuses slot 0. */
	h = obj_alloc(8, OBJ_TAG_DATA, OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_V);
	if (h != 0)
		return 13;
	obj_free(h);

	return 42;
}
