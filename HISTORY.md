# Object RISC — Project History

This document traces the evolution of the Object RISC project from its
opening premise through the initial git commit. It complements the
formal architecture volumes (which document *what* the architecture
is) by recording *how* it came to be — the design decisions made,
the ones revised, and the conventions that emerged from working
through the consequences.

## The premise

The project began as a single prompt:

> Let's engage in a bit of computing alternate history. The year is
> 1986. You are in charge of designing a newfangled CPU architecture
> called Object RISC. To paraphrase some previous writing on Object
> RISC: it is a simple 32-bit load-store RISC design in keeping with
> the style of the era, but with an emphasis on connectivity to other
> Object RISC processors by way of a crossbar interconnect.

The premise contained the entire shape of the work. Everything that
followed unpacks two phrases: *"Object RISC"* (RISC-style minimalism
applied to first-class hardware objects) and *"crossbar
interconnect"* (multiprocessor as a primitive, not an afterthought).

## Phase 1 — Volume I, in three passes

### Pass 1: the bare overview

The first draft of [`OVERVIEW.md`](OVERVIEW.md) committed to the core
shape:
- 32-bit load-store RISC at the MIPS R2000 / SPARC v1 weight class
  (~110K transistors, 16–20 MHz, single-issue 5-stage pipeline)
- A separate object register file (16 ORs) holding 64-bit object
  references
- Hardware-enforced bounds and capability checks on object loads/stores
- A `SEND` instruction that ships messages across the crossbar

It deliberately excluded inheritance, dispatch, garbage collection,
floating-point, and virtual memory — the architecture was minimal in
silicon and pushed everything else outward.

### Pass 2: System Firmware enters the picture

> the Object RISC architecture also defines a virtual machine
> interface with various system primitives such as task management,
> memory management, communications, device and I/O interaction, etc.

This added a third architectural pillar alongside the CPU and the
crossbar: System Firmware. The architecture now defined not just an
ISA but a *virtual machine interface* — a fixed primitive set invoked
through a single trap-style `CALL` instruction, with the firmware
implementing those primitives (and acting as hypervisor in
multi-tenant configurations).

This stratification absorbed the policy decisions (scheduling,
mapping, device drivers) that had been hand-waved in pass 1. The
ISA could stay smaller because firmware took over.

### Pass 3: the Apollo correction

The original Vol I committed implicitly to AS/400-style single-level
store: one global physical address space, no virtual address
translation. After Vol I went out, the response came back:

> You can't really do this in 32 bits with a single flat address space
> and scale. You have to think Apollo-style and *map* objects to
> virtual address spaces at runtime.

This was the most consequential revision. Apollo Computer's Domain
system held the answer: a global object namespace much larger than
any one task's address space, with per-task VASes populated by mapping
from that namespace at runtime. With this model, the architecture got:

- A real MMU (TLB, page-table walker, page-fault trap)
- Per-task 32-bit virtual address spaces
- Two distinct object-access paths: through the object register file
  (descriptor-cache mediated, can be remote) and through a virtual
  address mapped by firmware (MMU-mediated, local only)
- `MapObject` as a firmware primitive

The "single physical address space shared across the system" line —
which Vol I had carried since the first draft and which Section 5
already implicitly contradicted — was finally retired. *The
namespace in which storage is named must be larger than the address
space in which any single task computes, and the bridge between the
two is built at runtime rather than designed into the address layout.*

## Phase 2 — Volumes II–VII

### Volume II: the instruction set

Pinning the ISA forced a hundred small decisions: register usage
(R0=zero, R31=link, O0=null, the calling convention), four
instruction formats (R/I/J/O), opcode allocation (MIPS-shaped through
0x2B, novel block at 0x30–0x3D), the OBJECT funct codes, the precise
exception model, the trap cause table.

Two non-obvious commitments worth flagging:

