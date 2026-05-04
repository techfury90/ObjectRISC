# liborisc — Object RISC C library

A small standard library for C programs targeting Object RISC.
Bundled into a `.ora` archive (`liborisc.ora`) so the linker pulls
in only the parts a given program actually calls.

## What's in it

### `io.c` — console output

| Function | Effect |
|----------|--------|
| `void print_str(const char *s)` | write `strlen(s)` bytes to the console |
| `void print_char(char c)` | write a single byte |
| `void print_int(int n)` | write `n` formatted as a signed decimal integer |
| `void print_hex(unsigned int n)` | write `n` formatted as `0xHHHHHHHH` (8 hex digits) |

### `term.c` — oriscterm interaction (console + keyboard)

| Function | Effect |
|----------|--------|
| `void term_init(void)` | save boot O2/O3/O4 → O11/O14/O15, attach a queue, subscribe to keyboard. Call once. |
| `void term_print(const char *s)` | write a string to the terminal console |
| `void term_print_char(char c)` | write a single byte (uses a static lookup table; safe across rapid calls) |
| `void term_print_int(int n)` | write a signed decimal integer |
| `void term_print_hex(unsigned int n)` | write `0xHHHHHHHH` |
| `int  term_getkey(int *out_mods)` | block until next keystroke; return codepoint, write modifier mask via out_mods |

Programs MUST follow the OR-hygiene boot ABI:

    O5  = oriscterm console  (--service order)
    O6  = oriscterm keyboard (--service order)
    O11 = boot stack ref     (parked by term_init)
    O14 = boot self-svc      (parked by term_init)
    O15 = boot data ref      (parked by term_init)

Special-key codepoints (`TK_BACKSPACE`, `TK_RETURN`, etc.) and
modifier mask bits (`TK_MOD_SHIFT`, etc.) are in `liborisc.h`.

### `host_io.c` — host filesystem (hostfsd) wrapper

Documented in [`tools/devices/README.md`](../../devices/README.md)
under "hostfsd". Adds `hf_init / hf_open / hf_opendir / hf_close /
hf_read / hf_write` to the libc.

### `linkboot.c` — spawn programs via `linkbootd`

| Function | Effect |
|----------|--------|
| `int lb_init(void)` | ObjAlloc a private mailbox, attach a queue, derive an R+S sub-ref. Call once at startup, after `term_init`. |
| `int lb_spawn(const char *path)` | SEND a spawn request to `linkbootd` (in `O7` by convention) carrying `path` and the mailbox ref. Block until the guest exits; return its exit code (always 0 for MVP), 255 on a load failure, or -1 on a poll failure. |

The mailbox is a separate object from the boot self-svc on purpose:
the standard self-svc queue holds keyboard events and hostfsd
responses, and `lb_spawn` would otherwise interleave with them when
a long-running spawn lets traffic pile up. The boot ABI gets one
new slot for shells using these helpers:

    O7 = linkbootd  (--service order)

### `string.c` — string and memory primitives

| Function | Effect |
|----------|--------|
| `unsigned int strlen(const char *s)` | byte length up to (but not including) the terminating `\0` |
| `int strcmp(const char *a, const char *b)` | textbook three-way compare |
| `char *strcpy(char *dst, const char *src)` | copy `src` (including `\0`) to `dst`; return `dst` |
| `void *memcpy(void *dst, const void *src, unsigned int n)` | copy `n` bytes |
| `void *memset(void *dst, int c, unsigned int n)` | fill `n` bytes with `(char)c` |
| `int memcmp(const void *a, const void *b, unsigned int n)` | three-way compare of `n` bytes |
| `int atoi(const char *s)` | parse a signed decimal integer; skip leading whitespace |

## Using it

User programs `#include "liborisc.h"` and link against
`liborisc.ora`. The C demo runner
[`examples/cc/run_c.sh`](../../../examples/cc/run_c.sh) wires both
sides automatically — point `-I` at this directory and feed the
archive to `orld`. Selective-inclusion means a program that calls
only `print_str` doesn't pay for `string.c`'s code at all.

The OR-file inspection / OL-OS macros (`oref_eq`, `oref_isnull`,
`oref_loadw`, etc.) live in a separate header,
[`tools/cc/arch/orisc/orisc.h`](../arch/orisc/orisc.h) — they're
pure macros over inline asm and don't go in the archive. Most
programs include both headers.

## Building it

`build.sh` compiles each `.c` here through pcc + asmorisc and
bundles the resulting `.oro` files into `liborisc.ora` via
`tools/ld/oar`:

```sh
bash tools/cc/lib/build.sh
```

`run_c.sh` runs `build.sh` automatically the first time it doesn't
find `liborisc.ora`. After source changes, re-run `build.sh` (or
delete the `.ora`) to refresh.

## Layout

| File             | Role                                              |
|------------------|---------------------------------------------------|
| `liborisc.h`     | C-side declarations for everything in the archive |
| `io.c`           | Console-output functions (host stdout via the legacy bridge) |
| `term.c`         | oriscterm: console SENDs, keyboard subscription, queue helpers |
| `host_io.c`      | hostfsd client: open/read/write/close/opendir over the wire |
| `linkboot.c`     | linkbootd client: `lb_init` / `lb_spawn`         |
| `string.c`       | String + memory primitives                        |
| `build.sh`       | Compile every `.c` and bundle into `liborisc.ora` |
| `liborisc.ora`   | The archive (regenerated; not source-controlled)  |

## Adding more

Each new `.c` here becomes its own member of `liborisc.ora` and
contributes its global symbols to the archive's index. Drop the
file in, declare its prototypes in `liborisc.h`, run `build.sh`.
The linker handles the rest.
