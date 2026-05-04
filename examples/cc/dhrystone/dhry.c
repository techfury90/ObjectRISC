/*
 * dhry.c — Dhrystone Benchmark, Object RISC port.
 *
 * This is the canonical Dhrystone v2.1 (Reinhold Weicker, 1984;
 * Andrew C. Lowry, 1985 update) compressed into one file and
 * adapted for the Object RISC C runtime:
 *
 *   - printf → print_str / print_int from liborisc.
 *   - malloc → two static Rec_Type instances.
 *   - clock() / time() → time_now_us() and read_cycles() from
 *     liborisc's clock.c (#0x301 ReadCycles, #0x400 TimeNow).
 *   - Number_Of_Runs hardcoded via DHRY_RUNS (no scanf prompt).
 *   - register keyword dropped — pcc's orisc backend doesn't
 *     gain anything from it given the current allocator state,
 *     and removing it keeps the source uniform.
 *   (Earlier ports of this file substituted memcpy for struct
 *   assignments and reordered globals to dodge a missing-.align
 *   bug — both have been fixed in the pcc orisc backend, so the
 *   source here matches the canonical Dhrystone shape.)
 *
 * Algorithm and instruction mix are unchanged. The result lines
 * report:
 *   - elapsed cycles + microseconds
 *   - dhrystones / second
 *   - DMIPS at the alternate-history OR-1000 nominal clock rates
 *     (16 MHz and 20 MHz from Vol I §3) — derived from cycles, not
 *     from sim wall-clock, since the simulator runs many orders of
 *     magnitude slower than the hypothetical silicon would.
 *
 * The DMIPS reference is 1757 dhrystones/sec = 1 DMIPS, the
 * historical VAX 11/780 baseline.
 *
 * Bump DHRY_RUNS to taste. The default fits comfortably in the
 * 32-bit microsecond range exposed by the firmware's TimeNow
 * (low 32 bits wrap at ~71 minutes).
 */

#include "liborisc.h"

#ifndef DHRY_RUNS
#define DHRY_RUNS 5000
#endif

/* --- types ----------------------------------------------------- */

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;
typedef int  One_Thirty;
typedef int  One_Fifty;
typedef char Capital_Letter;
typedef int  Boolean;
typedef char Str_30 [31];
typedef int  Arr_1_Dim [50];
typedef int  Arr_2_Dim [50][50];

typedef struct record {
	struct record *Ptr_Comp;
	Enumeration    Discr;
	union {
		struct {
			Enumeration Enum_Comp;
			int         Int_Comp;
			char        Str_Comp[31];
		} var_1;
		struct {
			Enumeration E_Comp_2;
			char        Str_2_Comp[31];
		} var_2;
		struct {
			char Ch_1_Comp;
			char Ch_2_Comp;
		} var_3;
	} variant;
} Rec_Type, *Rec_Pointer;

/* --- globals --------------------------------------------------- *
 *
 * The reference Dhrystone allocates two Rec_Type instances via
 * malloc. We don't have a heap; declare them statically and point
 * Ptr_Glob / Next_Ptr_Glob at them at startup. Same memory shape,
 * same access patterns. */

Rec_Type Rec_1, Rec_2;
Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
int         Int_Glob;
Boolean     Bool_Glob;
char        Ch_1_Glob, Ch_2_Glob;
int         Arr_1_Glob[50];
int         Arr_2_Glob[50][50];

/* --- forward decls --------------------------------------------- */

void  Proc_1 (Rec_Pointer Ptr_Val_Par);
void  Proc_2 (One_Fifty *Int_Par_Ref);
void  Proc_3 (Rec_Pointer *Ptr_Ref_Par);
void  Proc_4 (void);
void  Proc_5 (void);
void  Proc_6 (Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par);
void  Proc_7 (One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
              One_Fifty *Int_Par_Ref);
void  Proc_8 (Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
              int Int_1_Par_Val, int Int_2_Par_Val);
Enumeration Func_1 (Capital_Letter Ch_1_Par_Val,
                    Capital_Letter Ch_2_Par_Val);
Boolean     Func_2 (Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref);
Boolean     Func_3 (Enumeration Enum_Par_Val);

/* --- helpers --------------------------------------------------- */

/* Local string-equality test, since liborisc's strcmp signature
 * isn't quite what Func_2 wants and pulling stdlib/<string.h> would
 * be overkill. Returns nonzero on equal. */
static int
str_eq (const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}

static void
str_copy (char *dst, const char *src)
{
	while ((*dst++ = *src++)) ;
}

/* --- Procs and Funcs -------------------------------------------- */

