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
 * Max outgoing-arg-spill bytes across all calls in the current
 * function (set by funcode() in code.c during pass1, read here in
 * pass2). moveargs() spills call args #5+ to OFFSET(sp), i.e. the
 * bottom of our frame; we must reserve this many bytes there or the
 * stores clobber locals / saved registers.
 */
int orisc_argszmax;

/*
 * Calculate stack frame size — automatic-storage area, one slot per
 * callee-preserved GPR this function uses, plus the outgoing-arg
 * spill area for the largest call. Returns the total in bytes,
 * rounded up to 8-byte alignment per Vol VII §2.2.
 *
 * Layout below FP (high → low address): autos [0..p2maxautooff),
 * then callee-preserved reg saves, then the outgoing-arg area at the
 * very bottom (sp+0..). Autos and reg saves are addressed FP-relative
 * so they're unaffected by the outgoing-arg reservation; growing the
 * frame at the bottom simply moves sp down, keeping sp+OFFSET clear.
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

	/* Reserve the outgoing-arg spill area at the bottom of the frame
	 * (where moveargs' "sw AL, OFFSET(sp)" stores land). Without this
	 * a function that both has locals and makes a 5+-arg call writes
	 * its spilled args over its own locals. */
	addto += (orisc_argszmax + 7) & ~7;

	return addto;
}

/*
 * Emit the function prologue. Mirrors the MIPS port's frame setup,
 * adapted to our naming and to drop the .ent/.frame/.cpload pseudo-
 * ops asmorisc doesn't recognize.
 *
 *     funcname:
 *         addiu sp, sp, -16              ; reserve 16-byte fixed area
 *         sw    r31, 4(sp)               ; save link register
 *         sw    fp, 0(sp)                ; save caller's FP
 *         addu  fp, sp, r0               ; FP <- SP (anchors locals/saves)
 *         addiu sp, sp, -addto           ; reserve locals + reg-save area
 *         sw    rN, -regoff(fp)          ; per used callee-preserved
 *
 * The 16 bytes at the top of OUR frame (above FP) hold caller's
 * outgoing-arg spill area per Vol VII §2.2. Callee-preserved regs
 * and locals live below FP. The frame pointer is set up
 * unconditionally; the Vol VII §2.1 "FP optional" clause
 * acknowledges that fixed-frame functions can skip it, but pcc's
 * codegen consistently emits FP-relative addressing for autos.
 */
void
prologue(struct interpass_prolog *ipp)
{
	int addto;
	int i, j;

	ftype = ipp->ipp_type;

	/* Asmorisc has no .globl; visibility is implicit. The .entry
	 * directive (CONTRACT.md §4.2) is emitted by the startup
	 * file (crt0.s) which calls main and TaskExits with its
	 * return value, not by the C compiler itself. */

	printf("%s:\n", ipp->ipp_name);

	addto = offcalc(ipp);

	printf("\taddiu sp, sp, -%d\n", ARGINIT/SZCHAR);
	printf("\tsw r31, 4(sp)\n");
	printf("\tsw fp, 0(sp)\n");
	printf("\taddu fp, sp, r0\n");

	if (addto)
		printf("\taddiu sp, sp, -%d\n", addto);

	for (i = p2env.p_regs[0], j = 0; i; i >>= 1, j++)
		if (i & 1)
			printf("\tsw r%d, -%d(fp) ; save callee-preserved\n",
			    j, regoff[j]);
}

/*
 * Emit the function epilogue.
 *
 *         lw    rN, -regoff(fp)          ; restore each callee-preserved
 *         addiu sp, fp, 16               ; deallocate locals + reach RA slot
 *         lw    r31, -12(sp)             ; restore RA from old slot
 *         lw    fp, -16(sp)              ; restore caller's FP
 *         jr    r31
 *         nop
 *
 * Layout reasoning: after `addiu sp, fp, 16`, sp is at the address
 * just above the saved-RA slot — i.e., the entry sp. RA was saved at
 * fp+4 (relative to old fp = old sp), which is now sp-12 because
 * we just bumped sp by 16. Same for FP at fp+0 → sp-16.
 */
