/*
 * host_cat.c — read a host file and print it to the console.
 *
 * Demonstrates:
 *   - hf_init / hf_open / hf_read / hf_close (the new host_io libc)
 *   - the OR-hygiene contract host_io requires (boot O2/O3/O4 parked
 *     in O11/O14/O15 once at startup)
 *   - reading more bytes than fit in one SEND by looping
 *
 * Boot environment expected (set up by run_host_cat.sh):
 *     O3  = our data segment   (parked in O15)
 *     O4  = our self-service   (parked in O14)
 *     O2  = our stack          (parked in O11)
 *     O10 = hostfsd service ref
 *
 * Path is fixed at "README.md" for the demo. Edit and rebuild to
 * point at something else, or extend with argv parsing later.
 */

#include "liborisc.h"

const char path_str[] = "README.md";

int
main(void)
{
	register void *__or o2_stack       __asm__("o2");
	register void *__or o3_data        __asm__("o3");
	register void *__or o4_self        __asm__("o4");
	register void *__or o11_stack_save __asm__("o11");
	register void *__or o14_self_save  __asm__("o14");
	register void *__or o15_data_save  __asm__("o15");

	int fd, n;
	char buf[256];

	/* Park boot refs in the slots host_io expects. */
	o11_stack_save = o2_stack;
	o14_self_save  = o4_self;
	o15_data_save  = o3_data;

	if (hf_init() != 0) {
		print_str("hf_init failed\n");
		return 1;
	}

	fd = hf_open(path_str, HF_O_RDONLY);
	if (fd < 0) {
		print_str("hf_open(\"");
		print_str(path_str);
		print_str("\") failed: ");
		print_int(fd);
		print_str("\n");
		return 2;
	}

	print_str("---- ");
	print_str(path_str);
	print_str(" ----\n");

	while (1) {
		n = hf_read(fd, buf, sizeof(buf));
		if (n <= 0) break;
		/* Print whatever bytes we got (may be < sizeof(buf) at EOF). */
		{
			int i;
			for (i = 0; i < n; i++) print_char(buf[i]);
		}
	}
	if (n < 0) {
		print_str("\nhf_read failed: ");
		print_int(n);
		print_str("\n");
	}

	hf_close(fd);
	print_str("---- end ----\n");
	return 0;
}
