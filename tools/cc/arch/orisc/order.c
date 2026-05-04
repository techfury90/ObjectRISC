/*
 * Object RISC backend for pcc — order/addressing helpers.
 *
 * Modeled on arch/sparc64/order.c (which is the smallest order.c in
 * the tree). The interesting Object-RISC-specific bit is `notoff`:
 * our load/store instructions take a sign-extended 16-bit immediate
 * offset (CONTRACT.md §5.6), so any constant offset that doesn't fit
 * in [-32768, 32767] must be materialized into a register and added.
 */

#include "pass2.h"

/*
 * Is `off` a legal addressing-mode offset on Object RISC? Our load
 * and store instructions take a 16-bit signed immediate, so the
 * answer is yes iff `off` fits in that range. The acceptable
 * immediate also has to be a multiple of the access width for
 * halfword/word loads, but that constraint is enforced at codegen
 * time, not addressing-mode time.
 */
int
notoff(TWORD t, int r, CONSZ off, char *cp)
{
	return !(off >= -(1L << 15) && off < (1L << 15));
}

/*
 * Turn a UMUL-referenced node into OREG. For Object RISC we accept
 * the simple base+immediate form when the offset is small enough;
 * otherwise we make the caller materialize the address.
 */
void
offstar(NODE *p, int shape)
{
	if (x2debug)
		printf("offstar(%p)\n", p);

	if (p->n_op == PLUS || p->n_op == MINUS) {
		if (p->n_right->n_op == ICON &&
		    getlval(p->n_right) >= -(1L << 15) &&
		    getlval(p->n_right) < (1L << 15)) {
			if (isreg(p->n_left) == 0)
				(void)geninsn(p->n_left, INAREG);
			/* Converted in ormake() */
			return;
		}
	}
	(void)geninsn(p, INAREG);
}

void
myormake(NODE *q)
{
}

int
shumul(NODE *p, int shape)
{
	if (shape & SOREG)
		return SROREG;
	return SRNOPE;
}

int
setbin(NODE *p)
{
	return 0;
}

int
setasg(NODE *p, int cookie)
{
	return 0;
}

int
setuni(NODE *p, int cookie)
{
	return 0;
}

/*
 * Special-case register requirements for individual instruction
 * patterns. Object RISC's MULT and DIV write the HI/LO pair
 * implicitly; if we model HI/LO as architectural registers (we
 * don't, currently — we treat the multiply/divide pair as an
 * opaque write-then-MFHI/MFLO sequence within table.c) this is
 * where their constraint would live.
 */
struct rspecial *
nspecial(struct optab *q)
{
	switch (q->op) {
	case STASG: {
		/* Struct assignment lowers to a memcpy call:
		 *   R4 = dest (we set it in stasg())
		 *   R5 = src  (pcc loads it here per NRIGHT)
		 *   R6 = size (we set it in stasg())
		 * NEVER means the allocator must not put any unrelated
		 * value in that register across this op. */
		static struct rspecial s[] = {
			{ NEVER,  R4 },
			{ NRIGHT, R5 },
			{ NEVER,  R6 },
			{ 0 }
		};
		return s;
	}
	}
	comperr("unknown nspecial %d: %s", q - table, q->cstring);
	return 0;
}

int
setorder(NODE *p)
{
	return 0;
}

/*
 * Registers that may carry live values across a CALL — the standard
 * argument-passing set per Volume VII §2.1. The allocator uses this
 * to avoid choosing one of these for a value that needs to survive
 * the call.
 *
 * We list the integer arg registers (R4..R7), the integer return
 * registers (R2..R3, the latter doubling as the high half of a
 * longlong return), and the four OR arg slots (O1..O4).
 */
int *
livecall(NODE *p)
{
	static int ret[] = {
		R2, R3, R4, R5, R6, R7,
		O1, O2, O3, O4,
		-1
	};
	return ret;
}

int
acceptable(struct optab *op)
{
	return 1;
}
