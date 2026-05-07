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

/* Phase 41a: argv plumbing exists at the shell + libc API surface
 * but the actual ARGV_VA mapping isn't wired into orx_setup_args
 * yet (deferred to a 41b). Until then this helper hands back a
 * static empty string so callers don't trap dereferencing the
 * unmapped ARGV_VA. */
static const char program_args_empty[] = "";

const char *
program_args(void)
{
	return program_args_empty;
}
