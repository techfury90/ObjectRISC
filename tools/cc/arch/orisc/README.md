# pcc — Object RISC backend

The architecture backend that teaches pcc to emit Object RISC
assembly. Lives alongside the other 16 backends pcc ships with
(`arch/mips`, `arch/m68k`, `arch/i386`, …), follows their
conventions, and links into the standard `ccom` (and `cxxcom`)
front ends without modification.

For the upstream provenance and the rationale for vendoring the
whole tree see [`../../UPSTREAM.md`](../../UPSTREAM.md).

## Status

What works end-to-end (compiles, assembles, runs on `simorisc`):

- The integer ISA core: arithmetic (`addiu`, `addu`, `subu`,
  `multu`/`mflo`/`mfhi`, `divu`), logicals (`and`/`or`/`xor`/
  `nor`), shifts (immediate and variable), comparisons (slt
  family), branches and jumps (with proper architectural delay
  slots), and direct/indirect calls.
- Pointer arithmetic, character arrays with `.ascii` coalescing,
  multi-file linkage against `examples/cc/lib.c`'s
  `print_str`/`print_int`.
- The `__or` storage-class qualifier, in three forms:
  - **Explicit register binding.**
    `register __or void *p __asm__("oN")` lands a C variable
    in OR slot N. Assignments compile to `omov`; assigning `0`
    compiles to `onull`.
  - **Caller-side calling convention.** `__or` arguments to a
    function call route through `O1..O4` per Vol VII §2.1; the
    caller emits `omov o1, oM; jal callee`.
  - **Callee-side parameters.** Function bodies whose parameters
    are `__or`-qualified can use them inside the body without
    explicit register binding; the parameter symbol is bound
    directly to the OR slot the value arrived in.
- Inline asm with `__or` operand bindings, e.g.
  `asm("olw %0, 0(%1)" : "=r"(out) : "r"(or_var))` — covers OL,
  OS, OEQ, OISN, OLEN, OTAG, OHOME, OCAP, and direct firmware
  `call #N` invocations from C. Convenience macros are in
  [`orisc.h`](orisc.h).

What's still missing or stubbed (most documented further in the
[`TODO`](TODO) file):

- **`__or` returns in O1.** Same root cause as the original
  callee-side issue — pcc's `cftnod` return temp gets a CLASSA
  class before FORCE runs.
- **OL/OS as native pcc patterns** (today via inline asm).
- **Capability-invariant type checks**: forbid casts between
  `__or` and integer pointers, forbid address-of of `__or`
  lvalues.
- **Floating point, bit fields, struct copies, longlong
  arithmetic beyond basic move/load**, and the privileged
  instructions (`SEND` / `OFENCE` / `OREFLD` / `OREFST` as
  builtins). None of the current demos exercise these.

## Build

From a build directory of your choice (`/tmp/pcc-build` is the
convention used by `examples/cc/run_c.sh`):

    mkdir -p /tmp/pcc-build && cd /tmp/pcc-build
    "$OBJECTRISC_ROOT"/tools/cc/configure --target=orisc-unknown-none
    make

(`$OBJECTRISC_ROOT` here is wherever you cloned the repo;
`configure` needs an absolute path because it's an out-of-tree
build.)

The output binaries are `cc/cpp/orisc-unknown-none-cpp` and
`cc/ccom/orisc-unknown-none-ccom`. The compiler is invoked through
the wrapper [`examples/cc/run_c.sh`](../../../../examples/cc/run_c.sh),
which drives the full pipeline (cpp → ccom → asmorisc → simorisc)
and links each program against
[`crt0.s`](crt0.s), [`console_io.s`](console_io.s), and
[`examples/cc/lib.c`](../../../../examples/cc/lib.c).

## Try it

From the repo root:

    examples/cc/run_c.sh examples/cc/hello.c           # Hello, world!
    examples/cc/run_c.sh examples/cc/factab.c          # 1!..7! table
    examples/cc/run_c.sh examples/cc/primes.c          # primes < 50
    examples/cc/run_c.sh examples/cc/fizzbuzz.c        # 1..20 fizzbuzz
    examples/cc/run_c.sh examples/cc/pascal.c          # Pascal's triangle
    examples/cc/run_c.sh examples/cc/hello_or.c        # __or via __asm__ binding
    examples/cc/run_c.sh examples/cc/print_or.c        # __or + inline asm
    examples/cc/run_c.sh examples/cc/inspect.c         # OEQ/OISN/OLEN/OTAG/OHOME/OCAP
    examples/cc/run_c.sh examples/cc/print_clean.c     # __or via calling convention
    examples/cc/run_c.sh examples/cc/print_via_or_arg.c # callee takes __or arg
    examples/cc/run_c.sh examples/cc/or_callee_inspect.c # OISN/OLEN inside __or callee

## Files

| File             | What it owns                                                  |
|------------------|---------------------------------------------------------------|
| `macdefs.h`      | Type sizes, register-file declarations (CLASSA/B/C), PCLASS hook for OREF |
| `code.c`         | Function entry/exit (`bfcode`/`efcode`), segment directives, call lowering (`moveargs`) |
| `local.c`        | `clocal` lowering (FORCE → R2/O1, AUTO/PARAM → STREF(FP, name)), char-init coalescing for `.ascii` |
| `local2.c`       | Backend hooks: `rnames[]`, prologue/epilogue, register-class helpers |
| `table.c`        | Instruction-selection patterns (~600 lines, 60+ patterns) including the `__or` ones |
| `order.c`        | Operand-ordering hints, `notoff`/`offstar`/`livecall` |
| `orisc.h`        | C-side header — `oref_eq`/`oref_isnull`/`oref_len`/`oref_tag`/`oref_home`/`oref_caps` and OL/OS macros |
| `crt0.s`         | Startup: `.entry _start`, calls main, TaskExits with R2 as the exit code |
| `console_io.s`   | C-callable bridge to firmware ConsoleWrite (legacy heuristic + clean `__or` variant) |
| `TODO`           | Per-phase work log: what's done, what's blocked, what's next  |

## Where to start reading

- The [TODO](TODO) file has detailed per-phase notes including the
  reverted approaches (regs.c hook, cerror gates) and the
  forensic record of what was tried.
- Phase 12 of [`HISTORY.md`](../../../../HISTORY.md) covers the
  whole port at a higher level: vendoring pcc, the contract-first
  backend buildup, the `__or` qualifier in three subphases.
- For the broader Object RISC architecture and why the OR file is
  a separate register class at all, see Volumes I–VII in the repo
  root (`OVERVIEW.md`, `INSTRUCTION_SET.md`, etc.).
