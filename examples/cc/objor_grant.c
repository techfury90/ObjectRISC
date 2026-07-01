/*
 * objor_grant.c — Phase-3 messaging proof: a capability GRANT sent across a
 * task boundary with the `void *__or` value object API (obj_or.h).
 *
 * objor_delegate.c proves delegation within one task (pass a derived cap as
 * a value param).  This proves it BETWEEN two tasks over the messaging API —
 * the object-console shell's "a pipe is a capability grant" and the document
 * architecture's cross-process block sharing both reduce to exactly this:
 * one task derives a RESTRICTED capability and SENDs it; another RECEIVEs it
 * as a value and uses it under the narrowed rights.
 *
 * Flow (single CPU, cooperative scheduling; the parent blocks on recv, which
 * lets the child run, and vice versa):
 *
 *   parent                                  child (task_spawn'd)
 *   ------                                  --------------------
 *   alloc rendezvous mailbox + queue
 *   stash it in O7  ----spawn---->          adopt rendezvous from O7
 *   recv_cap(rendezvous)  [blocks]          alloc own mailbox + queue
 *                                           derive R|S send-cap of it
 *                          <--send_cap--    send_cap(rendezvous, myaddr)
 *   (wakes) derive R grant of a resource    recv_cap(mymbox)  [blocks]
 *   send_cap(child addr, grant)  --send-->  (wakes) got the grant
 *   task_wait(child)  [blocks]              check: reads marker, has no W
 *                          <--exit 42----   task_exit(42)
 *   return the child's code
 *
 * Exercises objor_queue_attach / objor_send_cap / objor_recv_cap (the
 * messaging half of the value API, unexercised by any running program until
 * now) plus objor_stash_o7 / objor_adopt_o7 (the O-register<->value bridge).
 *
 * Returns 42 on success; a smaller code marks the first failed check
 * (parent-side 2..7, child-side 12..18; see test_objor_grant.sh).
 *
 * Structure rules (obj_or.h): task_init() in each task's entry; every `__or`
 * auto/param lives in a helper called after it.
 */

#include "liborisc.h"
#include "obj_or.h"

#define MARKER		0x51EED		/* the word the grant must read back */
#define OP_ANNOUNCE	1		/* child -> parent: "here is my address" */

/* Full-rights mailbox: R|W to hold the object, S so peers can SEND to it,
 * V to own it, C to derive send-only sub-caps. */
#define MBOX_CAPS	(OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_S | OBJ_CAP_V | OBJ_CAP_C)

/*
 * Child body.  Adopts the rendezvous cap the parent parked in O7, stands up
 * its own reply mailbox, announces a send-only cap of it to the parent, then
 * blocks for the grant and verifies it is a usable read-only view.
 */
static int
child_work(void)
{
	int word;

	void *__or rendez = objor_adopt_o7();
	if (objor_isnull(rendez))
		return 12;

	/* My own mailbox + queue; the grant will be delivered here. */
	void *__or mymbox = objor_alloc(16, OBJ_TAG_SERVICE, MBOX_CAPS);
	if (objor_isnull(mymbox))
		return 13;
	if (objor_queue_attach(mymbox, 4) != 0)
		return 14;

	/* Hand the parent a send-only (R|S) cap of my mailbox as the payload. */
	void *__or myaddr = objor_derive(mymbox, OBJ_CAP_R | OBJ_CAP_S);
	if (objor_isnull(myaddr))
		return 15;
	objor_send_cap(rendez, myaddr, OP_ANNOUNCE, 0, 0, 0);
	objor_drop(myaddr);

	/* Block for the grant; it rides O2 and comes back as a value. */
	void *__or grant = objor_recv_cap(mymbox, &word);
	if (objor_isnull(grant))
		return 16;
	if (objor_caps(grant) & OBJ_CAP_W)	/* granted R-only: no W */
		return 17;
	if (objor_loadw(grant) != MARKER)	/* and it really reads the marker */
		return 18;

	return 42;
}

static void
child_entry(int arg)
{
	task_init();
	task_exit(child_work());
}

/*
 * Parent body.  Publishes a rendezvous mailbox to the child via O7, waits
 * for the child's address, then mints a resource and SENDs the child a
 * read-only grant of it.
 */
static int
parent_work(void)
{
	int word;

	/* Rendezvous mailbox + queue; parked in O7 for the child to inherit. */
	void *__or rendez = objor_alloc(16, OBJ_TAG_SERVICE, MBOX_CAPS);
	if (objor_isnull(rendez))
		return 2;
	if (objor_queue_attach(rendez, 4) != 0)
		return 3;
	objor_stash_o7(rendez);

	task_t kid = task_spawn(child_entry, 0);
	if (kid < 0)
		return 4;

	/* Wait for the child to announce its reply address (blocks -> child
	 * runs and sends). The announced send-cap rides O2 -> returned value. */
	void *__or child_addr = objor_recv_cap(rendez, &word);
	if (objor_isnull(child_addr))
		return 5;

	/* Mint a resource, stamp it, derive a read-only grant. */
	void *__or res = objor_alloc(8, OBJ_TAG_DATA,
	    OBJ_CAP_R | OBJ_CAP_W | OBJ_CAP_C | OBJ_CAP_V);
	if (objor_isnull(res))
		return 6;
	objor_storew(res, MARKER);
	void *__or grant = objor_derive(res, OBJ_CAP_R);
	if (objor_isnull(grant))
		return 7;

	/* Grant it to the child (blocks it awake), then collect its verdict. */
	objor_send_cap(child_addr, grant, 0, 0, 0, 0);

	int rc = task_wait(kid);		/* child's exit code = 42 on success */
	task_free(kid);
	return rc;
}

int
main(void)
{
	task_init();
	return parent_work();
}