- **`CALL` has no delay slot.** Unlike every branch and jump, CALL is
  a direct trap into firmware; no delay-slot instruction runs before
  the primitive. Justified inline as "infrequent enough relative to
  ordinary control flow that the simplification is worth the
  exception."
- **Object register file as a separate class.** ORs cannot hold
  integers and integer registers cannot be dereferenced as objects.
  This is the single most consequential ISA decision; it makes the
  capability invariant statically enforceable.

### Volumes III–VI in one stretch

[`OBJECT_SYSTEM.md`](OBJECT_SYSTEM.md),
[`INTERCONNECT_PROTOCOL.md`](INTERCONNECT_PROTOCOL.md),
[`REFERENCE_IMPLEMENTATION.md`](REFERENCE_IMPLEMENTATION.md), and
[`SYSTEM_FIRMWARE_INTERFACE.md`](SYSTEM_FIRMWARE_INTERFACE.md) were
written in a single stretch because Vol III's reference layout
constrains Vol IV's wire format, which constrains Vol V's caches and
TLB sizing, which constrains Vol VI's primitive ABI. Each downstream
volume forced clarifications upstream.

Key decisions pinned in this stretch:

- **64-bit object reference layout**: 16 bits generation, 8 bits home
  processor, 24 bits local index, 8 bits cached effective caps, 8 bits
  reserved.
- **32-byte descriptor**: base, length, generation, type tag, max
  caps, flags, send handler ref, send handler offset.
- **Generation-counter coherence** instead of cross-CPU invalidation
  broadcasts. The descriptor cache compares the reference's
  generation to the cached descriptor's generation on every hit; a
  mismatch means the slot was reused, refetch.
- **Migration preserves references; revocation doesn't.** Migration
  uses a forwarding stub at the original home and never bumps the
  generation; old references continue to work via lazy redirection.
  Revocation bumps the generation and invalidates everything globally
  in one atomic step.
- **Wire format** with 64-bit headers, 32-bit packet words, trailing
  XOR checksum. Rotating-priority arbitration (single-cycle, fair
  within N−1). Credit-based flow control with 32-word input FIFOs.
- **Reference implementation** committed to OR-1000 (5-stage pipeline,
  4KB caches, 32-entry 4-way ODC, 32-entry FA TLB) and OR-XBAR-1
  (16-port crossbar, 80K transistors). Performance estimates
  comparable to MIPS R2000 single-CPU and INMOS T800 communication
  latency.

### Volume VII: programming practice

[`PROGRAMMING_PRACTICE.md`](PROGRAMMING_PRACTICE.md) is different in
shape from the others — discursive rather than purely formal. It
exists because writing the worked example forced design questions
the formal volumes had glossed over:

- **Object register spill.** ORs cannot be dumped to integer memory
  on pain of forging references (the bits would be observable and
  reconstructable). The ABI says ORs are preserved across calls *by
  convention*, with a firmware spill primitive as the slow path. This
  is a real consequence of the capability invariant.
- **The handler "self" convention.** `SEND_DELIVER` carries 4 OR
  payload slots, but the receiving handler needs a full-capability
  reference to itself. Resolution: the dispatched handler's `O1` is
  firmware-overridden with a fresh self-ref, and `O2`–`O4` receive
  only the first three wire OR slots. The fourth wire slot is
  delivered through a per-task side-channel buffer for the rare
  handler that needs it. *Asymmetric and slightly awkward; documented
  as "slated for cleanup in a future revision."*
- **The worked example: a distributed counter service.** A Counter
  object, a SEND-based RPC interface (READ / INCREMENT / RESET), and
  a synchronous client wrapper that allocates a reply object,
  attaches a receive queue, derives a send-only cap, polls. Touches
  most of the moving parts. Includes a 9-step trace of a single READ
  across two processors.

## Phase 3 — Cross-volume alignment

After all seven volumes existed, several inconsistencies emerged
that needed back-propagation:

- **Vol II Section 14**: claimed "seven of the sixteen OBJECT funct
  codes" used; actually eight (off-by-one).