void
Proc_1 (Rec_Pointer Ptr_Val_Par)
{
	Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;

	*Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
	Ptr_Val_Par->variant.var_1.Int_Comp = 5;
	Next_Record->variant.var_1.Int_Comp =
		Ptr_Val_Par->variant.var_1.Int_Comp;
	Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
	Proc_3 (&Next_Record->Ptr_Comp);
	if (Next_Record->Discr == Ident_1) {
		Next_Record->variant.var_1.Int_Comp = 6;
		Proc_6 (Ptr_Val_Par->variant.var_1.Enum_Comp,
		        &Next_Record->variant.var_1.Enum_Comp);
		Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
		Proc_7 (Next_Record->variant.var_1.Int_Comp, 10,
		        &Next_Record->variant.var_1.Int_Comp);
	} else {
		*Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
	}
}

void
Proc_2 (One_Fifty *Int_Par_Ref)
{
	One_Fifty   Int_Loc;
	Enumeration Enum_Loc;

	Int_Loc = *Int_Par_Ref + 10;
	do {
		if (Ch_1_Glob == 'A') {
			Int_Loc -= 1;
			*Int_Par_Ref = Int_Loc - Int_Glob;
			Enum_Loc = Ident_1;
		}
	} while (Enum_Loc != Ident_1);
}

void
Proc_3 (Rec_Pointer *Ptr_Ref_Par)
{
	if (Ptr_Glob != 0) {
		*Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
	}
	Proc_7 (10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

void
Proc_4 (void)
{
	Boolean Bool_Loc;
	Bool_Loc = Ch_1_Glob == 'A';
	Bool_Glob = Bool_Loc | Bool_Glob;
	Ch_2_Glob = 'B';
}

void
Proc_5 (void)
{
	Ch_1_Glob = 'A';
	Bool_Glob = 0;
}

void
Proc_6 (Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par)
{
	*Enum_Ref_Par = Enum_Val_Par;
	if (!Func_3 (Enum_Val_Par)) *Enum_Ref_Par = Ident_4;
	switch (Enum_Val_Par) {
	case Ident_1: *Enum_Ref_Par = Ident_1; break;
	case Ident_2:
		if (Int_Glob > 100) *Enum_Ref_Par = Ident_1;
		else                *Enum_Ref_Par = Ident_4;
		break;
	case Ident_3: *Enum_Ref_Par = Ident_2; break;
	case Ident_4: break;
	case Ident_5: *Enum_Ref_Par = Ident_3; break;
	}
}

void
Proc_7 (One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
        One_Fifty *Int_Par_Ref)
{
	One_Fifty Int_Loc;
	Int_Loc = Int_1_Par_Val + 2;
	*Int_Par_Ref = Int_2_Par_Val + Int_Loc;
}

void
Proc_8 (Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
        int Int_1_Par_Val, int Int_2_Par_Val)
{
	One_Fifty Int_Index, Int_Loc;
	Int_Loc = Int_1_Par_Val + 5;
	Arr_1_Par_Ref[Int_Loc]                  = Int_2_Par_Val;
	Arr_1_Par_Ref[Int_Loc + 1]              = Arr_1_Par_Ref[Int_Loc];
	Arr_1_Par_Ref[Int_Loc + 30]             = Int_Loc;
	for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index)
		Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
	Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
	Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
	Int_Glob = 5;
}

Enumeration
Func_1 (Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val)
{
	Capital_Letter Ch_1_Loc, Ch_2_Loc;
	Ch_1_Loc = Ch_1_Par_Val;
	Ch_2_Loc = Ch_1_Loc;
	if (Ch_2_Loc != Ch_2_Par_Val) return Ident_1;
	else { Ch_1_Glob = Ch_1_Loc; return Ident_2; }
}

Boolean
Func_2 (Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref)
{
	One_Thirty     Int_Loc;
	Capital_Letter Ch_Loc;
	Int_Loc = 2;
	while (Int_Loc <= 2) {
		if (Func_1 (Str_1_Par_Ref[Int_Loc], Str_2_Par_Ref[Int_Loc + 1])
		        == Ident_1) {
			Ch_Loc = 'A';
			Int_Loc += 1;
		}
	}
	if (Ch_Loc >= 'W' && Ch_Loc < 'Z') Int_Loc = 7;
	if (Ch_Loc == 'R') return 1;
	else {
		if (str_eq (Str_1_Par_Ref, Str_2_Par_Ref)) {
			Int_Loc += 7;
			Int_Glob = Int_Loc;
			return 1;
		}
		else return 0;
	}
}

Boolean
Func_3 (Enumeration Enum_Par_Val)
{
	Enumeration Enum_Loc;
	Enum_Loc = Enum_Par_Val;
	if (Enum_Loc == Ident_3) return 1;
	else                     return 0;
}

/* --- main ------------------------------------------------------- */

int
main (void)
{
	One_Fifty   Int_1_Loc, Int_2_Loc, Int_3_Loc;
	Capital_Letter Ch_Index;
	Enumeration Enum_Loc;
	Str_30      Str_1_Loc, Str_2_Loc;
	int         Run_Index;

	unsigned int t0_us, t1_us, dt_us;
	unsigned int c0,    c1,    dc;

	/* Initialization (replaces the malloc'd Ptr_Glob / Next_Ptr_Glob). */
	Ptr_Glob = &Rec_1;
	Next_Ptr_Glob = &Rec_2;

	Ptr_Glob->Ptr_Comp                       = Next_Ptr_Glob;
	Ptr_Glob->Discr                          = Ident_1;
	Ptr_Glob->variant.var_1.Enum_Comp        = Ident_3;
	Ptr_Glob->variant.var_1.Int_Comp         = 40;
	str_copy (Ptr_Glob->variant.var_1.Str_Comp,
	          "DHRYSTONE PROGRAM, SOME STRING");
	str_copy (Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");

	Arr_2_Glob[8][7] = 10;

	print_str ("\nDhrystone Benchmark, Version 2.1 (Object RISC port)\n");
	print_str ("Iterations: ");
	print_int (DHRY_RUNS);
	print_str ("\n\n");

	c0    = read_cycles ();
	t0_us = time_now_us ();

	for (Run_Index = 1; Run_Index <= DHRY_RUNS; ++Run_Index) {
		Proc_5 ();
		Proc_4 ();
		Int_1_Loc = 2;
		Int_2_Loc = 3;
		str_copy (Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
		Enum_Loc = Ident_2;
		Bool_Glob = !Func_2 (Str_1_Loc, Str_2_Loc);
		while (Int_1_Loc < Int_2_Loc) {
			Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
			Proc_7 (Int_1_Loc, Int_2_Loc, &Int_3_Loc);
			Int_1_Loc += 1;
		}
		Proc_8 (Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
		Proc_1 (Ptr_Glob);
		for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index) {
			if (Enum_Loc == Func_1 (Ch_Index, 'C'))
				Proc_6 (Ident_1, &Enum_Loc);
		}
		Int_3_Loc = Int_2_Loc * Int_1_Loc;
		Int_2_Loc = Int_3_Loc / Int_1_Loc;
		Int_2_Loc = 7 * (Int_3_Loc - Int_2_Loc) - Int_1_Loc;
		Proc_2 (&Int_1_Loc);
	}

	t1_us = time_now_us ();
	c1    = read_cycles ();
	dt_us = t1_us - t0_us;
	dc    = c1    - c0;

	print_str ("Final values of the variables used in the benchmark:\n");
	print_str ("  Int_Glob:           ");
	print_int (Int_Glob);
	print_str ("    (should be 5)\n  Bool_Glob:          ");
	print_int (Bool_Glob);
	print_str ("    (should be 1)\n  Ch_1_Glob / Ch_2_Glob: ");
	print_char (Ch_1_Glob);
	print_str (" / ");
	print_char (Ch_2_Glob);
	print_str ("  (should be A / B)\n");

	print_str ("\nMicroseconds elapsed: ");
	print_int ((int)dt_us);
	print_str ("\nCycles elapsed:       ");
	print_int ((int)dc);
	print_str ("\n");

	/* dhrystones/sec from the cycle count + nominal clock rates.
	 *
	 * Done in two steps to dodge 32-bit overflow:
	 *
	 *   cycles_per_iter = dc / DHRY_RUNS
	 *   dhry_per_sec    = freq / cycles_per_iter
	 *
	 * At freq=20MHz and DHRY_RUNS=5000 the more obvious form
	 * `DHRY_RUNS * freq / dc` would compute 1e11 mid-expression
	 * (well above 32 bits). Going via cycles-per-iter both
	 * intermediate values fit in 32 bits comfortably. */
	if (dc > 0) {
		unsigned int cyc_per_iter = dc / (unsigned int)DHRY_RUNS;
		if (cyc_per_iter == 0) cyc_per_iter = 1;
		unsigned int dh_per_sec_16 = 16000000U / cyc_per_iter;
		unsigned int dh_per_sec_20 = 20000000U / cyc_per_iter;
		print_str ("\nCycles per iteration: ");
		print_int ((int)cyc_per_iter);
		print_str ("\n\nAt nominal clock rates (Vol I §3):\n");
		print_str ("  16 MHz: ~");
		print_int ((int)dh_per_sec_16);
		print_str (" dhry/s   = ~");
		print_int ((int)(dh_per_sec_16 / 1757));
		print_str (".");
		print_int ((int)((dh_per_sec_16 % 1757) * 10 / 1757));
		print_str (" DMIPS\n  20 MHz: ~");
		print_int ((int)dh_per_sec_20);
		print_str (" dhry/s   = ~");
		print_int ((int)(dh_per_sec_20 / 1757));
		print_str (".");
		print_int ((int)((dh_per_sec_20 % 1757) * 10 / 1757));
		print_str (" DMIPS\n");
		print_str ("\n(VAX 11/780 = 1757 dhrystones/sec = 1 DMIPS reference.)\n");
	}

	return 0;
}
