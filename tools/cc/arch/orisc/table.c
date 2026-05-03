/*
 * Object RISC backend for pcc — instruction selection patterns.
 *
 * First substantive cut, ported from the top-level pcc 32-bit MIPS
 * table (Enoksson/Olsson 2005). Object RISC's integer ISA is
 * mnemonically identical to MIPS R2000 (we deliberately matched), so
 * most patterns translate verbatim — the differences are register
 * naming (handled by rnames[] in local2.c) and the absence of an
 * FPU.
 *
 * What this cut covers:
 *   - SCONV / PCONV between int / pointer / short / char / ulonglong
 *   - ASSIGN (reg-reg, reg-mem, mem-reg) for byte / half / word /
 *     longlong widths
 *   - PLUS / MINUS / AND / OR / ER / UMINUS / COMPL for word and
 *     pointer-shaped values, both register-register and register-
 *     immediate forms
 *   - MUL / DIV / MOD via mult{,u} / div{,u} + mflo / mfhi
 *   - LS / RS shifts, immediate and variable
 *   - EQ / NE / OPLOG comparisons (FORCC) emitting beq / bne / etc.
 *   - OPLTYPE for byte / half / word loads and immediate loads
 *   - GOTO / CALL / UCALL
 *   - FUNARG move-to-arg-reg / spill-to-stack
 *   - UMUL dereference
 *   - The standard catch-all DF rewrites and the FREE sentinel
 *
 * What's NOT covered yet (will trip OPANY → DF rewrite or comperr):
 *   - Floating point (no FPU on Object RISC yet)
 *   - Bit fields (SFLD assignment patterns)
 *   - Struct copies (STASG)
 *   - Object register operations — no patterns for OMOV / OEQ /
 *     OISN / OLEN / OTAG / OHOME / OCAP / OL{B,BU,H,HU,W} /
 *     OS{B,H,W} / OREFLD / OREFST / OFENCE / SEND
 *   - Most longlong arithmetic (add/sub/mul/div/shift) — only basic
 *     ASSIGN and OPLTYPE patterns
 *   - Branch-likely / annulled-delay-slot variants
 *
 * Asmorisc's pseudo-instruction set (CONTRACT.md §4.4):
 *   nop, move, b, li, la — usable directly
 *   neg / beqz / bnez are NOT defined; we emit the expanded forms.
 *
 * Pattern template placeholders (pcc convention):
 *   AL / AR  = left / right operand source
 *   A1..A3   = result registers
 *   UL/UR/U1 = upper half of a CLASSB pair (longlong)
 *   AD       = destination operand
 *   M, H, S  = bit-field mask / position / size (FLD only — unused)
 *   ZC, ZE   = zzzcode hooks (call helper in local2.c)
 *   LC, LL   = label / call target
 */

#include "pass2.h"

#define TUWORD  TUNSIGNED|TULONG
#define TSWORD  TINT|TLONG
#define TWORD   TUWORD|TSWORD