- **Vol II Section 4.4**: O-type load/store diagram summed to 31 bits
  instead of 32; the high bit of the rt' field was reserved but
  un-drawn. CONTRACT.md Section 5.9 already had the correct version.
- **Vol VII** had introduced `ORegSpill`/`ORegRestore` and
  `MessagePayloadOR4` as *firmware extensions outside the conformance
  requirement*; they were promoted to standard primitives in Vol VI
  Sections 5.3 and 6, with the conformance language updated to
  match.
- **The `‖` (U+2016) glyph** for bitwise concatenation in Vol II
  Section 2 turned out to be missing from Menlo (and most monospace
  fonts); replaced with `||`, which is more conventional anyway.

## Phase 4 — Typesetting

Two PDF builds emerged from successive iterations:

[`ObjectRISC.pdf`](ObjectRISC.pdf) — the Computer Modern build, 74
pages. Built via `pandoc → xelatex` with a custom `\@makechapterhead`
that suppresses the "Chapter N" prefix so each volume's chapter page
just shows its title centered.

[`ObjectRISC-Palatino.pdf`](ObjectRISC-Palatino.pdf) — the academic
press variant, 79 pages. Uses TeX Gyre Pagella (the open Palatino
clone, since macOS Palatino lacks arrow glyphs), with old-style
figures and slightly looser leading. Reads as a printed-book version
of the same material.

The [`build_pdf.py`](build_pdf.py) script handles the front-matter
stripping needed to combine seven independently-headed markdown files
into one coherent book with a single title page and unified TOC.

## Phase 5 — The toolchain, contract-first

With the architecture spec complete, the next question was whether it
was *executable*. The plan: build an assembler and a simulator, and
make a hello-world program survive the round trip.

### The contract

The risk in dispatching two parallel agents was that they would
disagree on a hundred small encoding choices. The remedy was a
contract document — [`CONTRACT.md`](CONTRACT.md) — that pinned
exactly what the architecture spec deliberately left to the
integrator: the on-disk binary format, the loader's choice of where
to map code/stack/data, the host-side semantics of the firmware
primitives the simulator implements directly. Plus comprehensive
encoding tables (major opcodes + SPECIAL/REGIMM/OBJECT funct codes +
all field layouts) — the architecture spec told *what each
instruction does* but not always *which 6 bits encode the opcode*.

### The two agents

Two parallel agents, both Python 3.10+ standard library only:
- **Assembler agent** under [`tools/asm/`](tools/asm/) — produced
  `asmorisc` with `--disasm`, `--hex` modes, examples, and tests.
- **Simulator agent** under [`tools/sim/`](tools/sim/) — produced
  `simorisc` with the full Vol II ISA, two firmware primitives
  (`TaskExit`, `ConsoleWrite`), and a hand-encoded hello world to
  prove its loader and decoder agreed with the contract independently
  of the assembler.

Both agents reported back with completed deliverables (the assembler
agent's tests and the simulator agent's hand-encoded hello world).
Neither could execute its own work due to sandbox constraints.

### The integration test

The moment of truth — assemble `hello.s` with `asmorisc`, load with
`simorisc`, observe stdout:

```
$ python3 tools/asm/asmorisc tools/asm/examples/hello.s -o /tmp/hello.orx
$ python3 tools/sim/simorisc /tmp/hello.orx
Hello, world!
```

It worked on the first attempt. The byte-diff between the assembler's
output and the simulator's hand-encoded version revealed exactly one
discrepancy — the trailing `nop` in the canonical source vs. the
prose claim of "six 4-byte instructions" in the contract — which the
assembler agent had already flagged as an ambiguity. Both binaries
ran correctly because the trailing `nop` was unreachable.

The contract did its job: two agents working from cold against the
same document produced compatible artifacts.

### Bug cleanup

