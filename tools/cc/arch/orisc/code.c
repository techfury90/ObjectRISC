/*
 * Object RISC backend for pcc — function entry/exit, segment
 * directives, and call lowering.
 *
 * Modeled on the top-level MIPS code.c (Enoksson/Olsson 2005),
 * cut down to the minimum coverage of integer and OR arguments —
 * struct passing, struct return, varargs, and double-precision
 * arguments are noted with `cerror` until they're needed by a real
 * test program. The first end-to-end milestone (compile a hello
 * world) doesn't exercise any of those.
 */

#include <assert.h>
#include "pass1.h"

#ifndef LANG_CXX
#undef NIL
#define NIL NULL
#define NODE P1ND
#define nfree p1nfree
#define ccopy p1tcopy
#define tfree p1tfree
#endif

extern void flush_charbuf(void);	/* in local.c */

/*
 * Print the assembler segment directive corresponding to a pcc
 * section enum. Asmorisc supports `.text`, `.data`, and the related
 * directives in CONTRACT.md §4.2 — no .rodata / PIC / TLS / CTORS
 * variants yet.
 */
void
setseg(int seg, char *name)
{
	/* Flush any pending .ascii accumulation before changing
	 * sections — the chars must close out under their original
	 * section directive. */
	flush_charbuf();

	switch (seg) {
	case PROG: name = ".text"; break;
	case DATA:
	case LDATA:
	case STRNG:
	case RDATA: name = ".data"; break;
	case UDATA: break;
	case PICLDATA:
	case PICDATA:
	case PICRDATA:
	case TLSDATA:
	case TLSUDATA:
	case CTORS:
	case DTORS:
		uerror("section type not supported on Object RISC");
		return;
	case NMSEG:
		uerror("named sections not supported on Object RISC");
		return;
	}
	printf("\t%s\n", name);
}

/*
 * Define a label and (for top-level symbols) the entry point if this
 * is `main`. Asmorisc has no .globl / .type / .size directives, so
 * we just emit `name:` and `.entry main` as needed.
 */
void
defloc(struct symtab *sp)
{
	char *n;

	/* Flush any pending .ascii accumulation before starting a new
	 * symbol — char inits for the previous symbol must close out
	 * with their own .ascii directive before we emit the label. */
	flush_charbuf();

	if (ISFTN(sp->stype))
		return; /* function labels emitted by prologue */

	n = getexname(sp);
	if (sp->slevel == 0) {
		if (sp->sclass == EXTDEF && strcmp(n, "main") == 0)
			printf("\t.entry main\n");
		printf("%s:\n", n);
	} else {
		printf(LABFMT ":\n", sp->soffset);
	}
}

void
defalign(int n)
{
	/* Convert n (in bits) to an asmorisc .align shift count.
	 * .align K means "round up to a multiple of 2^K bytes". */
	int bytes = n / SZCHAR;
	int k = ispow2(bytes);
	if (k == -1)
		cerror("defalign: %d not a power of two", bytes);
	if (k > 0)
		printf("\t.align %d\n", k);
}

static int rvnr;	/* register holding hidden struct-return arg */

/*
 * End-of-function code. For a function that returns a struct, we
 * have to copy the return-buffer pointer (saved earlier in `rvnr`)
 * back into R2 so the caller can find the result. Plain integer /
 * pointer / OR returns are already in the right registers when this
 * runs.
 */
void
efcode(void)
{
	if (cftnsp->stype != STRTY+FTN && cftnsp->stype != UNIONTY+FTN)
		return;
	cerror("struct return not yet implemented for orisc");
}

/*
 * Beginning-of-function code. Move integer args from R4..R7 into
 * temp registers (so the allocator can decide where to keep them);
 * spill-area args (#5 and beyond) are accessed in place via FP.
 *
 * OR args arrive in O1..O4 and stay there until the body uses them
 * — they're always in OR file regs, never on the stack (Vol VII §2.5).
 */
void
bfcode(struct symtab **sp, int cnt)
{
	struct symtab *sym;
	NODE *p, *q;
	int i, gpr;

	/* Hidden struct-return arg occupies R4 if present. */
	if (cftnsp->stype == STRTY+FTN || cftnsp->stype == UNIONTY+FTN) {
		cerror("struct return not yet implemented for orisc");
	}

	gpr = R4;
	{
		int opr = O1;

		for (i = 0; i < cnt; i++) {
			sym = sp[i];

			switch (sym->stype) {
			case STRTY+FTN:
			case UNIONTY+FTN:
				cerror("struct argument not yet implemented for orisc");
				break;

			case LONGLONG:
			case ULONGLONG:
				cerror("longlong argument not yet implemented for orisc");
				break;

			case DOUBLE:
			case LDOUBLE:
			case FLOAT:
				cerror("FP argument not yet implemented for orisc");
				break;

			default:
				if (ISOREF(sym->squal) && opr <= O4) {
					/* `__or` parameter — arg arrived in
					 * O[opr]. Callee-side binding is not
					 * yet implemented: pcc's tempnode is
					 * class-blind and produces a CLASSA
					 * temp, which then fails to coalesce
					 * with the CLASSC OR source. Until we
					 * either add an OREF basic type or
					 * teach tempnode about qualifiers,
					 * `__or` parameters can only be used
					 * via explicit `register __or T *p
					 * __asm__("oN")` bindings inside the
					 * body. The caller-side calling
					 * convention (moveargs → O1..O4) does
					 * work, so external assembly callees
					 * see the right value in O[opr]. */
					(void)opr;
				} else if (!ISOREF(sym->squal) && gpr <= R7) {
					q = block(REG, NIL, NIL, sym->stype,
					    sym->sdf, sym->sap);
					q->n_rval = gpr++;
					p = tempnode(0, sym->stype, sym->sdf,
					    sym->sap);
					sym->soffset = regno(p);
					sym->sflags |= STNODE;
					p = buildtree(ASSIGN, p, q);
					ecomp(p);
				} else {
					/* Stack arg: leave in place, accessed via FP. */
				}
			}
		}
	}
}

