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

---

# Addendum — Since the Initial Commit

The work that followed the initial commit closed the two open
consequences flagged above, took the simulator from a single Python
process to a small distributed system of cooperating processes, and
fed the lessons learned back into the architecture volumes themselves.

## Phase 8 — Loadable modules and OR-typed storage

Two architecturally distinct gaps were addressed in one stretch of
work, because each enables the other.

### Loadable modules

The simulator's `InstallHandler` originally required the handler code
to live in the boot text — there was no path by which a fresh code
object could become an executable handler at runtime. The fix had
three parts:

1. `InstallHandler` was generalized to accept any code object that is
   currently mapped executable on the local CPU. Whether that mapping
   was set up at boot or by `MapObject` at runtime is irrelevant.
2. `MapObject` was generalized accordingly, accepting any reference
   with the X cap and threading the resulting VA→object mapping into
   the same translation list that boot mappings use.
3. The five-step pattern *allocate → write → derive-without-W →
   map → install handler* (now Volume VII §3.4) was given as a
   first-class idiom.

[`tools/sim/tests/validation/12_modules/`](tools/sim/tests/validation/12_modules)
— seven tests covering allocation, copy-and-seal, map, install, and
the full end-to-end pattern (`07_loadable_handler.s`) where one task
loads a module from a region of bytes and successfully invokes it
through `SEND`.

### OR-typed storage and the OBJSTORE flag

The capability invariant prohibits ORs from being stored to or loaded
from byte-typed memory. The architecture's original answer — the
firmware primitives `ORegSpill`/`ORegRestore` — paid a `CALL` per
spill and was awkward for arbitrarily indexed spill slots.

The cleaner answer turned out to be a flag on the descriptor itself:

- A new descriptor flag, `OBJSTORE` (bit 6), marks an object as
  OR-typed. Integer accesses (`OL*`/`OS*`) on an OBJSTORE object
  trap; only the new `OREFLD`/`OREFST` instructions may access it.
- `OREFLD` and `OREFST` (major opcodes 0x36 and 0x37) load and store
  a single 64-bit reference at an 8-byte-aligned offset. They behave
  like ordinary OR-relative loads/stores: full descriptor-cache
  participation, full bounds and capability checking, full delay-slot
  semantics.
- A new firmware primitive `0x106 ObjAllocStore` allocates an
  OBJSTORE-flagged object whose length must be a multiple of 8. The
  flag is fixed at allocation time and cannot be flipped.

The capability invariant is preserved because the bits never become
observable as integers; the OR file ←→ OBJSTORE storage path moves
references through dedicated instructions that the architecture
verifies the same way it verifies any other OR operation.

In passing, the `OFENCE` instruction — also in the OBJECT funct
group — got promoted from "implicit at every primitive boundary" to
an explicit instruction. It orders all prior OR-mediated accesses
before all subsequent ones, which matters when the same object is
visible through both an OR and a mapped page (a situation that arises
naturally with loadable modules).

[`tools/sim/tests/validation/12_modules/`](tools/sim/tests/validation/12_modules)
also covers `OREFLD`/`OREFST` and `ObjAllocStore`, including the
trap on integer access to an OBJSTORE object and the symmetric trap
on `OREFLD` against a byte-typed object.

## Phase 9 — Receive queues, link boot, and an embarrassment

### Receive queues

The Vol VII RPC pattern from the initial commit (caller allocates a
reply object, derives a send-only cap, polls for the response) had a
correctness problem in the dispatch model: receiving the reply on the
caller's side meant *spawning a new handler task* on the reply
object, which then needed to communicate the result back to the
original caller through some shared channel. The handler-spawning
dispatch model wasn't the right fit for "I am the recipient and I am
already a task waiting for you."

The architectural answer was a per-object receive queue, attached
through `0x203 ReceiveQueueAttach` and polled through `0x204
ReceiveQueuePoll`. An object with a queue attached buffers incoming
SENDs into the queue rather than dispatching a handler; a task that
holds the V cap on the object can poll the queue (with a timeout)
and receive the next message into its own register state, blocking
in the scheduler until something arrives.

This is the canonical implementation of the RPC reply path now
described in Vol VII §4.3, and it is what
[`tools/sim/tests/validation/13_queues/06_rpc_round_trip.s`](tools/sim/tests/validation/13_queues/06_rpc_round_trip.s)
exercises end-to-end across two processors.