- **Two assembler `.expected` files** were wrong. The agent had
  computed them by hand without being able to run its own assembler;
  the OEQ encoding for `oeq r10, o5, o6` was off, and one ASCII
  rendering had a misplaced character. Regenerated by feeding the
  source through the now-verified assembler.
- **The simulator's `tests/run-tests.sh`** had a shell-newline
  footgun: `$(printf '%s\n' "$x")` strips the very newline that
  `printf` was adding, so the test's hex comparison was unfair to the
  simulator. Fixed by capturing simulator stdout to a `mktemp` file
  and hexing it directly.

## Phase 6 — The validation suite

With the toolchain working, the next step was systematic coverage —
not just "hello world runs" but "every architecturally-observable
behaviour is tested."

[`tools/sim/tests/validation/`](tools/sim/tests/validation/) — 10
categories, 87 tests, all passing on first full run:

| Cat | Coverage |
|-----|----------|
| 01_integer  | ADD/ADDU/SUB/SUBU overflow vs wrap, ADDI sign-ext, SLT vs SLTU, MULT 64-bit, DIV (LO+HI), LUI+ORI, ANDI zero-ext |
| 02_logical  | AND/OR/XOR/NOR, SLL/SRL/SRA constant + variable, R0 hardwiring, low-5-bits-only |
| 03_memory   | LW/SW round-trip, sign vs zero extension, big-endian byte order, misalignment traps |
| 04_control  | All branches, J/JAL/JR/JALR, BLTZAL link-on-not-taken, branch-delay-slot-runs-when-taken |
| 05_oreg     | OMOV/ONULL/OEQ/OISN/OLEN/OTAG/OHOME/OCAP, null-trap behaviour |
| 06_omem     | OL*/OS* widths, bounds/cap/null traps |
| 07_firmware | ConsoleWrite happy path + EFAULT/EINVAL, TaskExit code + low-byte-truncation, ENOSYS for unknowns |
| 08_traps    | Precise EPC capture, reserved-instruction (raw `.word`), reserved OBJECT funct, unmapped-VA tlb-miss |
| 09_call     | No-delay-slot enforcement, PC+4 return, max imm26, chained CALLs |
| 10_golden   | Iterative factorial, count loop, conditional, partial-string print, print-in-a-loop |

The runner ([`tools/sim/tests/validation/runner.py`](tools/sim/tests/validation/runner.py))
parses `; @key: value` headers from each `.s`, assembles, runs the
simulator, and checks each `@expect-*` directive (exit code, stdout,
trap name, trap PC, stderr substring, max cycles, processor count).

A few subtle behaviours the suite caught implicitly:

- **Trap PC capture is exactly right** — tests pin specific PCs
  (0x1000C for an arithmetic-overflow, 0x10004 for a
  capability-violation) and they match.
- **The architectural delay slot really runs on taken branches** —
  the test sets `r4 = 5`, branches always-taken with `addiu r4, r4, 1`
  in the slot, exits 6.
- **CALL really has no delay slot** — the test puts an `addiu r5, r0,
  99` after a CALL to ConsoleWrite that depends on `R5 = 5`; if CALL
  had a delay slot, the bogus assignment would corrupt the count and
  the test would fail in a noisy way. It doesn't.

## Phase 7 — Multi-CPU and the crossbar

The validation suite gave coverage of single-CPU behaviour. The next
ambition was getting *two Object RISC processors talking to each
other* — exercising the SEND instruction and crossbar that had been
defined in spec but stubbed in the simulator.

### The proposal

Four design choices framed the work:

1. **Same `.orx` on every processor; role differs by `PROCID`.** The
   simulator loads one binary onto N CPUs; programs branch on a
   register holding their identifier.
2. **Pre-allocated service objects, refs handed out at boot.** The
   simulator allocates one service object per CPU automatically. Each
   CPU's initial state then carries refs to all of them — `O4` = my
   own (full caps), `O5+` = the others (R+S only).
3. **One task per processor; handler IS the next task.** No
   preemption, no scheduler. Simulator dispatches a registered
   handler when the CPU is idle and has a queued SEND.