void
bjobcode(void)
{
	/*
	 * astypnames[] — assembler directive used for emitting each
	 * type's worth of bytes when a static initializer asks for one.
	 * Asmorisc uses .byte / .half / .word; LONGLONG is two .word.
	 */
	astypnames[USHORT] = astypnames[SHORT] = "\t.half";
	astypnames[INT] = astypnames[UNSIGNED] = "\t.word";
	astypnames[LONG] = astypnames[ULONG] = "\t.word";
	/* longlong has no single asmorisc directive — emitted as 2x .word
	 * by the back end as needed. */
}

void
ejobcode(int flag)
{
}

/*
 * Lower a CALL's argument list. Walks the right side of `p` (a CM-
 * tree of arguments) and assigns each argument to its destination
 * register or stack slot. The first four integer / pointer args go
 * to R4..R7; the first four OR args go to O1..O4; everything else
 * spills into the outgoing-arg area at the top of `sp` (per Vol VII
 * §2.2).
 */
static NODE *
moveargs(NODE *p, int *gpr, int *opr, int *stacksize)
{
	NODE *r, *q;

	if (p->n_op == CM) {
		p->n_left = moveargs(p->n_left, gpr, opr, stacksize);
		r = p->n_right;
	} else {
		r = p;
	}

	if (r->n_op == STARG) {
		cerror("struct argument not yet implemented for orisc");
		return p;
	}

	/*
	 * `__or`-qualified arguments route to the OR file (O1..O4)
	 * per Vol VII §2.1; everything else uses the integer arg
	 * regs (R4..R7) and spills to the outgoing-arg area beyond
	 * that.
	 */
	if (ISOREF(r->n_qual) && *opr <= O4) {
		q = block(REG, NIL, NIL, r->n_type, r->n_df, r->n_ap);
		q->n_qual = OREF;
		q->n_rval = (*opr)++;
		r = buildtree(ASSIGN, q, r);
	} else if (!ISOREF(r->n_qual) && *gpr <= R7) {
		q = block(REG, NIL, NIL, r->n_type, r->n_df, r->n_ap);
		q->n_rval = (*gpr)++;
		r = buildtree(ASSIGN, q, r);
	} else {
		/* Stack overflow arg — bump stacksize, remember offset.
		 * tsize returns bits; convert to bytes. SETOFF aligns
		 * up to a multiple of the type's natural width. OR args
		 * past O4 currently fall here, but spilling an OR to
		 * byte memory violates the capability invariant; cerror
		 * if we hit that case. */
		int sz;
		if (ISOREF(r->n_qual))
			cerror("more than four __or arguments — needs OBJSTORE spill");
		sz = tsize(r->n_type, r->n_df, r->n_ap) / SZCHAR;
		r = block(FUNARG, r, NIL, r->n_type, r->n_df, r->n_ap);
		SETOFF(*stacksize, sz);
		r->n_rval = *stacksize;
		*stacksize += sz;
	}

	if (p->n_op == CM) {
		p->n_right = r;
		return p;
	}
	return r;
}

NODE *
funcode(NODE *p)
{
	int gpr = R4, opr = O1, stacksize = 0;

	p->n_right = moveargs(p->n_right, &gpr, &opr, &stacksize);
	/*
	 * Caller's outgoing-arg-spill area is allocated in the
	 * prologue — pcc tracks the maximum across all calls in the
	 * function and reserves it once. (See local2.c::prologue.)
	 */
	(void)stacksize;
	return p;
}

void
fldty(struct symtab *p)
{
}

int
mygenswitch(int num, TWORD type, struct swents **p, int n)
{
	return 0;
}

NODE *
builtin_cfa(const struct bitable *bt, NODE *a)
{
	uerror("__builtin_dwarf_cfa not implemented on orisc");
	return bcon(0);
}

NODE *
builtin_frame_address(const struct bitable *bt, NODE *a)
{
	uerror("__builtin_frame_address not implemented on orisc");
	return bcon(0);
}

NODE *
builtin_return_address(const struct bitable *bt, NODE *a)
{
	uerror("__builtin_return_address not implemented on orisc");
	return bcon(0);
}
