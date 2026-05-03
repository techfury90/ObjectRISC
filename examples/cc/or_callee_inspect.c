/*
 * or_callee_inspect.c — exercises the callee-side `__or`
 * binding in less-trivial ways.
 *
 * inspect_or takes an `__or` parameter and uses it three
 * different ways inside the body:
 *   1. compare-to-null via OISN (through the OR_ISN macro)
 *   2. read OLEN
 *   3. forward to orisc_console_write
 *
 * Each of those used to fail with "Coalesce: src class 3, dst
 * class 1" because pcc lifted the param into a CLASSA temp.
 * With the SINREG binding in bfcode, the param symbol IS the
 * O1 slot directly and the body's three uses each read it
 * straight from the OR file.
 */

#include "orisc.h"

extern int orisc_console_write(void *__or src, int offset, int count);
extern void print_str(const char *s);
extern void print_int(int n);

const char banner[] = "len = ";
const char nl[] = "\n";
const char nullmsg[] = "(null)\n";

static void
inspect_or(void *__or src)
{
	int len;

	if (oref_isnull(src)) {
		print_str(nullmsg);
		return;
	}

	len = oref_len(src);
	print_str(banner);
	print_int(len);
	print_str(nl);
}

const char hello[] = "Hello!\n";

int
main(void)
{
	register void *__or o3_data __asm__("o3");
	register void *__or null_ref __asm__("o5");

	/* O2 is the stack ref and O3 is the data ref — both used by
	 * console_write's VA-range heuristic in lib.c. Park our null
	 * test value in O5 so we don't disturb either. */
	null_ref = 0;

	inspect_or(o3_data);    /* live ref → "len = N" */
	inspect_or(null_ref);   /* null     → "(null)"  */

	orisc_console_write(o3_data, 17, 7);
	return 0;
}
