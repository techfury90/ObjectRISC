/*
 * Object RISC backend for pcc — assembly emission, register names,
 * function prologue/epilogue.
 *
 * This file is the bridge between pcc's IR and the asmorisc syntax
 * documented in CONTRACT.md §4. Modeled on the top-level MIPS
 * local2.c (Enoksson/Olsson 2005) — same shape, but emits Object
 * RISC mnemonics and register names, and uses the simpler Vol VII
 * §2.3 prologue/epilogue (no FP unless forced, no .frame/.ent/.cpload
 * pseudo-ops).
 *
 * Many helpers below are stubbed with `comperr` until the
 * corresponding code path is exercised by a real test. The goal of
 * this initial cut is to nail down the surface area, not to compile
 * arbitrary C — table.c is empty, so the only thing this file
 * actually drives end-to-end so far is the prologue/epilogue.
 */

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "pass1.h"
#include "pass2.h"

int bigendian = 1;	/* Object RISC is big-endian per Vol I */

void
deflab(int label)
{
	printf(LABFMT ":\n", label);
}

static int regoff[32];
static TWORD ftype;

/*
 * Calculate stack frame size — sum of automatic-storage area plus
 * one slot per callee-preserved GPR that this function actually
 * uses. Returns the total in bytes, rounded up to 8-byte alignment
 * per Vol VII §2.2.
 */
static int
offcalc(struct interpass_prolog *ipp)
{
	int i, j, addto;

	addto = p2maxautooff;

	for (i = p2env.p_regs[0], j = 0; i; i >>= 1, j++) {
		if (i & 1) {
			addto += SZINT / SZCHAR;
			regoff[j] = addto;
		}
	}

	addto = (addto + 7) & ~7;
	return addto;
}

/*
 * Emit the function prologue per Vol VII §2.3:
 *
 *     funcname:
 *         addiu sp, sp, -F
 *         sw    r31, F-4(sp)         ; save link register
 *         sw    rN,  F-K(sp)         ; per saved callee-preserved
 *
 * No FP convention here — we use SP-relative addressing throughout.
 * Asmorisc has no .globl directive; visibility is implicit, all
 * top-level labels resolve.
 */
void
prologue(struct interpass_prolog *ipp)
{
	int addto;
	int i, j;

	ftype = ipp->ipp_type;
	printf("%s:\n", ipp->ipp_name);

	addto = offcalc(ipp);

	if (addto) {
		printf("\taddiu sp, sp, -%d\n", addto);
		printf("\tsw r31, %d(sp)\n", addto - 4);
		for (i = p2env.p_regs[0], j = 0; i; i >>= 1, j++)
			if (i & 1)
				printf("\tsw r%d, %d(sp) ; save callee-preserved\n",
				    j, addto - regoff[j]);
	}
}

/*
 * Emit the function epilogue per Vol VII §2.3:
 *
 *         lw    rN,  F-K(sp)         ; restore each callee-preserved
 *         lw    r31, F-4(sp)
 *         jr    r31
 *         addiu sp, sp, F            ; (delay slot) deallocate frame
 */
void
eoftn(struct interpass_prolog *ipp)
{
	int addto, i, j;

	addto = offcalc(ipp);

	if (ipp->ipp_ip.ip_lbl == 0)
		return;	/* no code generated */

	if (addto) {
		for (i = p2env.p_regs[0], j = 0; i; i >>= 1, j++)
			if (i & 1)
				printf("\tlw r%d, %d(sp)\n",
				    j, addto - regoff[j]);
		printf("\tlw r31, %d(sp)\n", addto - 4);
		printf("\tjr r31\n");
		printf("\taddiu sp, sp, %d\n", addto);
	} else {
		printf("\tjr r31\n");
		printf("\tnop\n");
	}
}

/*
 * Mnemonic for a binary op — used by the pattern-table machinery
 * when it needs to emit "add" vs "sub" etc. without per-op patterns.
 * Object RISC matches MIPS naming for the integer ALU.
 */