4. **Round-robin scheduling, one instruction per CPU per tick.**
   Simple loop; exits when every CPU is idle and every inbox is
   empty.

Plus a shortcut explicitly held off for later: the Transputer-style
"link boot" bootstrap (one CPU sending firmware images to wake the
others) was deferred. For now the simulator IS the firmware.

### The simulator refactor

[`tools/sim/simorisc`](tools/sim/simorisc) gained a `System` class
wrapping N `CPU` instances. `CPU` gained a `pid` and a back-pointer
to the system. Object references picked up the `home = pid` field
they always had room for. `deref_obj` and `inspect_obj` learned to
route to the home CPU's descriptor table when the home wasn't local;
remote OL/OS works transparently with no special syntax.

Four new firmware primitives appeared: `0x100 ObjAlloc`, `0x101
ObjFree`, `0x103 ObjDerive`, `0x200 InstallHandler`. The `SEND`
instruction stopped being a stub and started actually delivering
messages into the home CPU's inbox.

### Two surprises

- **EPERM beats EREMOTE.** A test originally expected
  `InstallHandler` on a remote object to return `EREMOTE`. It returned
  `EPERM` — because remote service refs in `O5+` carry only R+S,
  the V-cap requirement fires before the home check. That's actually
  the right security outcome (you genuinely cannot install handlers
  remotely without V), so the test was renamed honestly.

- **The handler dispatch convention loses register state.** The
  initial implementation snapshotted register state at boot and
  restored it on handler dispatch. The first ping demo failed because
  the server's main set up `O15` with the data ref before exiting,
  but the boot snapshot didn't capture that — `O15` was 0 at handler
  dispatch. Fixed by snapshotting at *TaskExit* time so the handler
  inherits whatever environment main configured. Architecturally
  this is a fudge (a real handler is a fresh task that should reach
  shared state through objects, not registers); pragmatically it
  makes single-file demos far more ergonomic. Documented as such in
  CONTRACT §2.1.

### The result

[`tools/sim/tests/validation/11_multicpu/`](tools/sim/tests/validation/11_multicpu)
— 10 multi-CPU tests, all passing:

```
$ simorisc --processors 2 ping.orx
Hello from CPU 1!

$ simorisc --processors 2 ping-pong.orx
PONG
```

The ping-pong test is genuinely architectural: a full round-trip RPC
where both CPUs run handlers, both get dispatched after their main
TaskExits, and the reply capability is just an object reference
passed in the SEND payload (the architectural pattern from Vol VII
§4.3). The crossbar — a Python dict of pending messages, no real
wire format yet — turns out to be enough to demonstrate the
architectural model.

## Where things stand at the initial commit

- 7 architecture volumes plus the integration contract (~3,000 lines)
- 2 typeset PDFs (74 / 79 pages)
- An assembler and a simulator (~1,800 lines of Python, stdlib only)
- A validation suite of 97 tests across 11 categories, all passing
- Single-CPU and multi-CPU modes, six firmware primitives, working
  cross-CPU SEND with handler dispatch and reply capabilities

Two architectural consequences of the capability invariant remain
unresolved at this checkpoint, both flagged for the work that follows:

1. **Object references cannot be stored to or loaded from integer
   memory.** Without that, handler code objects must equal the boot
   text (the simulator's `InstallHandler` doesn't yet route through
   independent code mappings), and ORs cannot be properly spilled.
   The next work — loadable modules + `OREFLD`/`OREFST` with
   page-tagged storage — addresses this directly.
2. **The handler dispatch convention is asymmetric** (`O1` overridden,
   wire OR[0..2] → handler `O2..O4`, wire OR[3] → side-channel).
   Documented as "slated for cleanup in a future revision."

The Transputer-style link-boot bootstrap, where one processor sends
firmware images to wake the others over the crossbar, is the natural
target once loadable code objects exist. That gets us one step closer
to a real Object RISC system that boots itself.
