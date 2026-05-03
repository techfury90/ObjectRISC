/*
 * hello_or.c — Hello world that controls the OR file directly from C.
 *
 * Demonstrates the `__or` qualifier: `register __or T *p __asm__("oN")`
 * binds a C variable to a specific Object Register slot. Assignments
 * between such variables compile to OMOV; assignment of 0 compiles to
 * ONULL.
 *
 * Here we move the boot-supplied data section reference (in O3 per
 * CONTRACT.md §2) into O1, then invoke ConsoleWrite (firmware
 * primitive 0x320) directly from C with inline asm — no
 * console_io.s bridge needed.
 *
 * The standard hello.c still uses the asm bridge because it calls
 * console_write() from a function rather than main; once the __or
 * calling convention lands, even that path will go through C-level
 * OR refs.
 */

const char hello[] = "Hello, world!\n";

int
main(void)
{
	register __or void *o1_target __asm__("o1");
	register __or void *o3_data   __asm__("o3");

	o1_target = o3_data;        /* compiles to: omov o1, o3 */

	/* Set up firmware primitive arguments and invoke. R4 = byte
	 * offset within source object, R5 = byte count. ConsoleWrite
	 * reads from O1 (the source ref we just set up). */
	asm("addiu r4, r0, 0");     /* offset = start of `hello` */
	asm("addiu r5, r0, 14");    /* count  = strlen(hello) */
	asm("call #0x320");         /* firmware ConsoleWrite */
	asm("nop");                 /* CALL has no delay slot but pad */

	return 0;
}