void
eoftn(struct interpass_prolog *ipp)
{
	int i, j;

	(void)offcalc(ipp);

	if (ipp->ipp_ip.ip_lbl == 0)
		return;	/* no code generated */

	for (i = p2env.p_regs[0], j = 0; i; i >>= 1, j++)
		if (i & 1)
			printf("\tlw r%d, -%d(fp)\n", j, regoff[j]);

	printf("\taddiu sp, fp, %d\n", ARGINIT/SZCHAR);
	printf("\tlw r31, %d(sp)\n", 4 - ARGINIT/SZCHAR);
	printf("\tlw fp, %d(sp)\n", 0 - ARGINIT/SZCHAR);
	printf("\tjr r31\n");
	printf("\tnop\n");
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
	/* Comparison ops emit branches against zero (used by the
	 * OPLOG SZERO pattern in table.c). asmorisc provides beqz
	 * and bnez pseudos; the rest map directly to bltz/bgez/
	 * blez/bgtz which take a single GPR operand. Unsigned
	 * variants reuse the same mnemonics since the comparison
	 * is against zero. */
	case EQ:	str = "beqz";	break;
	case NE:	str = "bnez";	break;
	case ULE: case LE:	str = "blez";	break;
	case ULT: case LT:	str = "bltz";	break;
	case UGE: case GE:	str = "bgez";	break;
	case UGT: case GT:	str = "bgtz";	break;
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

/*
 * Struct assignment — `*dst = *src;` for a struct, where both sides
 * may be lvalues or pointer-derefs. Modeled on the MIPS64 backend
 * (tools/cc/arch/mips64/local2.c::stasg): pcc's rewriter drops the
 * source pointer into R5 (= A1) before reaching us, leaving us to
 *
 *   1. set R6 (= A2) to the byte size,
 *   2. compute the dest address into R4 (= A0) — either base+offset
 *      from a register (OREG) or the address of a named global (NAME),
 *   3. reserve the standard 16-byte outgoing-arg spill area,
 *   4. JAL memcpy + nop delay slot,
 *   5. unwind the spill area.
 *
 * The struct size is stashed in the node's ATTR_P2STRUCT attribute
 * by the framework. Caller-saved scratch registers and the source
 * pointer are spilled / preserved by pcc's standard call-clobber
 * machinery — we just emit the call.
 */
void
stasg(NODE *p)
{
	int sz = attr_find(p->n_ap, ATTR_P2STRUCT)->iarg(0);

	assert(p->n_right->n_rval == R5);

	printf("\tli r6, %d\t; struct size\n", sz);

	if (p->n_left->n_op == OREG) {
		printf("\taddiu r4, %s, " CONFMT "\t; dest addr\n",
		    rnames[p->n_left->n_rval], getlval(p->n_left));
	} else if (p->n_left->n_op == NAME) {
		printf("\tla r4, ");
		adrput(stdout, p->n_left);
		printf("\n");
	} else {
		comperr("stasg: unhandled n_left op %d", p->n_left->n_op);
	}

	printf("\taddiu sp, sp, -16\n");
	printf("\tjal %s\t; struct copy via memcpy\n", exname("memcpy"));
	printf("\tnop\n");
	printf("\taddiu sp, sp, 16\n");
}

int
shiftop(NODE *p)
{
	comperr("shiftop not implemented for orisc");
	return 0;
}

/*
 * zzzcode template hook — called by table.c patterns containing
 * `Zx` to emit code that's awkward to express as a static template.
 * We implement only the cases the current pattern table actually
 * uses; new ones get added as the table grows.
 */
void
zzzcode(NODE *p, int c)
{
	int sz;

	switch (c) {
	case 'C':
		/* Restore SP after a call — undoes the 16-byte spill
		 * area we reserved before the jal. Per MIPS pcc the
		 * actual amount is max(16, n_qual) so callers passing
		 * more than 4 stack args bump SP back by the larger
		 * amount. */
		sz = p->n_qual > 16 ? p->n_qual : 16;
		printf("\taddiu sp, sp, %d\n", sz);
		break;
	case 'Q':
		/* Struct assignment — emit a memcpy call. The matched
		 * STASG node has the source pointer in R5 (forced by the
		 * NSPECIAL entry in order.c::nspecial), the dest is in
		 * p->n_left (OREG or NAME), and the byte size is in the
		 * ATTR_P2STRUCT attribute. local2.c::stasg does the rest. */
		stasg(p);
		break;
	default:
		comperr("zzzcode '%c' not implemented for orisc", c);
	}
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
			fprintf(fp, CONFMT, getlval(p));
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
		if (getlval(p) != 0)
			fprintf(io, "+" CONFMT, getlval(p));
		return;

	case OREG:
		r = p->n_rval;
		if (getlval(p))
			fprintf(io, CONFMT, getlval(p));
		fprintf(io, "(%s)", rnames[r]);
		return;

	case ICON:
		if (p->n_name[0] != '\0') {
			fputs(p->n_name, io);
			if (getlval(p) != 0)
				fprintf(io, "+" CONFMT, getlval(p));
		} else {
			fprintf(io, CONFMT, getlval(p));
		}
		return;

	case REG:
		fputs(rnames[p->n_rval], io);
		return;

	case FUNARG:
		/*
		 * Outgoing stack argument (call arg #5+). moveargs() in
		 * code.c stashed this arg's byte offset within the outgoing-
		 * arg area in n_rval; the FUNARG template ("sw AL, AR(sp)")
		 * references it via AR, which getlr() resolves to the FUNARG
		 * node itself (it is UTYPE, so 'R' returns p). Emit the raw
		 * offset so the store lands at OFFSET(sp).
		 */
		fprintf(io, "%d", p->n_rval);
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
	/*
	 * Object-reference pointers (OBIT set in the type word) live in
	 * the OR file (CLASSC). Because OBIT rides n_type — preserved by
	 * every node creator/copier — this classes return temps, param
	 * temps and spill nodes CLASSC by construction, with no reliance
	 * on the (creator-zeroed) qualifier word.
	 */
	if (ISOREFT(t))
		return CLASSC;
	if (t == LONGLONG || t == ULONGLONG)
		return CLASSB;
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
