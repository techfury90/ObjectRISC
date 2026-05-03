# Object RISC

A 1986 alternate-history CPU architecture, designed in detail and
implemented enough to run.

Object RISC is a single-issue 32-bit load-store RISC in the style of
its era — MIPS R2000, SPARC v1, the family — with two
non-conventional commitments:

1. **A separate object register file.** Sixteen 64-bit object
   registers hold *references* — opaque, capability-bearing pointers
   to objects in a global namespace. Integer registers cannot be
   dereferenced as objects, and objects cannot be smuggled through
   integer memory. The capability invariant is enforced statically at
   the ISA level.
2. **Crossbar interconnect as a primitive.** A `SEND` instruction
   ships a message across an N-port crossbar to another processor's
   service object, where firmware dispatches it as a fresh task.
   Multiprocessor isn't an afterthought; it's in the ISA.

## What's in here

This repository contains the architecture documentation, an
assembler, a simulator, a wire-level crossbar daemon, a graphical
terminal device, and a validation suite.

### Architecture documentation (the seven volumes)

| Volume | File                                                     | Subject                              |
|--------|----------------------------------------------------------|--------------------------------------|
| I      | [`OVERVIEW.md`](OVERVIEW.md)                             | Architectural pitch and shape        |
| II     | [`INSTRUCTION_SET.md`](INSTRUCTION_SET.md)               | The ISA                              |
| III    | [`OBJECT_SYSTEM.md`](OBJECT_SYSTEM.md)                   | References, descriptors, capabilities |
| IV     | [`INTERCONNECT_PROTOCOL.md`](INTERCONNECT_PROTOCOL.md)   | Crossbar wire format                 |
| V      | [`REFERENCE_IMPLEMENTATION.md`](REFERENCE_IMPLEMENTATION.md) | OR-1000 / OR-XBAR-1 reference designs |
| VI     | [`SYSTEM_FIRMWARE_INTERFACE.md`](SYSTEM_FIRMWARE_INTERFACE.md) | Firmware primitive ABI               |
| VII    | [`PROGRAMMING_PRACTICE.md`](PROGRAMMING_PRACTICE.md)     | ABI, idioms, worked example          |

[`CONTRACT.md`](CONTRACT.md) pins what the architecture spec leaves
to the integrator: the `.orx` binary format, the loader's initial
task state, and the host-side semantics of the firmware primitives
the simulator implements directly.

[`HISTORY.md`](HISTORY.md) traces the project's evolution — three
passes on Volume I, the Apollo correction, the toolchain's
contract-first dispatch, the validation suite, multi-CPU, the
wire-level crossbar, multi-process, and the manual revision.

Combined PDFs are in the repo root:
[`ObjectRISC.pdf`](ObjectRISC.pdf) (Computer Modern, 72 pp) and
[`ObjectRISC-Palatino.pdf`](ObjectRISC-Palatino.pdf) (TeX Gyre
Pagella, 79 pp).

### Toolchain

The asm / sim / devices / launcher tools are pure Python 3.10+,
standard library only — no build step. The C compiler is a
vendored pcc and needs `./configure && make` once (see
[`tools/cc/arch/orisc/README.md`](tools/cc/arch/orisc/README.md)).

| Tool                                         | Purpose                                              |
|----------------------------------------------|------------------------------------------------------|
| [`tools/asm/asmorisc`](tools/asm)            | Assembler — `.s` → `.orx`, or `.s` → `.oro` with `-r` |
| [`tools/ld/orld`](tools/ld)                  | Linker — `.oro` and `.ora` inputs → `.orx`           |
| [`tools/ld/oar`](tools/ld)                   | Archiver — bundle `.oro` files into `.ora` archives  |
| [`tools/sim/simorisc`](tools/sim)            | Simulator — runs `.orx` binaries, single or multi-CPU |
| [`tools/sim/oriscbar`](tools/sim)            | Standalone wire-level crossbar daemon                |
| [`tools/devices/oriscterm`](tools/devices)   | Tk-based terminal device that connects to `oriscbar` |
| [`tools/devices/linkbootd`](tools/devices)   | Python-side link-boot server                         |
| [`tools/oriscrun`](tools/oriscrun)           | Launcher: spawns crossbar + devices + CPU processes  |
| [`tools/cc`](tools/cc/arch/orisc)            | Vendored pcc with an Object RISC backend (`arch/orisc/`) |
| [`tools/cc/lib`](tools/cc/lib)               | C library (liborisc.ora) — console I/O, string/memory primitives |

### Validation

114 tests across thirteen categories — integer, logical, memory,
control flow, object registers, object memory, firmware, traps,
CALL, golden programs, multi-CPU (with link boot), loadable modules,
and receive queues:

```sh
python3 tools/sim/tests/validation/runner.py
```

## Quick taste

Hello world:

```sh
tools/asm/asmorisc tools/asm/examples/hello.s -o /tmp/hello.orx
tools/sim/simorisc /tmp/hello.orx
# Hello, world!
```

Two CPUs talking over the crossbar (single-process):

```sh
tools/sim/simorisc --processors 2 --trace ping-pong.orx
```

Hello world on a graphical terminal connected to a real
wire-format crossbar in its own process:

```sh
examples/run_hello_terminal.sh
```

A generic link-boot loader: CPU 0 (master) ships an 8-instruction
module to a CPU running a content-free `linkboot.orx`, which
discovers the master via an announce SEND, copies the module
across the wire, maps it executable, and JRs in:

```sh
examples/linkboot/run.sh
# [xbar] oriscbar READY ...
# [cpu1] Booted!
```

See [`examples/linkboot/README.md`](examples/linkboot/README.md)
for the announce/boot protocol, the unrolled-OLW copy strategy,
and the limits.

C compiled by our in-tree pcc port. Build pcc once (`cd tools/cc &&
./configure --target=orisc-unknown-none && make` into a build dir
of your choice — `/tmp/pcc-build` is the default), then run any
`.c` through the pipeline:

```sh
examples/cc/run_c.sh examples/cc/hello.c     # Hello, world!
examples/cc/run_c.sh examples/cc/factab.c    # 1!..7! table
examples/cc/run_c.sh examples/cc/primes.c    # primes < 50
examples/cc/run_c.sh examples/cc/fizzbuzz.c  # 1..20 fizzbuzz
examples/cc/run_c.sh examples/cc/pascal.c    # Pascal's triangle
examples/cc/run_c.sh examples/cc/hello_or.c  # __or in C
examples/cc/run_c.sh examples/cc/print_or.c  # __or + inline asm
examples/cc/run_c.sh examples/cc/inspect.c   # OEQ/OISN/OLEN/OTAG/OHOME/OCAP
examples/cc/run_c.sh examples/cc/print_clean.c     # __or via calling convention
examples/cc/run_c.sh examples/cc/print_via_or_arg.c # callee takes __or arg
examples/cc/run_c.sh examples/cc/or_callee_inspect.c # OISN/OLEN inside __or callee
examples/cc/run_c.sh examples/cc/strings_demo.c    # exercises liborisc string fns
```

Two interactive C demos that use oriscterm's keyboard and
graphics capabilities — each opens a Tk window and waits for
keypresses (ESC to exit):

```sh
examples/cc/run_kbd_echo.sh    # echoes every keystroke (codepoint + mods)
examples/cc/run_paint.sh       # arrow + letter keys → vector drawing on canvas
```

The `hello_or.c` and `print_or.c` variants use the `__or`
qualifier to control the OR file directly from C:
`register __or void *p __asm__("o5")` binds a C variable to a
named Object Register slot. Assignments between `__or` variables
compile to `omov`; assigning `0` compiles to `onull`. Combined
with extended inline asm — `asm("olw %0, 0(%1)" : "=r"(out) :
"r"(or_var))` — you can read or write through OR pointers, invoke
firmware primitives via `call #N`, and otherwise treat OR slots
as first-class C lvalues.

The `__or` calling convention is wired both ways: callers pass
`__or` args in O1..O4 (`print_clean.c`), and pure-C callees
that take `__or` parameters can use them inside the body
without explicit register binding (`print_via_or_arg.c`,
`or_callee_inspect.c`). OL/OS-via-OR as native pcc patterns
(rather than via inline asm) and `__or` returns in O1 are
still pending — see
[`tools/cc/arch/orisc/README.md`](tools/cc/arch/orisc/README.md)
for the backend's status and TODO.

The demos collectively exercise: recursion, loops with
conditionals, if/else-if chains, stack-allocated arrays of int and
char, computed array indexing, pointer arithmetic, string literals,
integer arithmetic (add/sub/mul/div/mod/and/or/xor), bitwise
operations, multi-file linkage (programs link against the
[`liborisc.ora`](tools/cc/lib) C library for `print_str` /
`print_int` / `strlen` / `memcpy` / etc.), and external calls.

Each program links against `crt0.s` (provides `.entry _start`,
calls main, TaskExits with main's R2 as the exit code) and
`console_io.s` (a small bridge to firmware ConsoleWrite —
temporary until the C compiler grows the `__or` qualifier and
OR-file patterns). Both live in `tools/cc/arch/orisc/`.

Parallel π(N) across K+1 CPUs, results streamed live to the
terminal. The coordinator partitions [2..N] into K+1 equal ranges,
dispatches work to the workers via SEND with a derived reply cap,
computes its own range, and prints each worker's count and elapsed
cycles as the reply arrives:

```sh
examples/run_parallel_primes.sh               # defaults: N=2000, w=3
examples/run_parallel_primes.sh -N 5000 -w 5
examples/run_parallel_primes.sh -N 10000 -w 8

# Parallel pi(10000) across 9 CPUs:
# CPU 0: 186 primes in 41270 cycles
# CPU 1: 145 primes in 59893 cycles
# CPU 2: 139 primes in 71274 cycles
# CPU 3: 133 primes in 79765 cycles
# CPU 4: 129 primes in 86472 cycles
# CPU 5: 127 primes in 93499 cycles
# CPU 6: 126 primes in 99069 cycles
# CPU 7: 122 primes in 103186 cycles
# CPU 8: 122 primes in 108465 cycles
# Total: pi(10000) = 1229
```

`w` is capped at 10 (the program's worker-dispatch jump table has ten
entries — one per OR slot O6..O15 holding worker service refs at
boot). At larger `w` the result lines visibly arrive out of order
because the OS schedules each CPU process independently and they
finish at different rates.

## Status

Object RISC is a designed and implemented exercise in alternate-
history computer architecture, not a production system. It is
complete enough to design programs against, write them in assembly,
and run them — single-CPU, multi-CPU, distributed across processes,
and talking to a graphical device — but it does not pretend to be
silicon. The reference implementation lives in Python.

## License

MIT — see [`LICENSE`](LICENSE).