### Link boot

With loadable modules in place, the Transputer-style link-boot path
became implementable: one CPU holds a service object whose handler
accepts a code-image payload, allocates an OBJSTORE-style data
buffer, copies the image in, derives an executable reference, maps
it, and installs it as a handler on a fresh service object exposed
back to the booting CPU. The boot CPU never carries the loaded
program in its own text segment.

[`tools/sim/tests/validation/11_multicpu/11_link_boot.s`](tools/sim/tests/validation/11_multicpu/11_link_boot.s)
exercises this pattern, with one CPU shipping a code image to a
second CPU that loads it, runs it, and reports back. The test
deliberately does *not* share boot text between the two CPUs; the
booted code arrives entirely over the wire.

### A primitive-number collision

While reviewing the manual against the as-built simulator, we
discovered that we had assigned `ObjAllocStore` the primitive number
`0x102` — which Vol VI had already given to `ObjRevoke`. The fix was
to relocate `ObjAllocStore` to `0x106` (the next free slot in the
Memory Management range). Simulator dispatch, four validation tests,
the integration contract, and the simulator's README were updated;
all 113 validation tests still passed afterward. The episode was a
useful reminder that the integration contract is part of the
architecture surface and must be treated as such.

## Phase 10 — Wire-level crossbar and multi-process simulation

The crossbar in the initial commit was a Python dictionary mapping
destination port to a queue of pending messages. It exercised the
architectural model but bypassed the wire format Vol IV had
specified. Two pieces of work changed this.

### The wire format goes live

A `Crossbar` abstract interface was introduced with two
implementations:

- `InProcessCrossbar` — the original direct-dispatch model, kept for
  speed and for in-the-loop debugging.
- `SocketCrossbar` — a real wire-level crossbar that serializes every
  cross-CPU message into a `SEND_DELIVER`, `OBJ_READ_REQ/RESP`, or
  `OBJ_WRITE_REQ/RESP` packet per Vol IV §3 (8-byte header, payload
  words, trailing XOR checksum, all big-endian) and ships it over a
  UNIX domain socket.

`pack_packet` / `unpack_packet` and the per-message-type builders
became first-class infrastructure with unit-test coverage in
[`tools/sim/tests/test_wire_format.py`](tools/sim/tests/test_wire_format.py).
The `--trace` flag was extended to print packet hex dumps alongside
the instruction trace.

The architecturally important consequence: a remote `OL`/`OS` now
genuinely blocks the issuing CPU at the scheduler level, waiting for
an `OBJ_READ_RESP` or `OBJ_WRITE_RESP` packet to arrive from the
home CPU's autonomous "memory controller." There is no synchronous
shortcut.

### Multiple processes

Once the wire format was real, splitting CPUs into separate processes
became a small additional step. The result was three new tools and a
new mode for the simulator:

- `oriscbar` — a standalone crossbar daemon. Listens on a UNIX
  domain socket; routes packets between connected ports; survives
  CPUs and devices coming and going.
- `simorisc --bar <socket>` — runs a single CPU as a client process
  of an external `oriscbar`. Multiple `simorisc` processes connected
  to the same `oriscbar` form a multi-CPU system distributed across
  Unix processes.
- `oriscterm` — a graphical terminal device, a Tk window with a
  monospace text widget and a virtual keyboard. Connects to
  `oriscbar` as a port; exposes a service object that accepts
  console-write SENDs and produces input events.
- `oriscrun` — the launcher. Spawns the crossbar, the requested
  device processes, and the requested CPU processes; threads the
  right service references into each CPU's initial state; forwards
  each child's output to a single console with per-process prefixes.

The hello-on-terminal demo reduces to one command:

```
$ tools/oriscrun \
    --terminal pid=16 \
    --cpu pid=0:program=examples/hello_terminal.orx,service=16=1@9
```

The hello world appears in the Tk window, having traveled from a
CPU process, through the crossbar daemon, into the terminal process,
across a wire format that matches the architecture spec to the byte.

## Phase 11 — The manual revision

The work above produced enough learning to warrant updating the
architecture volumes themselves. The volumes maintain their 1986
voice (the architecture group writing for an architecture audience),
but the content reflects the architectural commitments validated by
the implementation:

- **Vol II** gained an `OFENCE` row in §8 and a new §10 documenting
  `OREFLD`/`OREFST`. The opcode map (Appendix A) lists 0x36 and 0x37
  as live; the major-opcode tally rose from thirty-three to
  thirty-five.
- **Vol III** gained the `OBJSTORE` flag in the descriptor flags
  table (§3.3) and a new §5.4 explaining OR-typed storage as the
  proper mechanism for compiler spill, reference-bearing heap
  structures, and handler state via service objects.
- **Vol VI** gained `0x106 ObjAllocStore` (§5.1.1).
- **Vol VII**'s OR-discipline section (§2.4) was rewritten: the OR
  spill problem is now described as solved by `OREFLD`/`OREFST`
  against an `ObjAllocStore`'d backing object, with the
  `ORegSpill`/`ORegRestore` primitives demoted to legacy. A new §3.4
  covers the loadable-modules pattern; §4.3 was tightened to
  describe the receive-queue RPC pattern as the canonical
  construction.

The integration contract was re-checked against the revised volumes
and the simulator's source for consistency. Both PDF builds (Computer
Modern, Palatino) were regenerated.

## Phase 12 — A C compiler

Up to Phase 11, every Object RISC program was hand-written
assembly. The architecture had survived hello-world, parallel π,
the wire-format crossbar, and a graphical terminal — but each of
those was someone (or some agent) writing the instructions
directly. The next question was whether a real C compiler could
target the architecture, and what it would discover when it tried.

### The macro assembler

A small precondition: pcc emits assembly that uses `.set`
directives and label arithmetic in expression contexts (e.g.
`addiu sp, sp, F-4` where `F` is a frame-size symbol). The
original asmorisc accepted only literal integer immediates, so
the first work was a small expression evaluator: `.set NAME, EXPR`
defines a symbol; immediates and offsets in instruction operands
accept the same expression grammar (`+`, `-`, parens, label
references). The change touched ~150 lines of asmorisc and one
test category in the validation suite.

### Vendoring pcc

