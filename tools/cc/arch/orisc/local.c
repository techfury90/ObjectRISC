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

/*
 * Build the lvalue for an `__or` auto/param homed at byte offset `off`
 * in the per-frame OBJSTORE (see macdefs.h). The home is reached two
 * levels deep -- load the per-frame spill objstore's ref from the O12
 * task-table anchor slot, then OREFLD/OREFST the capability at `off`
 * within it -- emitted as a single OREG leaf whose table.c OREFLD/OREFST
 * patterns expand the two-level sequence via zzzcode ('H' load / 'I'
 * store). The OREG carries the home byte offset in its lval; its base
 * register field is R0 (cosmetic: the zzzcode hard-codes the O12 anchor
 * deref and never consults it, and R0 avoids the allocator seeing a
 * live precolored CLASSC value at every home access). The OREFTY type
 * keeps it clear of the byte lw/sw patterns.
 */
static NODE *
oref_home(int off)
{
	NODE *p = block(OREG, NIL, NIL, OREFTY, 0, 0);

	p->n_rval = R0;
	slval(p, off);
	p->n_name = "";
	return p;
}

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
			/*
			 * An `__or` auto/param lives in the per-frame OBJSTORE
			 * (its soffset is a byte offset there), not on the byte
			 * stack — replace the NAME with the two-level OBJSTORE
			 * access. bfcode/oalloc assign the home; the prologue
			 * allocates the spill objstore and homes incoming params.
			 */
			if (ISOREFT(q->stype)) {
				int off = q->soffset;
				nfree(p);
				p = oref_home(off);
				break;
			}
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

	case ASSIGN:
		/*
		 * Storing a capability *call result* straight into an `__or`
		 * home (`void *__or x = f(...);`) used to be rejected here: the
		 * OREFST needs a base scratch O-reg (the O12 anchor deref), and
		 * with only four allocatable ORs the store's scratch interfered
		 * with all of O1..O4 across the call and spilled — impossible for
		 * a capability. That interference was SPURIOUS: it came from
		 * pcc's coarse per-block call liveness marking every OR argument
		 * register live across every call, even those the call does not
		 * pass. macdefs.h's PRUNE_CALLLIVE now clears that false OR
		 * liveness (regs.c::insnwalk), so the store's scratch no longer
		 * collides with the call clobbers and this case lowers correctly.
		 * The guard is therefore removed. (Genuinely holding more than a
		 * few `__or` VALUES live across a call at once can still exceed
		 * the 4-wide OR class and spill — that needs the callee-saved
		 * CLASSC register work, out of scope here — but the common
		 * store-a-call-result and value-across-one-call forms now work.)
		 */
		break;

	case FORCE:
		/*
		 * Lower `return X;` into the form `<retreg> = X`. The
		 * return register is V0/R2 (RETREG) for ordinary
		 * scalars, R2:R3 for longlong, and O1 for `__or`-
		 * qualified return types per Vol VII §2.1.
		 *
		 * An object-reference return type carries OBIT in the
		 * type word, which rides n_type and is preserved by the
		 * cftnod tempnode — so the return value's type tells us
		 * directly whether it goes in O1, with no reliance on the
		 * (creator-zeroed) qualifier or on inspecting cftnsp.
		 */
		p->n_op = ASSIGN;
		p->n_right = p->n_left;
		p->n_left = block(REG, NIL, NIL, p->n_type, 0, 0);
		p->n_left->n_qual = p->n_qual;
		if (ISOREFT(p->n_type)) {
			p->n_left->n_rval = O1;
		} else {
			p->n_left->n_rval = RETREG(p->n_type);
		}
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
 * Char-init coalescing: pcc invokes ninval once per element when
 * initializing a char array (including from a string literal). The
 * naive output is one `.byte N` per character — readable but ugly.
 * We buffer consecutive char inits at consecutive offsets and emit
 * them as a single `.ascii "..."` directive when the run ends.
 *
 * Run end is detected three ways:
 *   - ninval is called for a non-char or a non-consecutive offset
 *     (handled inline below)
 *   - defloc is called for a new label (flush_charbuf called from
 *     code.c::defloc before the new label's code)
 *   - End of compilation (we don't currently hook ejobcode for this
 *     because pcc emits a final newline-terminated label anyway)
 */
#define CHARBUF_CAP 80
static char charbuf[CHARBUF_CAP];
static int  charbuf_len = 0;
static CONSZ charbuf_next_off = 0;	/* next-expected offset */

static void
charbuf_emit_one(int c)
{
	switch (c) {
	case '"':  fputs("\\\"", stdout); break;
	case '\\': fputs("\\\\", stdout); break;
	case '\n': fputs("\\n",  stdout); break;
	case '\t': fputs("\\t",  stdout); break;
	case '\r': fputs("\\r",  stdout); break;
	case '\0': fputs("\\0",  stdout); break;
	default:
		if (c >= 0x20 && c < 0x7f)
			putchar(c);
		else
			printf("\\%o", c & 0xff);
	}
}

void
flush_charbuf(void)
{
	int i;
	if (charbuf_len == 0)
		return;
	printf("\t.ascii \"");
	for (i = 0; i < charbuf_len; i++)
		charbuf_emit_one((unsigned char)charbuf[i]);
	printf("\"\n");
	charbuf_len = 0;
}

/*
 * Print out a constant initializer in a way the assembler will
 * accept. `fsz` is the number of bits of `p` to emit.
 */
int
ninval(CONSZ off, int fsz, NODE *p)
{
	switch (p->n_type) {
	case CHAR:
	case UCHAR:
		/* Coalesce consecutive char inits into one .ascii. pcc
		 * emits inits sequentially in call order; the `off`
		 * parameter is set per-call but unused by the standard
		 * sequential path (often 0). We don't validate against
		 * it — just accumulate and flush on the next non-char
		 * init or label.
		 */
		if (p->n_op == ICON && p->n_sp == NULL) {
			if (charbuf_len >= CHARBUF_CAP)
				flush_charbuf();
			charbuf[charbuf_len++] = (char)glval(p);
			return 1;
		}
		break;

	case FLOAT:
	case DOUBLE:
	case LDOUBLE:
		uerror("FP initializers not supported on Object RISC yet");
		return 0;
	}
	flush_charbuf();
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
 *
 * Emit a leading `.align` based on the symbol's type alignment.
 * pcc's uninitialized-global path (pftn.c::nidcl2) doesn't go
 * through locctr — it calls defzero directly without first calling
 * defalign — so two consecutive globals of mismatched alignment
 * (e.g. `char Ch_Glob;` followed by `int Arr_Glob[50];`) would
 * leave the int array on an odd offset and word loads would trap.
 * Initialized globals get their alignment from the locctr → defalign
 * path inside init.c.
 */
void
defzero(struct symtab *sp)
{
	int off;
	char *name;
	int al, bytes, k;

	name = getexname(sp);
	off = tsize(sp->stype, sp->sdf, sp->sap);
	off = (off + SZCHAR - 1) / SZCHAR;

	printf("\t.data\n");
	al = talign(sp->stype, sp->sap);   /* in bits */
	if (al > SZCHAR) {
		bytes = al / SZCHAR;
		k = ispow2(bytes);
		if (k > 0)
			printf("\t.align %d\n", k);
	}
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
