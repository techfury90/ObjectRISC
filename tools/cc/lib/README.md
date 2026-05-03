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
| `io.c`           | Console-output functions                          |
| `string.c`       | String + memory primitives                        |
| `build.sh`       | Compile every `.c` and bundle into `liborisc.ora` |
| `liborisc.ora`   | The archive (regenerated; not source-controlled)  |

## Adding more

Each new `.c` here becomes its own member of `liborisc.ora` and
contributes its global symbols to the archive's index. Drop the
file in, declare its prototypes in `liborisc.h`, run `build.sh`.
The linker handles the rest.
