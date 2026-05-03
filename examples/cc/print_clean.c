/*
 * print_clean.c — Hello world via firmware_console_write.
 *
 * Uses pcc's caller-side `__or` calling convention. The function
 * `firmware_console_write` is declared with `void *__or src` as
 * the first parameter; pcc routes the corresponding caller
 * argument through O1 (the firmware ConsoleWrite source-ref slot)
 * via the calling convention. R4 and R5 carry the offset and
 * count as usual integer args.
 *
 * This is the cleanest way to call firmware from C today: only
 * the function declaration needs the `__or` qualifier; everything
 * else is plain C. The implementation
 * (tools/cc/arch/orisc/console_io.s) is a 4-instruction
 * pass-through to `call #0x320`.
 *
 * Compare hello.c (uses VA-range heuristic in console_io.s) and
 * hello_or.c / print_or.c (use inline asm). This is the cleanest
 * of the three — ABI does the work.
 */

extern int orisc_console_write(void *__or src, int offset, int count);

const char hello[] = "Hello, world!\n";

int
main(void)
{
	register void *__or o3_data __asm__("o3");

	orisc_console_write(o3_data, 0, 14);
	return 0;
}