void
hopcode(int f, int o)
{
	char *str;

	switch (o) {
	case PLUS:	str = "addu";	break;
	case MINUS:	str = "subu";	break;
	case AND:	str = "and";	break;
	case OR:	str = "or";	break;
	case ER:	str = "xor";	break;
	default:
		comperr("hopcode: %d", o);
		str = 0;
	}
	printf("%s%c", str, f);
}

/*
 * Register names. Order must match macdefs.h exactly:
 *   0..31  = R0..R31  (CLASSA)
 *   32..47 = R0R1..R30R31  (CLASSB pairs — hi part stored first
 *           per big-endian; the asm syntax is the low-numbered
 *           half's name, with the high half implied by adjacency)
 *   48..63 = O0..O15  (CLASSC)
 *
 * For pair registers we use the encoding "rN!rN+1!" (the trailing
 * `!` chars are placeholders that pcc fills with spaces to keep
 * column alignment in error messages); the assembler never sees
 * these strings directly — code emission references the named
 * halves explicitly.
 */
char *rnames[] = {
	"r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
	"r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
	"r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
	"r24", "r25", "r26", "r27", "r28", "sp",  "fp",  "ra",

	/* CLASSB pair register names — only the allocatable pairs are
	 * meaningful; the reserved ones (R0R1, R28R29, R30R31) are
	 * present for index alignment and never emitted. */
	"r0!!r1!!",
	"r2!!r3!!",   "r4!!r5!!",   "r6!!r7!!",   "r8!!r9!!",
	"r10!r11!",   "r12!r13!",   "r14!r15!",   "r16!r17!",
	"r18!r19!",   "r20!r21!",   "r22!r23!",   "r24!r25!",
	"r26!r27!",   "r28!sp!!",   "fp!!ra!!",

	/* CLASSC: object registers */
	"o0",  "o1",  "o2",  "o3",  "o4",  "o5",  "o6",  "o7",
	"o8",  "o9",  "o10", "o11", "o12", "o13", "o14", "o15",
};

int
tlen(NODE *p)
{
	switch (p->n_type) {
	case CHAR:
	case UCHAR:
		return 1;
	case SHORT:
	case USHORT:
		return SZSHORT / SZCHAR;
	case DOUBLE:
	case LDOUBLE:
	case LONGLONG:
	case ULONGLONG:
		return SZLONGLONG / SZCHAR;
	default:
		return SZINT / SZCHAR;
	}
}

/* These all need real implementations once table.c lands and we have
 * actual instruction patterns to flesh out. For now: stubs that
 * scream loudly, so we don't silently emit broken code. */

void
starg(NODE *p)
{
	comperr("starg not implemented for orisc");
}

void
stasg(NODE *p)
{
	comperr("stasg not implemented for orisc");
}

int
shiftop(NODE *p)
{
	comperr("shiftop not implemented for orisc");
	return 0;
}

void
zzzcode(NODE *p, int c)
{
	comperr("zzzcode '%c' not implemented for orisc", c);
}

int
rewfld(NODE *p)
{
	return 1;
}

int
fldexpand(NODE *p, int cookie, char **cp)
{
	/* Object RISC has no native bit-field instructions; field
	 * expansion happens through the standard pcc shift-and-mask
	 * lowering. */
	return 0;
}

int
flshape(NODE *p)
{
	return SRREG;
}

int
shtemp(NODE *p)
{
	return 0;
}

void
adrcon(CONSZ val)
{
	printf(CONFMT, val);
}

void
conput(FILE *fp, NODE *p)
{
	switch (p->n_op) {
	case ICON:
		if (p->n_name[0] != '\0')
			fprintf(fp, "%s", p->n_name);
		else
			fprintf(fp, CONFMT, p->n_lval);
		return;
	default:
		comperr("conput: bad op %d", p->n_op);
	}
}

void
insput(NODE *p)
{
	comperr("insput");
}

void
upput(NODE *p, int size)
{
	comperr("upput not implemented for orisc");
}