struct optab table[] = {

/* Initial empty entry — pcc requires this. */
{ -1, FOREFF, SANY, TANY, SANY, TANY, 0, 0, "", },

/*
 * Pointer / integer conversions. Most are no-ops on a 32-bit
 * machine; narrowing conversions need a sign- or zero-extension.
 */

/* PCONV: integer <-> pointer is bit-pattern preservation */
{ PCONV,	INAREG,
	SAREG,	TWORD|TPOINT,
	SAREG,	TWORD|TPOINT,
		0,	RLEFT,
		"	# pconv: int<->ptr\n", },

/* int <-> ptr */
{ SCONV,	INAREG,
	SAREG,	TPOINT|TWORD,
	SAREG,	TWORD|TPOINT,
		0,	RLEFT,
		"", },

/* (u)char to (u)char/(u)short/(u)int — already in low byte */
{ SCONV,	INAREG,
	SAREG,	TCHAR|TUCHAR,
	SAREG,	TCHAR|TUCHAR|TWORD|TSHORT|TUSHORT,
		0,	RLEFT,
		"", },

/* (u)short to (u)int — already fits */
{ SCONV,	INAREG,
	SAREG,	TSHORT|TUSHORT,
	SAREG,	TWORD|TSHORT|TUSHORT,
		0,	RLEFT,
		"", },

/* (u)int to (u)int — no-op */
{ SCONV,	INAREG,
	SAREG,	TWORD,
	SAREG,	TWORD,
		0,	RLEFT,
		"", },

/* (u)int/(u)short to char — sign-extend the byte */
{ SCONV,	INAREG,
	SAREG,	TWORD|TSHORT|TUSHORT,
	SAREG,	TCHAR,
		NAREG|NASL,	RESC1,
		"	sll A1, AL, 24\n"
		"	sra A1, A1, 24\n", },

/* (u)int/(u)short to uchar — mask the byte */
{ SCONV,	INAREG,
	SAREG,	TWORD|TSHORT|TUSHORT,
	SAREG,	TUCHAR,
		NAREG|NASL,	RESC1,
		"	andi A1, AL, 255\n", },

/* (u)int to short — sign-extend the halfword */
{ SCONV,	INAREG,
	SAREG,	TWORD,
	SAREG,	TSHORT,
		NAREG|NASL,	RESC1,
		"	sll A1, AL, 16\n"
		"	sra A1, A1, 16\n", },

/* (u)int to ushort — mask the halfword */
{ SCONV,	INAREG,
	SAREG,	TWORD,
	SAREG,	TUSHORT,
		NAREG|NASL,	RESC1,
		"	andi A1, AL, 65535\n", },

/* int -> longlong (signed): low half = source, high half = sign-bit */
{ SCONV,	INBREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SBREG,	TLONGLONG,
		NBREG,	RESC1,
		"	addu A1, AL, r0	# int -> longlong (low half)\n"
		"	sra U1, AL, 31	# (sign-extend to high half)\n", },

/* int -> ulonglong: low half = source, high half = 0 */
{ SCONV,	INBREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SBREG,	TULONGLONG,
		NBREG,	RESC1,
		"	addu A1, AL, r0	# int -> ulonglong (low half)\n"
		"	addu U1, r0, r0\n", },

{ SCONV,	INBREG,
	SAREG,	TWORD|TUSHORT|TSHORT|TUCHAR|TCHAR,
	SBREG,	TLONGLONG|TULONGLONG,
		NBREG,	RESC1,
		"	addu A1, AL, r0\n"
		"	addu U1, r0, r0\n", },

/* longlong -> narrower */
{ SCONV,	INAREG,
	SBREG,	TLONGLONG|TULONGLONG,
	SAREG,	TWORD,
		NAREG,	RESC1,
		"	addu A1, AL, r0	# (u)ll -> int\n", },

{ SCONV,	INAREG,
	SBREG,	TLONGLONG|TULONGLONG,
	SAREG,	TSHORT,
		NAREG,	RESC1,
		"	sll A1, AL, 16\n"
		"	sra A1, A1, 16\n", },

{ SCONV,	INAREG,
	SBREG,	TLONGLONG|TULONGLONG,
	SAREG,	TCHAR,
		NAREG,	RESC1,
		"	sll A1, AL, 24\n"
		"	sra A1, A1, 24\n", },

{ SCONV,	INAREG,
	SBREG,	TLONGLONG|TULONGLONG,
	SAREG,	TUSHORT,
		NAREG,	RESC1,
		"	andi A1, AL, 65535\n", },

{ SCONV,	INAREG,
	SBREG,	TLONGLONG|TULONGLONG,
	SAREG,	TUCHAR,
		NAREG,	RESC1,
		"	andi A1, AL, 255\n", },

/* longlong <-> ulonglong (no-op) */
{ SCONV,	INBREG,
	SBREG,	TLONGLONG|TULONGLONG,
	SBREG,	TULONGLONG|TLONGLONG,
		0,	RLEFT,
		"", },

/*
 * Multiplication / division / modulus.
 *
 * Object RISC's MULT/MULTU write the 64-bit result to HI:LO;
 * MFLO collects the low half. DIV/DIVU write quotient to LO,
 * remainder to HI. The trailing nops are the standard pipeline
 * bubble per Vol II §6.
 */

{ MUL,	INAREG,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
		NAREG|NASR|NASL,	RESC1,
		"	multu AL, AR\n"
		"	nop\n"
		"	nop\n"
		"	mflo A1\n", },

{ MUL,	INAREG,
	SAREG,	TWORD|TUSHORT|TSHORT|TUCHAR|TCHAR,
	SAREG,	TWORD|TUSHORT|TSHORT|TUCHAR|TCHAR,
		NAREG|NASR|NASL,	RESC1,
		"	mult AL, AR\n"
		"	nop\n"
		"	nop\n"
		"	mflo A1\n", },

{ DIV,	INAREG,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
		NAREG|NASR|NASL,	RESC1,
		"	divu AL, AR\n"
		"	mflo A1\n"
		"	nop\n"
		"	nop\n", },

{ DIV,	INAREG,
	SAREG,	TWORD|TUSHORT|TSHORT|TUCHAR|TCHAR,
	SAREG,	TWORD|TUSHORT|TSHORT|TUCHAR|TCHAR,
		NAREG|NASR|NASL,	RESC1,
		"	div AL, AR\n"
		"	mflo A1\n"
		"	nop\n"
		"	nop\n", },

{ MOD,	INAREG,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
		NAREG,	RESC1,
		"	divu AL, AR\n"
		"	mfhi A1\n"
		"	nop\n"
		"	nop\n", },

{ MOD,	INAREG,
	SAREG,	TWORD|TUSHORT|TSHORT|TUCHAR|TCHAR,
	SAREG,	TWORD|TUSHORT|TSHORT|TUCHAR|TCHAR,
		NAREG,	RESC1,
		"	div AL, AR\n"
		"	mfhi A1\n"
		"	nop\n"
		"	nop\n", },

/*
 * Add / subtract. Short-immediate (SSCON) forms get addiu/subu
 * with an immediate; register-register forms get the three-operand
 * register version.
 */

{ PLUS,	INAREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SSCON,	TANY,
		NAREG|NASL,	RESC1,
		"	addi A1, AL, AR\n", },

{ PLUS,	INAREG,
	SAREG,	TUWORD|TPOINT|TUSHORT|TUCHAR,
	SSCON,	TANY,
		NAREG|NASL,	RESC1,
		"	addiu A1, AL, AR\n", },

{ PLUS,	INAREG,
	SAREG,	TUWORD|TPOINT|TUSHORT|TUCHAR,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
		NAREG|NASL,	RESC1,
		"	addu A1, AL, AR\n", },

{ PLUS,	INAREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SAREG,	TSWORD|TSHORT|TCHAR,
		NAREG|NASL,	RESC1,
		"	add A1, AL, AR\n", },

{ MINUS,	INAREG,
	SAREG,	TUWORD|TPOINT|TUSHORT|TUCHAR,
	SSCON,	TANY,
		NAREG|NASL,	RESC1,
		"	subu A1, AL, AR\n", },

{ MINUS,	INAREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SSCON,	TANY,
		NAREG|NASL,	RESC1,
		"	sub A1, AL, AR\n", },

{ MINUS,	INAREG,
	SAREG,	TUWORD|TPOINT|TUSHORT|TUCHAR,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
		NAREG|NASL,	RESC1,
		"	subu A1, AL, AR\n", },

{ MINUS,	INAREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SAREG,	TSWORD|TSHORT|TCHAR,
		NAREG|NASL,	RESC1,
		"	sub A1, AL, AR\n", },

{ UMINUS,	INAREG,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SANY,	TANY,
		NAREG|NASL,	RESC1,
		"	subu A1, r0, AL	# negate\n", },

/*
 * Generic OPSIMP — for AND / OR / ER (xor). pcc fills in `O` from
 * the operator's mnemonic. The reg/reg form uses the three-operand
 * register version; reg/imm uses the `i`-suffixed immediate form.
 */

{ OPSIMP,	INAREG,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TUCHAR|TCHAR,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TUCHAR|TCHAR,
		NAREG|NASR|NASL,	RESC1,
		"	O A1, AL, AR\n", },

{ OPSIMP,	INAREG,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TUCHAR|TCHAR,
	SPCON,	TANY,
		NAREG|NASL,	RESC1,
		"	Oi A1, AL, AR\n", },

/*
 * Shifts. Constant shift amount uses sll/srl/sra; register
 * amount uses the `v`-suffixed variants.
 */

{ RS,	INAREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SCON,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESC1,
		"	sra A1, AL, AR\n", },

{ RS,	INAREG,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
	SCON,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESC1,
		"	srl A1, AL, AR\n", },

{ LS,	INAREG,
	SAREG,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SCON,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESC1,
		"	sll A1, AL, AR\n", },

{ RS,	INAREG,
	SAREG,	TSWORD|TSHORT|TCHAR,
	SAREG,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESC1,
		"	srav A1, AL, AR\n", },

{ RS,	INAREG,
	SAREG,	TUWORD|TUSHORT|TUCHAR,
	SAREG,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESC1,
		"	srlv A1, AL, AR\n", },

{ LS,	INAREG,
	SAREG,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SAREG,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESC1,
		"	sllv A1, AL, AR\n", },

/* One's complement. nor with r0 source = not. */
{ COMPL,	INAREG,
	SAREG,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SANY,	TANY,
		NAREG|NASL,	RESC1,
		"	nor A1, r0, AL	# complement\n", },

/*
 * Assignments. Memory destinations get sw / sh / sb; register
 * destinations get a move (addu rd, rs, r0).
 */

{ ASSIGN,	FOREFF|INAREG,
	SOREG|SNAME,	TWORD|TPOINT,
	SAREG,		TWORD|TPOINT,
		0,	RDEST,
		"	sw AR, AL\n", },

{ ASSIGN,	FOREFF|INAREG,
	SOREG|SNAME,	TSHORT|TUSHORT,
	SAREG,		TSHORT|TUSHORT,
		0,	RDEST,
		"	sh AR, AL\n", },

{ ASSIGN,	FOREFF|INAREG,
	SOREG|SNAME,	TCHAR|TUCHAR,
	SAREG,		TCHAR|TUCHAR,
		0,	RDEST,
		"	sb AR, AL\n", },

{ ASSIGN,	FOREFF|INBREG,
	SOREG|SNAME,	TLONGLONG|TULONGLONG,
	SBREG,		TLONGLONG|TULONGLONG,
		0,	RDEST,
		"	sw UR, UL	; high half\n"
		"	sw AR, AL	; low half\n", },

{ ASSIGN,	FOREFF|INBREG,
	SBREG,	TLONGLONG|TULONGLONG,
	SBREG,	TLONGLONG|TULONGLONG,
		0,	RDEST,
		"	addu UL, UR, r0\n"
		"	addu AL, AR, r0\n", },

{ ASSIGN,	FOREFF|INAREG,
	SAREG,	TANY,
	SAREG,	TANY,
		0,	RDEST,
		"	addu AL, AR, r0	; reg move\n", },

/*
 * Compares. EQ/NE generate beq/bne on the operand pair directly,
 * with a delay-slot nop. Other relations fall back to the OPLOG
 * generic, which subtracts and tests against zero.
 */

{ EQ,	FORCC,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
		0,	RESCC,
		"	beq AL, AR, LC\n"
		"	nop\n", },

{ NE,	FORCC,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
		0,	RESCC,
		"	bne AL, AR, LC\n"
		"	nop\n", },

/*
 * Compare-against-zero relational ops. pcc fills in `O` from the
 * branch mnemonic (bltz/bgez/blez/bgtz/beq/bne). Note we don't have
 * beqz/bnez pseudos in asmorisc; for EQ/NE against zero pcc will
 * route through a `sub Rt, AL, r0; beq Rt, r0, LC` pattern via the
 * second OPLOG below.
 */

{ OPLOG,	FORCC,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SZERO,	TANY,
		0,	RESCC,
		"	O AL, LC\n"
		"	nop\n", },

{ OPLOG,	FORCC,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESCC,
		"	subu A1, AL, AR\n"
		"	O A1, LC\n"
		"	nop\n", },

{ OPLOG,	FORCC,
	SAREG,	TWORD|TPOINT|TSHORT|TUSHORT|TCHAR|TUCHAR,
	SSCON,	TWORD|TSHORT|TUSHORT|TCHAR|TUCHAR,
		NAREG|NASL,	RESCC,
		"	subu A1, AL, AR\n"
		"	O A1, LC\n"
		"	nop\n", },

/*
 * Convert LTYPE (leaf) to register. These are the load-to-reg
 * patterns, both for memory operands (lw/lh/lb...) and for constant
 * loads (la / li / move-from-zero).
 */

{ OPLTYPE,	INAREG,
	SANY,		TANY,
	SOREG|SNAME,	TCHAR,
		NAREG,	RESC1,
		"	lb A1, AL	; load char\n"
		"	nop\n", },

{ OPLTYPE,	INAREG,
	SANY,		TANY,
	SOREG|SNAME,	TUCHAR,
		NAREG,	RESC1,
		"	lbu A1, AL	; load uchar\n"
		"	nop\n", },

{ OPLTYPE,	INAREG,
	SANY,		TANY,
	SOREG|SNAME,	TSHORT,
		NAREG,	RESC1,
		"	lh A1, AL	; load short\n"
		"	nop\n", },

{ OPLTYPE,	INAREG,
	SANY,		TANY,
	SOREG|SNAME,	TUSHORT,
		NAREG,	RESC1,
		"	lhu A1, AL	; load ushort\n"
		"	nop\n", },

{ OPLTYPE,	INAREG,
	SANY,		TANY,
	SOREG|SNAME,	TWORD|TPOINT,
		NAREG,	RESC1,
		"	lw A1, AL	; load word\n"
		"	nop\n", },

{ OPLTYPE,	INBREG,
	SANY,		TANY,
	SOREG|SNAME,	TLONGLONG|TULONGLONG,
		NBREG,	RESC1,
		"	lw U1, UL	; longlong high\n"
		"	lw A1, AL	; longlong low\n"
		"	nop\n", },

/* Address-of (la pseudo) for SCON pointers */
{ OPLTYPE,	INAREG,
	SANY,	TANY,
	SCON,	TPOINT,
		NAREG,	RESC1,
		"	la A1, AL\n", },

/* Zero -> reg (use r0 directly via move pseudo) */
{ OPLTYPE,	INAREG,
	SANY,	TANY,
	SZERO,	TANY,
		NAREG,	RESC1,
		"	move A1, r0	; load 0\n", },

/* Generic constant -> reg (uses li pseudo, which expands to addiu
 * or lui+ori as needed) */
{ OPLTYPE,	INAREG,
	SANY,	TANY,
	SCON,	TANY,
		NAREG,	RESC1,
		"	li A1, AL\n", },

/* Zero -> longlong reg pair */
{ OPLTYPE,	INBREG,
	SANY,	TANY,
	SZERO,	TANY,
		NBREG,	RESC1,
		"	move A1, r0\n"
		"	move U1, r0\n", },

/* Constant -> longlong reg pair */
{ OPLTYPE,	INBREG,
	SANY,	TANY,
	SCON,	TANY,
		NBREG,	RESC1,
		"	li A1, AL	; longlong low\n"
		"	li U1, UL	; longlong high\n", },

/* Reg -> reg (move pseudo) */
{ OPLTYPE,	INAREG,
	SANY,	TANY,
	SANY,	TANY,
		NAREG,	RESC1,
		"	move A1, AL\n", },

/*
 * Goto. Object RISC `j` has a delay slot.
 */

{ GOTO,	FOREFF,
	SCON,	TANY,
	SANY,	TANY,
		0,	RNOP,
		"	j LL\n"
		"	nop\n", },

/*
 * Subroutine calls.
 *
 * For Object RISC the calling convention reserves a 16-byte spill
 * area at the top of the caller's frame (Vol VII §2.2). We bump
 * SP by that amount before the jal so the callee can spill its
 * incoming arg regs there if it needs to take their addresses, then
 * restore SP after the call. The `ZC` template hook in local2.c
 * issues the matching restore.
 *
 * Function-pointer call (SAREG target) goes through a register;
 * direct call (SCON target) uses the symbolic jal form.
 */

{ CALL,		FOREFF,
	SCON,	TANY,
	SANY,	TANY,
		0,	0,
		"	addiu sp, sp, -16	; reserve spill area\n"
		"	jal CL\n"
		"	nop\n"
		"ZC", },

{ UCALL,	FOREFF,
	SCON,	TANY,
	SANY,	TANY,
		0,	0,
		"	jal CL\n"
		"	nop\n", },

{ CALL,		INAREG,
	SCON,	TANY,
	SAREG,	TANY,
		NAREG,	RESC1,
		"	addiu sp, sp, -16\n"
		"	jal CL\n"
		"	nop\n"
		"ZC", },

{ UCALL,	INAREG,
	SCON,	TANY,
	SAREG,	TANY,
		NAREG,	RESC1,
		"	jal CL\n"
		"	nop\n", },

{ CALL,		INBREG,
	SCON,	TANY,
	SBREG,	TANY,
		NBREG,	RESC1,
		"	addiu sp, sp, -16\n"
		"	jal CL\n"
		"	nop\n"
		"ZC", },

{ UCALL,	INBREG,
	SCON,	TANY,
	SBREG,	TANY,
		NBREG,	RESC1,
		"	jal CL\n"
		"	nop\n", },

/* Indirect calls — target in a GPR, jalr it. */
{ CALL,		FOREFF,
	SAREG,	TANY,
	SANY,	TANY,
		0,	0,
		"	addiu sp, sp, -16\n"
		"	jalr AL\n"
		"	nop\n"
		"ZC", },

{ UCALL,	FOREFF,
	SAREG,	TANY,
	SANY,	TANY,
		0,	0,
		"	jalr AL\n"
		"	nop\n", },

{ CALL,		INAREG,
	SAREG,	TANY,
	SAREG,	TANY,
		NAREG,	RESC1,
		"	addiu sp, sp, -16\n"
		"	jalr AL\n"
		"	nop\n"
		"ZC", },

{ UCALL,	INAREG,
	SAREG,	TANY,
	SAREG,	TANY,
		NAREG,	RESC1,
		"	jalr AL\n"
		"	nop\n", },

/*
 * Function arguments. Spill to stack at known offset. Register
 * arg passing happens through bfcode/funcode in code.c, not here;
 * FUNARG only fires for arg #5+ (overflow into the caller's spill
 * area at the top of the outgoing frame).
 */

{ FUNARG,	FOREFF,
	SAREG,	TWORD|TPOINT|TCHAR|TUCHAR|TSHORT|TUSHORT,
	SANY,	TWORD|TPOINT|TCHAR|TUCHAR|TSHORT|TUSHORT,
		0,	RNULL,
		"	sw AL, AR(sp)	; spill arg\n", },

{ FUNARG,	FOREFF,
	SBREG,	TLONGLONG|TULONGLONG,
	SANY,	TLONGLONG|TULONGLONG,
		0,	RNULL,
		"	sw UL, AR(sp)\n"
		"	sw AL, AR+4(sp)\n", },

/*
 * UMUL — dereference. Already lowered by the middle-end into
 * OREG-shaped accesses for the OPLTYPE patterns above; this
 * fallback covers the cases where the deref tree wasn't folded.
 */

{ UMUL,	INAREG,
	SANY,		TANY,
	SOREG,		TWORD|TPOINT,
		NAREG,	RESC1,
		"	lw A1, AL\n"
		"	nop\n", },

{ UMUL,	INAREG,
	SANY,		TANY,
	SOREG,		TCHAR,
		NAREG,	RESC1,
		"	lb A1, AL\n"
		"	nop\n", },

{ UMUL,	INAREG,
	SANY,		TANY,
	SOREG,		TUCHAR,
		NAREG,	RESC1,
		"	lbu A1, AL\n"
		"	nop\n", },

{ UMUL,	INAREG,
	SANY,		TANY,
	SOREG,		TSHORT,
		NAREG,	RESC1,
		"	lh A1, AL\n"
		"	nop\n", },

{ UMUL,	INAREG,
	SANY,		TANY,
	SOREG,		TUSHORT,
		NAREG,	RESC1,
		"	lhu A1, AL\n"
		"	nop\n", },

/*
 * Default catch-all rewrites. These tell pcc to break the operation
 * down further when no concrete pattern above matched, so the
 * matcher can re-try with sub-trees.
 */
#define DF(x) FORREW, SANY, TANY, SANY, TANY, REWRITE, x, ""

{ UMUL,    DF(UMUL),    },
{ ASSIGN,  DF(ASSIGN),  },
{ STASG,   DF(STASG),   },
{ FLD,     DF(FLD),     },
{ OPLEAF,  DF(NAME),    },
{ OPUNARY, DF(UMINUS),  },
{ OPANY,   DF(BITYPE),  },

/* Sentinel — last entry. */
{ FREE, FREE, FREE, FREE, FREE, FREE, FREE, FREE, "no pattern matched on orisc\n" },
};

int tablesize = sizeof(table) / sizeof(table[0]);
