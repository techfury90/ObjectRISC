/*
 * Object RISC backend for pcc — frontend hooks.
 *
 * Modeled on arch/sparc64/local.c (the smallest local.c). The
 * `clocal` tree-rewrite hook and the varargs builtins are stubbed
 * with `cerror` until they're exercised by an actual test program.
 *
 * The Object-RISC-specific lowering this file will eventually have
 * to do:
 *   - `__or` qualifier handling — recognize the qualifier on
 *     pointer types and route values to the OR file (CLASSC) rather
 *     than the GPR file. Cross-casting between integer pointers and
 *     `__or` pointers traps statically (the capability invariant).
 *   - `__builtin_orisc_send` lowering — multi-operand SEND with
 *     payload from O2..O4 and R4..R7.
 *   - `__builtin_orisc_oref_load`/`store` lowering — turn into
 *     OREFLD/OREFST against an OBJSTORE-backed object.
 *   - Struct return rules per Vol VII §2.6.
 *
 * None of those are done yet.
 */

#include "pass1.h"

#ifndef LANG_CXX
#undef NIL
#define NIL NULL
#define NODE P1ND
#define nfree p1nfree
#define ccopy p1tcopy
#define tfree p1tfree
#endif

NODE *
clocal(NODE *p)
{
	/*
	 * Tree rewriter — pcc calls this on every IR node after type
	 * checking but before code generation. Standard ports use it
	 * to lower target-specific constructs (e.g., extending small
	 * integers, materializing label addresses, calling-convention
	 * fixups).
	 *
	 * For Object RISC we'll need clocal hooks to:
	 *   - identify __or-qualified pointer ops and rewrite them
	 *     into OREFLD/OREFST trees;
	 *   - lower __builtin_orisc_* calls into target-specific tree
	 *     forms that table.c can match;
	 *   - handle integer extension since our loads sign- or
	 *     zero-extend based on the load mnemonic (LB vs LBU etc.).
	 */
	struct symtab *q;
	NODE *r;

	switch (p->n_op) {
	case NAME:
		/*
		 * Auto and param references arrive as NAMEs of named
		 * symbols; lower them to FP-relative struct refs so
		 * the matcher can fold them into OREG addressing for
		 * load/store. Static globals at the top level keep
		 * their NAME form (the assembler resolves the symbol
		 * to an absolute address). Register variables become
		 * REG nodes directly.
		 */
		if ((q = p->n_sp) == NULL)
			return p;
		switch (q->sclass) {
		case PARAM:
		case AUTO:
			r = block(REG, NIL, NIL, PTR+STRTY, 0, 0);
			slval(r, 0);
			r->n_rval = FP;
			p = stref(block(STREF, r, p, 0, 0, 0));
			break;
		case STATIC:
			if (q->slevel == 0)
				break;
			slval(p, 0);
			p->n_sp = q;
			break;
		case REGISTER:
			p->n_op = REG;
			slval(p, 0);
			p->n_rval = q->soffset;
			break;
		}
		break;

	case FORCE:
		/*
		 * Lower `return X;` into the form `R2 = X` so the
		 * return value lands in V0/R2 (RETREG) per Vol VII §2.1.
		 * Without this, pcc allocates the return temp to whatever
		 * it pleases and the caller can't find the value.
		 */
		p->n_op = ASSIGN;
		p->n_right = p->n_left;
		p->n_left = block(REG, NIL, NIL, p->n_type, 0, 0);
		p->n_left->n_rval = RETREG(p->n_type);
		break;
	}
	return p;
}

void
myp2tree(NODE *p)
{
}

int
andable(NODE *p)
{
	/* Most things are addressable. (Auto/static names, etc.) */
	return 1;
}

int
cisreg(TWORD t)
{
	if (t == LONGLONG || t == ULONGLONG)
		return SZINT * 2 / SZCHAR;
	return 1;
}

void
spalloc(NODE *t, NODE *p, OFFSZ off)
{
	cerror("alloca not implemented for orisc");
}

/*
 * Print out a constant initializer in a way the assembler will
 * accept. `fsz` is the number of bits of `p` to emit.
 */
int
ninval(CONSZ off, int fsz, NODE *p)
{
	switch (p->n_type) {
	case FLOAT:
	case DOUBLE:
	case LDOUBLE:
		uerror("FP initializers not supported on Object RISC yet");
		return 0;
	}
	return 0;
}

char *
exname(char *p)
{
	return p ? p : "";
}

TWORD
ctype(TWORD type)
{
	switch (BTYPE(type)) {
	case LONGLONG:
		MODTYPE(type, LONG);
		break;
	case ULONGLONG:
		MODTYPE(type, ULONG);
		break;
	}
	return type;
}

void
calldec(NODE *p, NODE *q)
{
}

void
extdec(struct symtab *q)
{
}

/*
 * Define a zero-initialized variable. Asmorisc has no .comm or .lcomm
 * directive — we lower BSS-style variables to a `.skip N` in the
 * data section after a label.
 */
void
defzero(struct symtab *sp)
{
	int off;
	char *name;

	name = getexname(sp);
	off = tsize(sp->stype, sp->sdf, sp->sap);
	off = (off + SZCHAR - 1) / SZCHAR;

	printf("\t.data\n");
	if (sp->slevel == 0)
		printf("%s:\n", name);
	else
		printf(LABFMT ":\n", sp->soffset);
	printf("\t.skip %d\n", off);
}

int
mypragma(char *str)
{
	return 0;
}

void
fixdef(struct symtab *sp)
{
}

void
pass1_lastchance(struct interpass *ip)
{
}

/*
 * Stubs for the varargs builtins forward-declared in macdefs.h.
 * The implementations follow the standard pcc pattern: stdarg_start
 * captures a pointer to the spill area, va_arg loads the next slot,
 * va_end is a no-op, va_copy is a structure assignment. Object RISC
 * varargs put arguments beyond R4..R7 into the caller's outgoing-arg
 * spill area at 0(sp), 4(sp), ... per Vol VII §2.5.
 */
NODE *
orisc_builtin_stdarg_start(const struct bitable *bt, NODE *a)
{
	uerror("__builtin_va_start not implemented on orisc");
	return bcon(0);
}

NODE *
orisc_builtin_va_arg(const struct bitable *bt, NODE *a)
{
	uerror("__builtin_va_arg not implemented on orisc");
	return bcon(0);
}

NODE *
orisc_builtin_va_end(const struct bitable *bt, NODE *a)
{
	return bcon(0);
}

NODE *
orisc_builtin_va_copy(const struct bitable *bt, NODE *a)
{
	uerror("__builtin_va_copy not implemented on orisc");
	return bcon(0);
}