/*
 * Print the address of an operand. The pcc convention is that the
 * caller has already issued whatever mnemonic prefix it wanted; this
 * just emits the operand text — register name, literal constant,
 * label reference, or an `OFFSET(REG)` form.
 */
void
adrput(FILE *io, NODE *p)
{
	int r;

	if (p->n_op == FLD) {
		comperr("adrput: FLD not implemented for orisc");
		return;
	}
	switch (p->n_op) {
	case NAME:
		if (p->n_name[0] != '\0')
			fputs(p->n_name, io);
		if (p->n_lval != 0)
			fprintf(io, "+" CONFMT, p->n_lval);
		return;

	case OREG:
		r = p->n_rval;
		if (p->n_lval)
			fprintf(io, CONFMT, p->n_lval);
		fprintf(io, "(%s)", rnames[r]);
		return;

	case ICON:
		if (p->n_name[0] != '\0') {
			fputs(p->n_name, io);
			if (p->n_lval != 0)
				fprintf(io, "+" CONFMT, p->n_lval);
		} else {
			fprintf(io, CONFMT, p->n_lval);
		}
		return;

	case REG:
		fputs(rnames[p->n_rval], io);
		return;

	default:
		comperr("adrput: illegal op %d", p->n_op);
	}
}

void
cbgen(int o, int lab)
{
	/* Conditional-branch helper. Object RISC's branches all take
	 * label operands directly, no need for special generation. */
}

void
myreader(struct interpass *ipole)
{
}

void
mycanon(NODE *p)
{
}

void
myoptim(struct interpass *ipole)
{
}

/*
 * Emit a register-to-register move. Selecting between integer-move
 * (`addu rd, rs, r0`) and OR-move (`omov od, os`) by register class.
 */
void
rmove(int s, int d, TWORD t)
{
	int sclass = GCLASS(s);
	int dclass = GCLASS(d);

	if (sclass != dclass)
		comperr("rmove: cross-class move s=%d d=%d", s, d);

	switch (sclass) {
	case CLASSA:
		printf("\taddu %s, %s, r0\n", rnames[d], rnames[s]);
		return;
	case CLASSB:
		/* longlong: move both halves. The pair register name is
		 * synthetic — we emit two real `addu` instructions. */
		printf("\taddu r%d, r%d, r0\n", (d - 32) * 2, (s - 32) * 2);
		printf("\taddu r%d, r%d, r0\n",
		    (d - 32) * 2 + 1, (s - 32) * 2 + 1);
		return;
	case CLASSC:
		printf("\tomov %s, %s\n", rnames[d], rnames[s]);
		return;
	}
	comperr("rmove: unknown class %d", sclass);
}

int
gclass(TWORD t)
{
	if (t == LONGLONG || t == ULONGLONG)
		return CLASSB;
	/*
	 * TODO: when `__or` qualifier lands, return CLASSC for
	 * reference-typed pointers. For now everything else lives
	 * in the GPR file.
	 */
	return CLASSA;
}

void
lastcall(NODE *p)
{
}

static int
argsiz(NODE *p)
{
	TWORD t = p->n_type;
	if (t < LONGLONG || t > ULONGLONG)
		return 4;
	if (t == LONGLONG || t == ULONGLONG)
		return 8;
	return 4;
}

int
special(NODE *p, int shape)
{
	return SRNOPE;
}

void
mflags(char *str)
{
}

int
myxasm(struct interpass *ip, NODE *p)
{
	return 0;
}

int
COLORMAP(int c, int *r)
{
	int num;

	switch (c) {
	case CLASSA:
		num  = r[CLASSA];
		num += 2 * r[CLASSB];	/* each pair eats two GPRs */
		return num < 27;	/* 32 GPRs minus 5 reserved */
	case CLASSB:
		num  = r[CLASSB];
		num += (r[CLASSA] + 1) / 2;
		return num < 13;	/* 13 allocatable pairs */
	case CLASSC:
		num = r[CLASSC];
		return num < 14;	/* 16 ORs minus O0 (null) */
	}
	return 0;
}
