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

Pure Python 3.10+, standard library only. No build step.

| Tool                                         | Purpose                                              |
|----------------------------------------------|------------------------------------------------------|
| [`tools/asm/asmorisc`](tools/asm)            | Assembler — `.s` → `.orx`                            |
| [`tools/sim/simorisc`](tools/sim)            | Simulator — runs `.orx` binaries, single or multi-CPU |
| [`tools/sim/oriscbar`](tools/sim)            | Standalone wire-level crossbar daemon                |
| [`tools/devices/oriscterm`](tools/devices)   | Tk-based terminal device that connects to `oriscbar` |
| [`tools/oriscrun`](tools/oriscrun)           | Launcher: spawns crossbar + devices + CPU processes  |

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

Parallel π(N) across four CPUs, results streamed live to the
terminal. The coordinator partitions [2..2000] into quarters,
dispatches work via SEND with a derived reply cap, and prints each
worker's count and elapsed cycles as the reply arrives:

```sh
examples/run_parallel_primes.sh
# Parallel pi(2000) across 4 CPUs:
# CPU 0: 95 primes in 14759 cycles
# CPU 1: 73 primes in 21024 cycles
# CPU 2: 71 primes in 24978 cycles
# CPU 3: 64 primes in 27567 cycles
# Total: pi(2000) = 303
```

## Status

Object RISC is a designed and implemented exercise in alternate-
history computer architecture, not a production system. It is
complete enough to design programs against, write them in assembly,
and run them — single-CPU, multi-CPU, distributed across processes,
and talking to a graphical device — but it does not pretend to be
silicon. The reference implementation lives in Python.

## License

MIT — see [`LICENSE`](LICENSE).
