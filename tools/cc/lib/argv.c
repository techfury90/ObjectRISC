/*
 * argv.c — accessors for the argv buffer mapped at ARGV_VA.
 *
 * orx_run (or any TaskCreate caller) optionally hands the new task
 * an O4 = args ref; the firmware's TaskCreate maps it R-only at a
 * fixed virtual address (ARGV_VA = 0x000a0000). The buffer is laid
 * out as TWO NUL-terminated strings back-to-back:
 *
 *     [0..]                  args   (NUL-terminated)
 *     [strlen(args)+1 ..]    cwd    (NUL-terminated)
 *
 * `program_args()` returns a pointer to the first segment; this
 * keeps the historical contract — old programs that just want the
 * user-typed command line keep working. `program_cwd()` walks past
 * the first NUL to return the launcher's working directory, so
 * programs (the editor, viewer, etc.) can resolve relative paths
 * the user typed. The shell fills both fields in cmd_run.
 *
 * No length is shipped — programs that want true argc/argv split
 * args themselves (see e.g. ouroboros/programs/edit.c).
 *
 * If the launcher didn't set up an argv mapping, dereferencing
 * ARGV_VA traps. The convention is: orx_run ALWAYS sets up an
 * argv mapping (empty fields if no args/cwd). Direct TaskCreate
 * callers that skip O4 are responsible for not calling either
 * accessor. Acceptable v1 tradeoff.
 */

#include "liborisc.h"

#define ARGV_VA  0x000a0000U

/* Return (char *)ARGV_VA. We avoid the natural `return (char
 * *)ARGV_VA;` because pcc lowers a literal-cast to a `la r,N`
 * load-address pseudo, and asmorisc rejects it. Synthesize the VA
 * with lui+ori inline asm instead — exact same two instructions
 * the lowering would emit, just without the pseudo. */
const char *
program_args(void)
{
	const char *p;
	asm volatile(
		"lui  %0, 0xa\n"
		"ori  %0, %0, 0"
		: "=r"(p)
	);
	return p;
}

/* Return the launcher's cwd: the second NUL-terminated string in
 * the argv buffer. Walks past `args + '\0'` to find it. Empty
 * string when the launcher passed cwd=NULL/"". */
const char *
program_cwd(void)
{
	const char *p = program_args();
	while (*p) p++;
	return p + 1;
}
