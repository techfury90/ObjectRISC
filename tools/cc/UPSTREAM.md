# Vendored upstream

This directory is a vendored copy of [pmer/pcc](https://github.com/pmer/pcc),
itself a snapshot of [Anders Magnusson's pcc](http://pcc.ludd.ltu.se/)
dated 2021-08-10 (see `DATESTAMP`). The CVS `$Id$` markers throughout
the source date to mid-2020.

## License

BSD 2-clause (Magnusson, Sonnenberger, and contributors) for the
modern code; the historic AT&T-derived pieces are covered by the
Caldera 2002 release of the original Unix sources. Both are MIT-
compatible — full headers are preserved in every file.

## Why vendor

We're forking the toolchain to add an Object RISC backend
(`arch/orisc/`). Keeping the full tree in-repo means:

- We can cross-reference the existing 16 architecture backends
  (especially the top-level 32-bit MIPS port and `arch/m68k/`, both
  good size and shape templates) without context-switching.
- Builds of the C compiler are reproducible from one `git clone` of
  ObjectRISC.
- Modifications to the middle-end (`mip/`) for capability-aware code
  generation are isolated to our fork.

The cost is ~4 MB of vendored source; cheap.

## Layout

The top-level source files (`code.c`, `local.c`, `local2.c`,
`table.c`, `macdefs.h`, `order.c`) are pmer's default-MIPS build
configuration — *not* a separate architecture. The `arch/` directory
contains all the alternative backends pcc supports: aarch64, amd64,
arm, hppa, i386, i86, m16c, m68k, mips64, nova, pdp10, pdp11, pdp7,
powerpc, sparc64, vax. Each backend has the canonical pcc target
shape: `macdefs.h` (target macros), `code.c` (function entry/exit),
`local.c` (frontend ABI hooks), `local2.c` (backend hooks),
`table.c` (instruction selection patterns), `order.c` (operand
ordering hints).

## Plan

The Object RISC port lives at `arch/orisc/` (not yet present at the
time of this vendor commit). The work in rough order:

1. `macdefs.h` — type sizes, register classes, ABI constants.
2. `code.c` — function prologue/epilogue, argument lowering.
3. `local.c` — frontend ABI hooks (struct return, varargs).
4. `local2.c` — backend hooks (peephole patterns, special idioms).
5. `table.c` — instruction selection patterns (the bulk of the work).
6. `order.c` — register-allocator hints.

The top-level 32-bit MIPS port is the closest template: same
load-store RISC, same R/I/J formats, same HI/LO pair for MULT/DIV,
same branch-delay-slot semantics. The Object-RISC-specific bits
(OR file, SEND, OREFLD/OREFST, OFENCE) will be exposed as
`__builtin_*` intrinsics with backend patterns matching them, and
references-as-lvalues marked with an `__or` storage-class qualifier.
