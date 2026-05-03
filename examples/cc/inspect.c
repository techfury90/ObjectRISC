/*
 * inspect.c — peek at the boot-supplied object references.
 *
 * The loader hands every initial task three object references:
 *   O1 = code   (R+X+C caps)
 *   O2 = stack  (R+W+C caps)
 *   O3 = data   (R+C caps)
 *
 * This program reads each reference's metadata using the OL/OEQ/
 * OISN/OLEN/OTAG/OHOME/OCAP wrappers in orisc.h, then prints the
 * results.
 *
 * Important: we read EVERY metadata value into ordinary int locals
 * BEFORE the first print call. The console_io.s bridge that
 * print_str ultimately invokes clobbers O1 (it does `omov o1, o3`
 * to set up the source ref), so any subsequent OR inspection
 * reading O1 would see the data ref instead of the code ref. This
 * is the calling-convention problem made visible — until the
 * `__or` calling convention lands and we can declare which OR
 * slots are caller-saved vs callee-preserved, every demo that
 * inspects ORs alongside printing has to snapshot first.
 */

#include "orisc.h"

extern void print_str(const char *s);
extern void print_int(int n);

int
main(void)
{
	register __or void *o1_code __asm__("o1");
	register __or void *o2_stk  __asm__("o2");
	register __or void *o3_data __asm__("o3");

	/* Snapshot all the metadata first. */
	int code_len  = oref_len(o1_code);
	int code_tag  = oref_tag(o1_code);
	int code_home = oref_home(o1_code);
	int code_caps = oref_caps(o1_code);

	int stk_len   = oref_len(o2_stk);
	int stk_tag   = oref_tag(o2_stk);
	int stk_caps  = oref_caps(o2_stk);

	int data_len  = oref_len(o3_data);
	int data_tag  = oref_tag(o3_data);
	int data_caps = oref_caps(o3_data);

	int code_eq_stk = oref_eq(o1_code, o2_stk);
	int stk_eq_stk  = oref_eq(o2_stk, o2_stk);

	/* Now print. */
	print_str("O1 (code):  len=");
	print_int(code_len);
	print_str("  tag=");
	print_int(code_tag);
	print_str("  home=");
	print_int(code_home);
	print_str("  caps=");
	print_int(code_caps);
	print_str("\n");

	print_str("O2 (stack): len=");
	print_int(stk_len);
	print_str("  tag=");
	print_int(stk_tag);
	print_str("  caps=");
	print_int(stk_caps);
	print_str("\n");

	print_str("O3 (data):  len=");
	print_int(data_len);
	print_str("  tag=");
	print_int(data_tag);
	print_str("  caps=");
	print_int(data_caps);
	print_str("\n");

	print_str("code == stack? ");
	print_int(code_eq_stk);
	print_str("\nstack == stack? ");
	print_int(stk_eq_stk);
	print_str("\n");
	return 0;
}
