/*
 * clock.c — Object RISC libc: system clock.
 *
 * Wraps the firmware Time and Clock primitives from Vol VI §8:
 *
 *   #0x301 ReadCycles       — CPU cycle counter (ticks per
 *                             instruction; not directly meaningful as
 *                             wall-clock, but useful for relative
 *                             cycle measurements between code points).
 *   #0x400 TimeNow          — Microseconds elapsed since boot. The
 *                             spec returns a 64-bit value but the
 *                             firmware in this revision puts only the
 *                             low 32 bits in R3 — wraps at ~71 min.
 *   #0x410 ClockResolution  — Ticks-per-second of TimeNow's clock,
 *                             always 1_000_000 in this firmware.
 *
 * No OR-hygiene contract — these primitives don't touch O1..O4 or
 * any service slots, so callers don't need the boot-park dance.
 */

#include "liborisc.h"

unsigned int
read_cycles(void)
{
	unsigned int cycles;
	asm volatile(
		"call  #0x301\n"
		"nop\n"
		"addu  %0, r3, r0"
		: "=r"(cycles)
		:
		: "r2", "r3"
	);
	return cycles;
}

unsigned int
time_now_us(void)
{
	unsigned int now;
	asm volatile(
		"call  #0x400\n"
		"nop\n"
		"addu  %0, r3, r0"
		: "=r"(now)
		:
		: "r2", "r3"
	);
	return now;
}

unsigned int
clock_resolution(void)
{
	unsigned int hz;
	asm volatile(
		"call  #0x410\n"
		"nop\n"
		"addu  %0, r3, r0"
		: "=r"(hz)
		:
		: "r2", "r3"
	);
	return hz;
}
