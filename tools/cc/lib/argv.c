/*
 * argv.c — accessor for the argv buffer mapped at ARGV_VA.
 *
 * orx_run (or any TaskCreate caller) optionally hands the new task
 * an O4 = args ref; the firmware's TaskCreate maps it R-only at a
 * fixed virtual address (ARGV_VA = 0x000a0000). Programs read
 * their args via this helper rather than dereferencing the VA
 * directly.
 *
 * No length is shipped — the convention is a single NUL-terminated
 * raw command string. Programs that want true argc/argv split it
 * themselves (see e.g. examples/cc/programs/edit.c).
 *
 * If the launcher didn't set up an argv mapping, the helper
 * returns an empty string. We don't actually try to dereference
 * ARGV_VA to detect that — the libc has no "is this VA mapped?"
 * primitive — so the convention is: orx_run ALWAYS sets up an
 * argv mapping (empty buffer if no args). Direct TaskCreate
 * callers that skip O4 will trap on access. Acceptable v1
 * tradeoff.
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