The plausible candidates for a 1986-era compiler were the BSD
*Portable C Compiler* (pcc) family. After tracing the lineage
back through Anders Magnusson's modern revival
(`pcc.ludd.ltu.se`), we vendored the
[`pmer/pcc`](https://github.com/pmer/pcc) snapshot into
`tools/cc/`. pcc is BSD-licensed, written in C90, builds with
`./configure && make`, and — crucially — has a clean separation
between the language front end (`cc/ccom`) and the per-target
backend (`arch/$TARGET/`). Adding orisc was a matter of writing
six files: `macdefs.h`, `code.c`, `local.c`, `local2.c`,
`order.c`, and `table.c`, plus a `crt0.s` and a small
`console_io.s` bridge.

### The backend, contract-first

The first cut compiled `int main(void) { return 7; }` to nothing
useful; the second cut compiled it to a function that stored 7
to a local but never moved it to R2. Everything from there was
a series of small discoveries about what pcc expected from a
backend:

- **FORCE lowering.** pcc's `clocal` hook receives a `FORCE` node
  for every `return`; the backend's job is to lower it to the
  ABI's return-register ASSIGN. Plain integer returns went to
  `R2` (the architectural retval) via a one-line addition.
- **AUTO/PARAM rewriting.** `NAME` nodes referencing locals
  arrive without addressing information; `clocal` rewrites them
  to `STREF(FP, name)` so `local2.c` can emit FP-relative
  loads/stores. Without this, every local lookup tried to
  resolve as a global symbol.
- **Comparison operators.** pcc's `hopcode` table didn't have
  the comparison operators; we added EQ/NE/LE/LT/GE/GT and their
  unsigned cousins, each emitting `beqz`/`bnez`/`blez`/`bltz`/
  `bgez`/`bgtz` against zero (with the comparison materialized
  into a register first by other patterns).
- **The post-call SP restore.** pcc's `zzzcode` hook is the
  catch-all for backend-specific assembly emission keyed by a
  letter; `'C'` was the slot we used for the post-CALL `addiu
  sp, sp, N` that releases the outgoing-arg spill area.

By the end of the first round the pipeline survived hello, a
factorial table, primes under 50, FizzBuzz, Pascal's triangle,
and an `inspect` demo that read OLEN/OTAG/OHOME/OCAP through
extended inline asm. Output assembly is correct but verbose;
the dead-store / spurious-save pattern visible in the early
output was tamed by withdrawing the callee-preserved GPRs
(R16..R23) from pcc's allocation pool, which costs theoretical
register pressure relief but eliminated 16 dead instructions
per function. (When we have programs that genuinely need a
wider pool, the optimizer pass will need a real elide-dead-saves
upgrade; nothing in the demo set hits the limit.)

### Char-init coalescing for `.ascii`

A `const char hello[] = "Hello, world!\n"` declaration emitted
one `.byte N` directive per character — fifteen lines for one
string. A small change in `local.c::ninval` accumulates
consecutive ICON-typed char inits and emits them as a single
`.ascii "..."` directive at section / symbol boundaries.
~15× reduction in object-file size for any C program with
string literals in static storage.

### The `__or` qualifier

The hard part. C has no native concept of a separate register
file for capability-bearing pointers; everything to do with the
OR file (CLASSC in pcc's allocator) had to be coaxed in through
a new type qualifier. Three subphases over multiple commits.

**Phase 1 — Parse-only foundation.** Added `OREF = 0x80` to the
qualifier-bit set in `mip/manifest.h` (alongside CON and VOL),
registered `__or` as a `C_QUALIFIER` keyword in `cc/ccom/scan.l`,
and extended `arch/orisc/macdefs.h`'s `PCLASS` macro to route
OREF-qualified nodes — and, separately, raw REG nodes whose
physical reg number falls in the OR-file range (48..63) — to
CLASSC. This last part is the bridge that makes the explicit
`register __or T *p __asm__("oN")` syntax work: pcc's
`__asm__` binding lands the value in CLASSC without flowing
the qualifier, and `PCLASS` recognizes the register number
even when n_qual is missing.

**Phase 2a — Explicit-binding ops.** Three new patterns in
`table.c` covered the common ASSIGN cases: SCREG↔SCREG → `omov`;
OPLTYPE INCREG SZERO → `onull`; OPLTYPE INCREG SCREG → `omov`.
Combined with extended inline asm
(`asm("olw %0, 0(%1)" : "=r"(out) : "r"(or_var))`), this is
enough to write a hello-world that calls firmware ConsoleWrite
directly from C: the OR moves are real C statements, only the
firmware-call sequence remains inline asm.

**Phase 2b — Caller-side calling convention.** `moveargs` in
`code.c` now routes `__or`-qualified call arguments to O1..O4
per Vol VII §2.1 and `cerror`s on overflow (spilling an OR
to byte memory would violate the capability invariant). With
the syntax `void *__or` (qualifier *after* the `*`, so it
qualifies the pointer rather than the pointee), C calls like
`orisc_console_write(o3_data, 0, 14)` lower to clean
`omov o1, o3; ...; jal orisc_console_write`. The
`print_clean.c` demo is the canonical example.

**Phase 2c — Callee-side parameters.** The natural way for pcc
to receive a parameter is to lift it through a `tempnode`,
which inherits CLASSA from `gclass(n_type)` and so can't
coalesce with the CLASSC source — `Coalesce: src class 3, dst
class 1`. The fix is to bypass the tempnode entirely: in
`bfcode`, set `sp->sclass = REGISTER; sp->sflags |= SINREG;
sp->soffset = O[opr]` for each OREF-qualified parameter,
mirroring exactly what the explicit `__asm__("oN")` syntax does.
Body NAME references then resolve straight to the OR file.
The cross-build wrinkle: `SINREG` is in `cc/ccom/pass1.h` but
not `cc/cxxcom/pass1.h`; an alias `#define SINREG SLOCAL1` to
the latter (where bit 010000 was unused) lets `code.c` build
under both. cxxcom never reads SINREG, so the alias is a
harmless no-op. The `print_via_or_arg.c` and `or_callee_inspect.c`
demos exercise the path.

What remains: `__or` returns in O1 (same root cause as the
tempnode-class blindness — `cftnod` is allocated CLASSA before
FORCE runs), OL/OS as native pcc patterns instead of via inline
asm, and the capability-invariant type checks (forbid casts
between `__or` and integer pointers, forbid address-of of `__or`
lvalues). All documented in `tools/cc/arch/orisc/TODO`.

### Status

Eleven C demos build and run end-to-end through the pipeline
(`run_c.sh` in `examples/cc/`): hello world, factorial table,
primes, FizzBuzz, Pascal's triangle, `__or` direct binding,
`__or` + inline asm, `__or` introspection, the caller-side
`__or` calling convention, an `__or` arg forwarded through a
pure-C function, and an `__or` arg used inside the body for
OISN/OLEN inspection. The architecture genuinely round-trips
ordinary C through a 1986-era compiler stack — and the
non-trivial bits (the OR file, the caller/callee `__or`
convention) round-trip too.

## Phase 13 — A generic link-boot loader

Phase 8's `11_link_boot.s` validation test proved one CPU could
ship code to another and have it run. But that test hardcoded
*everything*: the receiver knew the exact byte size of the module,
the source data ref's layout, and the dispatch sequence. It was
proof of the pattern, not a reusable mechanism.

Phase 13 is the generalization: a self-contained `.orx` you can
boot any extra CPU with, one that doesn't know in advance what code
it's supposed to run. The loader announces itself to a "boot
master" service on the crossbar and waits for a SEND carrying a
code reference, length, and entry offset; on receipt it copies the
code into a fresh local code object, maps it executable, and JRs
into the loaded module. The whole thing fits in a 1.6 KB `.orx`.

### Discovery

The user-suggested addition that made the loader actually useful.
Without an announce phase, the master would need to know the
loader's pid and service-object index out of band — fragile, and
defeating the point of a generic loader. With an announce, the
loader takes the initiative: it derives an `R|S` view of its own
self-service (just enough capability for the master to SEND it
back), SENDs that derived ref to whatever lives in `O5` (by
simorisc convention, the lowest-PID other CPU's service), and then
polls a receive queue for the master's reply. The master never has
to know who it'll be talking to until the announce arrives.

### The unrolled-OLW copy

The architecturally interesting bit. Two of Object RISC's design
constraints collide here:

- `MapObject` requires the target's home to be the calling CPU
  (capability scoping; you can't claim a remote CPU's address space).
- `OLW`'s offset is encoded in the instruction (16-bit signed
  immediate, no register-indexed form).

Together: the loader can't map the source to read it via ordinary
`lw`, and it can't write a register-driven `OLW` loop either. Every
word it pulls from the master needs a different hardcoded offset.

The pragmatic fix is to unroll. The loader contains 64 stanzas,
each `olw r2, OFF(o6); sw r2, OFF(r19); addiu r1, r1, 1; beq r1,
r20, copy_done; nop`, with the early-exit branch firing as soon
as enough words have been copied. The `MAX_WORDS = 64` constant
caps modules at 256 bytes; bumping it grows the loader's text
linearly. Generated by `examples/linkboot/gen_linkboot.py` so the
unrolled section stays readable in source.

### What didn't work

Several paths were considered and rejected:

- **MapObject the source remotely.** The simulator (and the spec)
  refuse with `EREMOTE`. The capability invariant says you can't
  install page-table entries for storage you don't own.
- **A new firmware primitive (`ObjCopy`).** Cleanest by far — one
  call, arbitrary length, copies through the wire transparently.
  Rejected as out of scope: it's a real architectural addition
  that should be discussed and added deliberately, not as a
  side-effect of building one demo.
- **SEND-streaming.** Master breaks the module into 16-byte chunks
  (4 ints per SEND), loader assembles. Architecturally pure but
  much more complex on both sides; the unrolled copy ships
  something working in a fraction of the code.
- **Self-modifying code that patches OLW offsets at runtime.**
  Requires a writable mapping of the loader's text, which would
  itself violate the X-not-W discipline the architecture enforces
  (and would make the loader code dynamically recompiled per
  instruction, which is far worse than just unrolling).

### The remote-`ConsoleWrite` gotcha

A discovery during testing. The first version of the demo had the
loaded module read its message from the data ref the master passed
through. This worked under `--processors 2` (one process holding
both CPUs' descriptor tables) but produced 8 NUL bytes under
`--connect` (each `simorisc` process holds only its own
descriptors; `ConsoleWrite` walks them directly rather than going
through `OBJ_READ_REQ`).

The fix: have the loader pass the loaded module its own loaded
code reference in `O1` before JR. The module's message lives just
past its code in the same allocation; `ConsoleWrite` from `O1`
reads locally and works in both modes. The diagnosis is in
[`examples/linkboot/README.md`](examples/linkboot/README.md);
the simulator's `ConsoleWrite` is the right place to grow real
remote-aware semantics, but for now the workaround is sufficient.

### Status

The loader works in both single-process (`--processors 2`) and
multi-process (`oriscrun`) modes; a validation test
(`11_multicpu/13_linkboot_loader.s`) exercises the single-process
path. The whole thing is generated from one Python file
(`gen_linkboot.py`) so the four output files (loader, master,
combined demo, validation test) stay in lockstep.

### linkbootd — the Python-side boot server

Once the asm master worked, an obvious follow-up: replace the
asm master with a Python program that connects to the crossbar
directly and answers boot requests. `oriscterm` was the model —
both are non-CPU participants that present themselves to the
crossbar as Volume IV §3 ports; the only difference is what
each does with incoming SENDs.

[`tools/devices/linkbootd`](tools/devices/linkbootd) hosts a
boot image (extracted from a `.orx` text section, or read raw),
synthesizes a `(home=our_pid, index=0x100, generation=1, caps=R|S|V|C)`
descriptor for it, and loops on the socket waiting for two
packet kinds:

- **`SEND_DELIVER`** — treated as an announce. Reply with another
  `SEND_DELIVER` aimed at the loader's R|S self-ref carrying
  the image_ref, length, and entry offset.
- **`OBJ_READ_REQ`** — generated by the loader's OLW copy stage.
  Validate the reference (home/index/generation/caps), bounds-
  check, reply with `OBJ_READ_RESP` carrying the bytes.

That's the whole protocol — about 250 lines of Python, mostly
wire-format helpers shared with `oriscterm` and `simorisc`. The
asm master had the same logic but spread across .s instructions
and a queue/poll dance; the Python version reads cleanly.

The interesting consequence: a single `linkbootd` can serve any
number of loader CPUs from one image. The
[`run_python_master.sh`](examples/linkboot/run_python_master.sh)
runner takes `NCPUS=N` and spins up that many loaders, all
booting from the same `linkbootd` over the wire. It works at
N=8 cleanly (8/8 CPUs print "Booted!"), which is more
demonstration than necessary but pleasant to watch — eight
independent processes, one host-side server, one shared
boot image, eight independent OLW copy passes interleaved on
the socket.

## Where things stand now

- 7 architecture volumes plus the integration contract, revised to
  reflect everything learned in implementation.
- An assembler (with `.set` and label arithmetic) and a simulator
  (~3,000 lines of Python, stdlib only) with single-CPU,
  in-process multi-CPU, and multi-process modes.
- A validation suite spanning thirteen categories (integer, logical,
  memory, control, oreg, omem, firmware, traps, call, golden,
  multi-CPU including link boot, loadable modules, receive queues),
  all passing.
- A wire-level crossbar daemon (`oriscbar`), separate-process CPU
  runtime (`simorisc --bar`), and graphical terminal device
  (`oriscterm`) demonstrating the architecture's communication model
  as a small distributed system.
- A vendored pcc with an Object RISC backend that compiles real C
  programs end-to-end, including a working `__or` storage-class
  qualifier on parameters and register-bound variables (caller and
  callee sides of the calling convention both wired).
- A generic link-boot loader that lets you spin up extra CPUs whose
  code is decided at runtime — announce on the crossbar, receive a
  module by SEND, map and JR.

The two open consequences from the initial commit are both closed:

1. *Object references cannot be stored to or loaded from integer
   memory* — addressed by the `OBJSTORE` flag and `OREFLD`/`OREFST`,
   which preserve the capability invariant by routing references
   through dedicated, statically-checkable instructions.
2. *Handler code objects must equal the boot text* — addressed by
   generalizing `InstallHandler` and `MapObject` to accept any
   executably-mapped reference, enabling loadable modules, link
   boot, and ultimately the multi-process device model.

The asymmetry of the handler dispatch convention (`O1` overridden,
wire OR[0..2] → handler `O2..O4`, wire OR[3] → side-channel) remains
flagged for a future revision, as does the small set of privileged
instructions still stubbed in the simulator. Neither blocks any
present use of the architecture.
