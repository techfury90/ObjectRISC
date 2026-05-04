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

## Phase 14 — A real linker

The original asmorisc accepted multiple `.s` files but did the
"linking" by concatenation: read every input into one stream, build
a single global symbol table, resolve every reference at assembly
time, emit one `.orx`. That worked for the demos but had a tell —
`examples/cc/run_c.sh` had a `sed 's/L\([0-9]\+\)/LP\1/'` per-unit
mangling step to dodge `L1` colliding with `L1`, because pcc emits
unscoped local labels per translation unit. Time to do it properly.

### The split

`asmorisc -r` now emits a relocatable `.oro` object file (format
spec in [`CONTRACT.md`](CONTRACT.md) §1.1) with text, data, a
symbol table, and a relocation table. A new tool,
[`tools/ld/orld`](tools/ld), takes one or more `.oro` files,
merges sections, resolves cross-file globals, applies the
relocations, and writes the final `.orx`. Pcc's pipeline becomes
the natural shape: one `cpp + ccom + asmorisc -r` per translation
unit, then one `orld` over all the produced `.oro` files. The
sed-mangling hack is gone — local labels are scoped per `.oro`,
so `L1`-vs-`L1` collisions stop happening.

### Symbol scoping

The default rule: any label name matching `L\d+` is local;
everything else is global. That's the standard pcc convention and
keeps the migration friction-free — existing `.s` files don't need
changes. Two new directives let asm authors pin the binding
explicitly: `.global NAME[, NAME2, ...]` exports a symbol;
`.local NAME` keeps it private.

### Relocations

Six relocation types, deliberately minimal, each corresponding to
one site `asmorisc` was already deferring to pass 2:

| Code | Name | Patches | Where |
|------|------|---------|-------|
| `0x01` | `R_ORISC_ABS32` | full 32-bit word | `.word symbol` |
| `0x02` | `R_ORISC_HI16`  | low 16 of an instr | `lui rd, hi(label)` (first half of `la`) |
| `0x03` | `R_ORISC_LO16`  | low 16 of an instr | `ori rd, rd, lo(label)` (second half of `la`) |
| `0x04` | `R_ORISC_BRANCH16` | low 16 of an instr | rare cross-`.oro` branches |
| `0x05` | `R_ORISC_J26`     | low 26 of an instr | `j label` / `jal label` |

Two architectural details worth flagging. First, `HI16` and `LO16`
use the simple unsigned high/low split (no MIPS-style
`((addr + 0x8000) >> 16)` adjustment), because `lui + ori` zero-
extends the low half. Second, branches between two locally-defined
labels in the same `.oro` don't generate relocations at all —
their PC-relative displacements are invariant under section moves,
so `asmorisc -r` still resolves them at pass 2 and emits the
final encoded word. Only the absolute-address forms (`la`/`j`/
`jal`/`.word symbol`) need the linker's help.

### What didn't surface

A surprising amount of the design was just "do the obvious
ELF-shaped thing, but minimal." Big things that are NOT in this
linker because nothing in the toolchain needed them yet:

- **Archives (`.a`).** A `.a` is just a tar of `.o` files with an
  index; trivial to add when we have a libc to link against.
- **Weak symbols.** Useful for libc but not for the current pcc
  output.
- **Section types beyond `.text`/`.data`.** No `.rodata`, no
  `.bss` (we always allocate zero-init in the loader). Adding
  these is straightforward but uncalled for.
- **Common symbols.** Same reason.
- **Linker scripts.** `orld` always lays out `.text` then `.data`
  in input order, with the standard text/data VAs from `CONTRACT`
  §2. No `--script` knob.
- **Position-independent code.** Not in the architecture's
  vocabulary. Every reference resolves to an absolute VA.

### Tests

[`tools/ld/tests/`](tools/ld/tests) has six per-feature tests, each
a self-contained shell script that builds its own `.oro` files in a
tempdir, runs `orld`, and asserts on the simulator's exit code or
the linker's error message. Cases: cross-file `jal` (J26),
cross-file `la` (HI16+LO16 pair), per-file local-label scoping
(both files define `L1`), undefined-reference error, duplicate-
global error, explicit `.local` letting a non-`L\d+` name be
file-private. All six pass; the linker passes the C demo suite as
well — every one of the eleven `examples/cc/*.c` programs builds
end-to-end through the new pipeline.

## Phase 15 — Archives and a real libc

The linker landed in Phase 14, which made the next move obvious:
something to actually link *against*. Promote the ad-hoc
`print_str`/`print_int` helpers we'd been hand-compiling into a
proper C library, and design the archive format that keeps the
"only pay for what you use" property along the way.

### The `.ora` archive format

A direct cousin of `ar(1)` + `ranlib(1)`, rolled into one file.
[`CONTRACT.md`](CONTRACT.md) §1.2 pins the layout: a header, a
member directory (one entry per `.oro` inside), a symbol-to-member
index built up front, a string table, and the member blobs
concatenated at the end. The symbol index is what makes archives
useful: the linker scans its set of unresolved external references
and consults each `.ora`'s index to pull in just the members that
satisfy them. Members nothing references stay where they are.

[`tools/ld/oar`](tools/ld/oar) is the archiver, with the four
classic operations:

    oar c lib.ora a.oro b.oro c.oro    # create
    oar t lib.ora                      # list members
    oar s lib.ora                      # dump the symbol index
    oar x lib.ora a.oro                # extract one member

`oar c` rebuilds the index every time, so there's no `ranlib`-
shaped failure mode where the index drifts out of sync with the
members.

### Linker support

[`orld`](tools/ld/orld) gains a small "archive resolution" loop
that runs before the existing link logic. It walks the explicit
`.oro` inputs, computes the unresolved-externals set, and for each
unresolved symbol consults each `.ora`'s index to find a member
that defines it. Pulling a member in can introduce new unresolved
references (the member uses something *it* didn't define), so the
loop iterates until no progress. After that, the existing link
path runs unchanged: merge symbols, lay out sections, apply
relocations, write `.orx`.

The interesting subtlety: archive members that don't get pulled in
*can* contain symbols (or even relocations) that would conflict
with the explicit inputs. The
`08_archive_selective_inclusion` test deliberately puts a
duplicate `_start` in an unused archive member; if selective
inclusion ever broke, the link would fail. Three new tests
(`07`–`09`) cover the basic case, selective inclusion, and
transitive pull-in.

### liborisc

The library itself lives in [`tools/cc/lib/`](tools/cc/lib):

- `io.c` — `print_str`, `print_char`, `print_int`, `print_hex`
- `string.c` — `strlen`, `strcmp`, `strcpy`, `memcpy`, `memset`,
  `memcmp`, `atoi`

Each file becomes its own member of `liborisc.ora`, contributing
its global symbols to the archive index. `build.sh` is a
six-line driver that compiles every `.c` here through pcc plus
`asmorisc -r` and bundles the outputs via `oar c`. The C demo
runner [`examples/cc/run_c.sh`](examples/cc/run_c.sh) auto-runs
`build.sh` the first time it doesn't find the archive.

[`liborisc.h`](tools/cc/lib/liborisc.h) is the C-side prototype
header. It's separate from the existing
[`tools/cc/arch/orisc/orisc.h`](tools/cc/arch/orisc/orisc.h)
(which holds the OR-file inspection and OL/OS macros) — most
programs include both.

### Migration

`examples/cc/lib.c` — the ad-hoc `print_str`/`print_int` source
that every demo's `run_c.sh` had been compiling and linking
manually — is gone. Its functions live in `tools/cc/lib/io.c`
now and reach demos through the archive. Eleven existing demos
keep working unchanged (their `extern void print_str(...)`
declarations resolve through the archive); a new
[`strings_demo.c`](examples/cc/strings_demo.c) exercises
`strlen`, `strcmp`, `strcpy`, `memset`, `atoi`, and `print_hex`
to prove the archive-pulled string functions actually link.

### What's not in v1

- **`malloc`/`free`** — would need a heap design and probably
  ObjAllocStore-backed bookkeeping; left for when something
  actually needs dynamic allocation.
- **`printf`** — needs varargs, which pcc's orisc backend hasn't
  exercised yet. The `print_*` family covers everything the
  demos need.
- **Math, file I/O, time, anything else** — none of the demos
  reach for it. Easy to add as new `.c` files when they do.

### Status

9/9 linker tests pass (six original + three archive). All 11
existing C demos work through the new archive-based pipeline,
plus the new `strings_demo`. `liborisc.ora` is 5140 bytes; a
hello-world that uses only `print_str` pulls in `io.oro` (1709
bytes) and skips `string.oro` entirely.

## Phase 16 — A two-way terminal with capability-shaped services

Up through Phase 15, oriscterm did one thing: write text. CPUs
SENDed bytes to its console object and the bytes appeared on
screen. No keyboard, no cursor control, no graphics, no anything.

Phase 16 turns it into a small graphics workstation in the spirit
of mid-1980s Tektronix / Apollo terminals — and does it the
Object-RISC way, by giving the terminal multiple capability-shaped
service objects rather than overloading a single console with
escape-sequence cliches.

### Keyboard input — the two-way piece

A second service object at index `2`. CPUs subscribe by SENDing a
derived `R|S` self-ref to it; the terminal records the ref and
SENDs a key event back through it on every keystroke. Each event
carries the codepoint in `R4` and a modifier mask in `R5`.

Codepoints: plain ASCII bytes pass through verbatim; special keys
(arrows, function keys, BackSpace/Return/Tab/etc.) use a portable
encoding ≥ `0x100`, so the wire format can describe arrow keys
without conflating them with control characters. Modifiers are a
four-bit mask (Shift, Ctrl, Alt, Meta) decoded best-effort from
Tk's `event.state`.

The C demo `kbd_echo.c` subscribes, attaches a queue, polls
forever, and prints each event. It also surfaces a small "OR
hygiene" lesson worth flagging: `print_str` → `console_write`
reads from `O3`, but both the subscribe SEND (which nulls O3 to
clear the wire's reply-cap slot) and the queue dispatch (which
overlays O1..O4 from the wire payload) clobber it. The demo saves
the boot O3 into O15 once and copies it back before every print.
This pattern shows up in every Phase 16 demo.

### Multiple capability-shaped service objects

The terminal now exposes five service indices, each a separate
capability that CPUs can hold independently:

| Index | Service     | Role                                                      |
|-------|-------------|-----------------------------------------------------------|
| `1`   | console     | Append text to the scrolling pane (existing)              |
| `2`   | keyboard    | Subscribe to / unsubscribe from key events (new)          |
| `3`   | grid        | Write text at a fixed (col, row) on the canvas (new)      |
| `4`   | vector      | Lines, rectangles, ovals on the canvas (new)              |
| `5`   | raster      | Bitmap blit (protocol pinned, implementation deferred)    |

The terminal window now has two regions: the existing scrolling
text pane on top (driven by service 1) and a graphics Canvas
below (driven by services 3 and 4). Both share the same monospace
font, so the grid service's character cells line up visually with
the text pane's columns.

Each service has its own SEND payload convention, documented in
[`tools/devices/README.md`](tools/devices/README.md). Vector
commands are immediate (R4 = command, R5/R6 = packed coords);
grid follows the same pull-bytes-via-OBJ_READ_REQ pattern as
console but with explicit (col, row) positioning. The vector
palette is a small fixed 9-colour set picked for an
"early-1980s graphics terminal" feel — index 0 is the
background, 1 is the default foreground, 2–8 are the usual
spectrum.

### The painting demo

`examples/cc/paint.c` wires keyboard + grid + vector together as
an interactive painting program. Arrow keys move a logical cursor;
letter keys drop dots / lines / rectangles / ovals at it; `C`
cycles colour; space clears; ESC exits. A grid-positioned title
("PAINT — Object RISC vector demo") lands at column 2, row 0 of
the canvas to demonstrate the grid service.

The demo also surfaces a real register-pressure limit in the orisc
backend. Inline asm with too many `"r"` constraints in one block
trips pcc's allocator with "couldn't find available register" —
direct fallout of Phase 12's decision to mark R16..R23 as
unallocatable to suppress dead callee-save sequences. The fix
inside paint.c: factor each SEND into its own helper function so
each call site uses ≤ 4 input operands. The longer-term fix is
the optimizer learning to elide dead saves so we can re-enable
the callee-save band; not for this commit.

### What's not yet done

- **Raster blit.** The protocol's reserved (index 5, SEND
  payload pinned in oriscterm) but the implementation just logs
  and drops. Lands when something actually needs it.
- **Window control** (title, resize, query dimensions). Useful
  but pure plumbing; deferred.
- **Multiple keyboard / pointer subscribers.** Currently a null
  SEND removes all subscriptions — coarse, fine for the demos.
  Would refine to per-CPU unsubscribe when multiple subs are
  needed.

## Phase 17 — Pointer service and the OR-hygiene discipline

The terminal had keyboard and graphics; mouse was the obvious
next gap. Per the user: tablet-style absolute coordinates — the
terminal owns cursor display, the protocol just delivers `(x, y)`
— rather than the "terminal CPU manages cursor visible state"
model. Cleaner separation, less protocol surface.

### The pointer service (idx 6)

Same shape as keyboard: subscribe-and-receive. The CPU SENDs a
derived `R|S` self-ref; the terminal records it and SENDs an
event back through that ref on every motion / button change.
Each event carries:

- `R4` = event type (0 motion, 1 button-down, 2 button-up)
- `R5` = packed `(x << 16) | y` in canvas pixel coordinates
- `R6` = button (1=L, 2=M, 3=R; 0 for motion)
- `R7` = button-state mask (bit N set if button N is held)

Bound on Tk's `<Motion>`, `<ButtonPress-N>`, `<ButtonRelease-N>`
on the canvas widget directly, so coordinates arrive in canvas
space — no widget-translation math.

### `mouse_paint.c`

The canonical demo. Click drops a small filled square; drag
draws a stroke (line segments between successive motion samples
while button 1 is held); middle click cycles the pen colour;
right click clears. ~200 lines of C. The user closes the Tk
window to quit (oriscrun tears the rest down).

### The OR-hygiene discipline, formalised

Every demo so far has tripped over the same trap: the
ReceiveQueuePoll overlay sets `O1..O4` from the wire payload
(Vol VI §6), and any SEND clobbers `O1/O2/O3` too — so after
either operation, libc calls reading from `O2` (stack ref) or
`O3` (data ref) silently fail. We discovered this once with
`O3` (Phase 16), then again with `O4` and `O2` after the user
reported the keyboard demo printing garbage and exiting on the
first keystroke.

`mouse_paint.c` introduces the discipline that makes the
problem go away for good: every helper function that issues a
primitive or a SEND calls `restore_or_state()` on its way out,
which copies the boot-time stack/data/self refs from
`O13/O14/O15` back into `O2/O3/O4`. The boot values are parked
in the high three slots once at startup. Callers in `main()`
never have to think about it — `print_str` / `print_int` "just
work" after any helper returns.

Documented in `tools/devices/README.md` for both keyboard and
pointer subscriber patterns. `paint.c` was migrated to the new
shape too; `kbd_echo.c` already had restore-after-each-asm
because it predated the helpers.

### Headless test infrastructure

`fake_terminal.py` was extended to emit pointer events alongside
keyboard. Its CLI is now an event-spec list:

    --event key:A
    --event motion:120,170
    --event down:100,150,1
    --event up:120,170,1

Each spec waits for the corresponding subscription to arrive
(kbd events wait for kbd subscribe; pointer events wait for
pointer subscribe). New `test_mouse_paint.sh` exercises the
full mouse flow — click + drag + release, middle-click, right-
click — and asserts on cpu0's stdout. The kbd test was
migrated to the new event-spec format and still PASSes.

### Status

7/7 asm tests, 9/9 ld tests, 115/115 sim tests, 12/12 C demos,
both device tests (kbd_echo + mouse_paint) PASS deterministically.
The terminal now has six service objects on one port — text
console, keyboard, grid, vector, pointer, and the (still
deferred) raster — and the demos cover end-to-end interactive
use of the first five.

## Phase 18 — The first OS-shaped piece: hostfsd

CPU programs could compute, talk to other CPUs, draw on a screen,
and read a keyboard — but they couldn't read or write a file.
The first proper "operating system" piece: a host-filesystem
server that exposes the host's actual files as Object RISC
resources, and a C library that wraps the wire protocol in
familiar `open` / `read` / `write` / `close` shape.

### Design choice

Two reasonable shapes:

1. **File-as-object.** `hf_open` returns an OR ref to a file
   object hosted on `hostfsd`; CPU does `OLB` to read bytes.
   Architecturally pure — files become first-class capability-
   bearing objects. Bites on the same architectural friction
   that bit linkboot (no register-indexed `OL`, no `MapObject`
   on remote sources).

2. **Fd-style with hostfsd-initiated transfers.** `hf_open`
   returns an `int fd`; `hf_read(fd, buf, count)` SENDs to
   hostfsd, which `OBJ_WRITE_REQ`s the bytes back into the
   CPU's stack buffer (the lib derives the right OR ref from
   the buffer's VA). No 32K cap, normal-shaped C API.

Phase 18 ships #2. The file-as-object aesthetic is appealing but
unworkable for v1 — too many architectural workarounds. With
something concrete in tree, a v2 layer that exposes files as
ORs becomes a sensible follow-up.

### `tools/devices/hostfsd`

A new Python device — third in the family alongside `oriscterm`
and `linkbootd`. Single service object at index 1; CPUs SEND
requests with the operation code in `R4`:

| Op          | Code | Body                                                     |
|-------------|------|----------------------------------------------------------|
| `SUBSCRIBE` | 4    | O2 = subscriber's reply ref                              |
| `OPEN`      | 0    | O2 = path buf, R5 = path off, R6 = path len, R7 = flags  |
| `CLOSE`     | 1    | R5 = fd                                                  |
| `READ`      | 2    | O2 = dst buf (W cap), R5 = fd, R6 = dst off, R7 = count  |
| `WRITE`     | 3    | O2 = src buf, R5 = fd, R6 = src off, R7 = count          |

All responses come back as SENDs to the per-CPU reply ref
established by SUBSCRIBE. Errors negative (`-1` EBADF, `-3`
ENOENT, `-4` EACCES, etc.).

`READ` is the interesting case: hostfsd `read()`s from the host
file, then issues `OBJ_WRITE_REQ` to land the bytes in the CPU's
buffer. The wire's bidirectional `OBJ_*_REQ`/`RESP` traffic
already supports device-to-CPU writes — `simorisc`'s "memory
controller" tick handles them just like cross-CPU writes — so
no new infrastructure was needed beyond hostfsd itself.

### `--root` jailing

Optional `--root DIR` chroots the service: paths resolve relative
to `DIR` and any escape via `..` or absolute paths returns
`EACCES`. Without `--root` the service is unjailed (full host FS
access for the launching user). Useful default for the test suite
(jail to a fixture dir); fine without for development.

### `tools/cc/lib/host_io.c`

Adds `hf_init`, `hf_open`, `hf_close`, `hf_read`, `hf_write` to
the libc archive. The `hf_read` buffer must be on the stack
(needs W cap; the boot data ref doesn't have it); `hf_write`
buffers can be anywhere with R cap. Programs follow the OR-
hygiene contract: park boot O2/O3/O4 in O11/O15/O14 once at
startup, every helper restores them on the way out. hostfsd's
service ref needs to be in O10 by the time `hf_init` runs (the
runner's `--service` order is responsible).

### Demo + test

`examples/cc/host_cat.c` opens `README.md` and streams its
contents to console — a `cat` from the simulated CPU through
the wire-format crossbar to the host filesystem and back. The
README arrives intact.

`tools/devices/tests/test_hostfs.sh` is the headless variant:
creates a fixture `test.txt` with known contents, jails hostfsd
to its directory, asserts on the cpu's stdout (`FD=0`,
`READ N=14`, `one`/`two`/`three`, `END`).

### What's not yet done

- **Seek / stat / readdir / unlink / mkdir.** Just the four
  basic ops for now.
- **File-as-object aesthetic.** Mentioned above; v2.
- **Multi-process libc-using demos.** The OR-hygiene contract
  is fragile and needs the runner to set `--service` order
  exactly right. A higher-level launcher abstraction would
  help.
- **A shared Python library for device authors.** The user
  flagged this — `oriscterm`, `linkbootd`, `hostfsd`, and
  `fake_terminal.py` all duplicate the same wire helpers. A
  base class with handshake + dispatch + storage primitives
  would let the next device come together in maybe 50 lines.
  Now that we have three real devices to learn from, the
  abstraction lines should be visible.

## Phase 19 — A shell (and a terminal library)

The C demos for keyboard / mouse / paint were getting unwieldy —
each one open-coded the same SEND patterns, OR-hygiene saves,
queue polls. Time to extract a real terminal library, then build
the first shell on top of it.

### `tools/cc/lib/term.c`

Wraps the wire protocols for oriscterm's console (idx 1) and
keyboard (idx 2) services. The interesting functions:

- `term_init()` — parks boot O2/O3/O4 into O11/O14/O15, attaches
  a receive queue to the self-service, subscribes to keyboard.
  Must be called once at program start.
- `term_print(s)` / `term_print_char(c)` / `term_print_int(n)` /
  `term_print_hex(n)` — write to the terminal console (NOT host
  stdout — for that keep using the print_* family from io.c).
- `term_getkey(*out_mods)` — block until next keystroke.

Programs follow the standard OR-hygiene contract: park boot
ORs once at term_init, every helper restores them on the way
out. Single-byte output uses a 256-byte static lookup table in
`.data` — `term_print_char(c)` sends offset `c` from that
table — because stack-buffer chars get clobbered by subsequent
calls before oriscterm's async OBJ_READ_REQ comes back.

### Bug uncovered: pcc treats `register __or` as callee-save

Building term_init exposed a real backend issue. With

    register void *__or o11_save __asm__("o11");
    o11_save = boot_stack;

pcc emits a callee-save dance: prologue stashes the OLD value
of O11 into O10, body sets O11 = boot stack, epilogue restores
O11 from O10. This corrupted the hostfsd ref the runner had
placed in O10.

Workaround in this commit: avoid `register __or __asm__("oN")`
declarations for the save slots; use raw `asm volatile("omov
o11, o2")` instead. pcc doesn't track the assignment and won't
emit save/restore. Documented in term.c at the relevant spot.
Real fix is in the orisc backend's RSTATUS / regs.c — those
slots should be marked caller-save. Left for a follow-up.

### `tools/devices/hostfsd` gains `OP_OPENDIR`

The shell needs `ls` — added `OP_OPENDIR` (5) to hostfsd. Same
shape as `OP_OPEN` but the resulting fd reads back a `name\n`
listing of the directory's entries (subdirs end with `/`).
Implemented by extending OpenFile with an optional `buffer`
field; `OP_READ`'s handler picks the buffer path when
`is_dir()`. `hf_opendir(path)` added to the libc wrapper.

### `tools/oriscrun --hostfsd`

Mirrors `--terminal` for the hostfsd device. `--hostfsd
"pid=N,root=PATH"` spawns it inline with the rest of the
oriscrun-launched system, including the wait-for-READY
synchronization. Removes the boilerplate of spawning hostfsd
manually before oriscrun.

### `examples/cc/shell.c`

The MVP shell. Built-ins: `help`, `cat <path>`, `ls [<path>]`,
`exit`. Read-line loop with backspace (no visual undo for
v1 — text widget is append-only). Prompt is `orisc> `.

The cute touch the user requested: the build banner shows the
current real-world date minus 40 years. `run_shell.sh` computes
it via `date -v -40y +"%b %e %Y %-l:%M %p"` and passes it to
cpp via `-DBUILD_BANNER`. So a fresh build today (2026-05-03)
announces itself as `Object RISC Shell (May 3 1986 6:34 PM)`.

### Headless test

`tools/devices/tests/test_shell.sh` builds shell.orx, launches
oriscbar + real hostfsd (jailed to a fixture dir) + the
already-extended `fake_terminal.py` (which now also handles
console-write SENDs by issuing OBJ_READ_REQ and rendering the
bytes), and asserts the shell handled `ls` correctly.

`fake_terminal.py` got a buffered render mode — single-byte
console writes were getting block-buffered when redirected to
a file, so we accumulate everything and dump it as a `console
render` block at exit.

### What's not yet done

- **Migrate existing demos onto term lib.** kbd_echo / paint /
  mouse_paint still open-code their inline asm. Worth a pass
  to validate the lib API; deferred to keep this commit
  bounded.
- **Backspace visual undo.** Needs grid-service overwrites or
  cursor positioning; defer until we have a use case.
- **Shell command history / line editing / piping.** Out of
  scope for the MVP.
- **Race in the test's exit path.** The shell's "bye!\n" SEND
  goes out, but its OBJ_READ_RESP races with TaskExit and
  fake_terminal sometimes doesn't render it before tearing
  down. Documented in the test; the SEND is visible in the
  wire log so we know the shell logic is correct.
- **Static C functions still leak into the global symbol
  table** (asmorisc default-globals everything not L\d+).
  Same flag as Phase 18.
- **A shared Python device library.** Three Python devices in
  tree (oriscterm, linkbootd, hostfsd) plus fake_terminal.py
  all duplicate the wire helpers. Still queued for a
  refactor commit.

## Phase 20 — Running programs from the shell

The shell can list and cat files. The natural next step toward "MVP
OS" is letting the user `run hello.orx` from the prompt. The
architectural choice: not in-CPU exec (which would need a relocatable
loader and freed page table) but **spawn semantics on a pool of
pre-launched spare CPUs** — each spare runs a chunked-boot loader,
gets a guest program from `linkbootd`, hands off via a new firmware
primitive, and gets reset by simorisc when the guest exits so the slot
is reusable.

### `simorisc --reset-on-exit`

Non-leader CPUs running the chunked-boot loader call this. On
TaskExit, instead of terminating the process, simorisc:

1. Defers the actual reset by `RESET_DRAIN_SECONDS` (0.5 s) of
   wall-clock time. During the drain the CPU is inactive but
   `_process_requests` keeps answering OBJ_READ_REQs against the
   still-valid descriptor table — without this, the receiver of the
   guest's last `term_print` SEND tries to read a descriptor that's
   already been wiped and gets a STALE fault.
2. After the deadline: clears all GPR/OR/HI/LO/PC, drops the
   descriptor table and mappings, and re-runs `init_cpu` /
   `populate_self_service` / `install_external_services` /
   `snapshot_boot_state` from saved boot args. The crossbar
   connection, PID, and `--service` refs all persist across the
   reset — only program state is wiped.

The shared libc pattern of "post-print-then-exit" turned out to be a
real race; the drain neatly papers over it without touching the wire
protocol.

### `chunkboot.s` (generated by `gen_chunkboot.py`)

Sister to the existing `linkboot.s`. The original loader copies a
single image of ≤256 B in one SEND — fine for the demo's 40-byte
hand-encoded module, useless for an 8 KB shell-spawned program. The
chunked version implements a request/reply protocol with `linkbootd`:

- Loader announces with `R5 = next_offset` (0 on initial). Master
  replies with one chunk SEND carrying a fresh `(home=master, idx=N,
  R)` chunk-source ref, the chunk's offset/length, and the program's
  total text/data sizes.
- Loader OLW-copies the chunk window into a writable code (or data)
  object it allocated on the first iteration, then ack-announces
  with the next offset. Repeats until the position equals
  `text_size + data_size`.
- After the last chunk: ObjDerive both objects (drop W; keep R|X|C
  for code, R|C for data), CALL `InstallProgram` (firmware
  primitive #0x009).

Subtle bits: the loader saves the V-cap-bearing self-svc ref in O15
because every `ReceiveQueuePoll` overwrites O1..O4 from the queue
dispatch. The chunk-source ref is copied into O13 on each iteration
because it's transient. Boot O6 — a "pad" service slot in the spare
CPU's `--service` order — gets hijacked as the R+S self-ref slot.
Just before the InstallProgram call the loader shifts O7..O11 down to
O5..O10 (`omov o5, o7; omov o6, o8; ...`), dropping the linkbootd ref
off the front so the guest sees the same boot ABI as a standalone
shell-launched program would (O5 = console, O6 = keyboard, O10 =
hostfsd).

### Firmware primitive `InstallProgram` (call #0x009)

The hand-off from loader to guest can't be a plain JR. The loader's
own code is mapped at `CODE_VA`, and the guest's code wants to be at
`CODE_VA` too — pcc-compiled programs aren't position-independent
(`j #target26` resolves to absolute addresses). MapObject doesn't
remove existing mappings; lookup_va finds the first match in
insertion order. So a naive sequence "MapObject guest at CODE_VA;
JR" fetches the JR's delay-slot from the *loader's* mapping (still
first), then jumps into the guest at offset 0 — which works for the
target but the delay slot already executed garbage.

`InstallProgram` is the atomic version. Inputs: `O1 = code_ref`,
`O3 = data_ref` (or null), `R4 = entry_offset`. Effects:

1. Drop every mapping whose descriptor isn't `TAG_STACK` (so the
   loader's text + any temp R|W mappings the loader used during
   the chunk copy are gone, but the inherited stack stays).
2. Map `code_ref` at `CODE_VA` with `R|X`, and `data_ref` (if
   provided) at `DATA_VA` with `R`.
3. Reset all GPRs to zero, then set R7=PROCID, R29=SP=STACK_TOP-16,
   R30=R31=0. O1=code_ref, O3=data_ref, O2=stack_ref (recovered
   from the surviving stack mapping so `term_init`'s `omov o11, o2`
   parks the right thing).
4. Set `cpu.pc = CODE_VA + entry_offset`, `cpu.next_pc = pc + 4`.
5. Set a `_installed_program` flag. The CALL dispatch site checks
   it and skips its post-call `pc += 4` advance. Net effect: the
   next instruction fetched is the guest's entry, and we never
   re-execute anything past the loader's CALL.

It mirrors `MapObject` in shape but is more like a syscall: "take
this program and run it." The spec is in the docstring, not yet in
the architecture volumes.

### `linkbootd` rewrite

The original linkbootd was a one-shot single-image server. The new
one has both a control plane (for the shell) and a chunked serve
loop (for loaders):

- `--shell-pids` and `--loader-pids` lists tell it who's allowed to
  send what. Shell SENDs are `op=spawn` requests; loader SENDs are
  announces/acks.
- A spawn request carries the shell's R+S mailbox-ref (in O3) and
  a path-source-ref + offset + length (in O2 / R4 / R5). linkbootd
  issues an OBJ_READ_REQ to the source, loads the resolved file
  through `load_image()` (now returning `(text, data, entry)`),
  and queues a `PendingSpawn`.
- Per-loader state machine: IDLE → ASSIGNED (mid-serve) →
  WAITING_DONE (last chunk sent; awaiting reset+re-announce). When
  a loader announces with `R5=0` while WAITING_DONE, that's the
  "previous run finished" signal — linkbootd notifies the original
  shell with a `LB_RESULT_MAGIC` SEND carrying the exit code, then
  flips the loader back to IDLE.
- `--root` resolves spawn paths against a host-side directory and
  rejects `..` escapes — same pattern as `hostfsd --root`. The
  shell launcher points both at the same dir so `run foo.orx` from
  the shell finds the same file `cat foo.orx` would.
- For backward compat with `run_python_master.sh`, `--image PATH`
  preloads a "sticky" PendingSpawn that re-queues itself after each
  serve, so all `NCPUS` loaders boot from the same image.

### `tools/cc/lib/linkboot.c` — the lb_init / lb_spawn helpers

The shell's `run` command calls `lb_spawn(path)`. To avoid messing
with the shared self-svc queue (where keyboard events and hostfsd
responses already interleave; a long-running spawn would either drop
keystrokes or mis-decode responses), `lb_init()` ObjAllocs a
dedicated 16-byte mailbox object, attaches a queue of depth 4 to it,
and derives an R+S sub-ref. The full ref lives in O12 (poll target);
the R+S ref lives in O13 (sent to linkbootd in the spawn request's
O3). lb_spawn polls O12 with infinite timeout, recognizes the
`LB_RESULT_MAGIC` reply, and returns the exit code.

The boot ABI for shells using these helpers gets one new slot:
`O7 = linkbootd` — replacing the first "pad" entry in the standard
`--service` order. `O12`/`O13` are not in the docs because they're
strictly internal to linkboot.c.

### Pool of pre-spawned spares in `run_shell.sh`

The launcher now spawns `oriscbar + oriscterm + hostfsd + linkbootd
+ shell + 4 spare CPUs` (PIDs 32..35). Each spare runs `chunkboot.orx`
with `--reset-on-exit`. Their `--service` slot order leaves O6 as a
pad (loader hijacks it), with terminal/keyboard/hostfsd at O7/O8/O11
in the loader's view — those are what the loader's pre-jump shift
demotes into O5/O6/O10 of the guest's view.

`oriscrun` grew `--linkbootd "pid=N[,shells=A;B][,loaders=C;D][,
image=PATH][,root=PATH]"` to launch the new daemon (semicolons inside
the value to keep comma as the outer separator), plus a `,reset`
field on `--cpu` specs that adds `--reset-on-exit` to the simorisc
invocation.

### `run` command in shell.c

Just `cmd_run(arg) { int code = lb_spawn(arg); print "[exited "; print
code; print "]\n"; }`. Returns to the prompt. Existing `cat` / `ls` /
`exit` / `help` are untouched.

### Ancillary fixes uncovered along the way

- **`_validate_at_home` was forcing alignment on byte-stream reads.**
  An OBJ_READ_REQ for 2 bytes at offset 465 (the shell printing
  `"]\n"`) was getting RESP_BUS_ERROR because 465 isn't 2-aligned.
  But the validator is shared by CPU-side OL/OS *and* device-side
  byte transfers; CPU-side already enforces VA alignment in
  `lookup_va` before the request even goes out, so the second check
  in the validator was only catching legitimate device traffic. Fix:
  remove the alignment branch from `_validate_at_home`, keep the
  `lookup_va` one.

- **fake_terminal's bounded drain timeout was killing visible
  output.** It was also incidentally what surfaced the alignment
  bug: without instrumented OBJ_READ_RESP logging it just looked
  like dropped bytes.

- **Largecat test was already flaky.** With my queue-depth tuning it
  exposed a long-standing libc-design issue: the shared self-svc
  queue interleaves keystrokes with hostfsd responses, so under
  load the `cat` loop occasionally mis-decodes a keypress as a read
  reply. The right fix is per-service queues; for now I kept the
  queue at depth 16 (drops excess keystrokes at the door rather
  than corrupting reads) and `KILL`-cleanup the test's CPU after
  the typed input runs out.

### What's not yet done

- **Per-service receive queues in libc.** Today term_init's queue
  on O4 is shared by keyboard events AND hostfsd responses, with
  the interleaving problem above. lb_spawn already sidesteps it
  with its own mailbox; the same pattern should generalize to
  hf_init.
- **Capability passing in SEND.** Once a spare CPU starts running
  a guest, it has its own self-svc queue and its own everything —
  but the *shell* can't hand it any of its own refs. Programs are
  fully isolated, no fork-style inheritance, no IPC between
  shell and guest beyond the spawn-result.
- **No exit code from the guest.** simorisc captures it in
  `cpu.exit_code` at TaskExit, but the reset clears it before
  linkbootd can read it. Today the shell always sees code 0; a
  proper fix needs a "report exit code" hook between TaskExit and
  reset.
- **Non-zero entry offsets.** The InstallProgram primitive accepts
  `R4 = entry_offset`, but the chunked-boot loader hardcodes 0
  because pcc-built programs always start at offset 0 of .text.
  The plumbing is there if needed.
- **Reuse the chunked loader for the linkboot demo's NCPUS>1.**
  Done in this commit — `run_python_master.sh` now uses
  chunkboot.s and the sticky-preload mode of linkbootd, replacing
  the older single-shot path. `gen_linkboot.py` (and its
  `linkboot.s` / `master.s` outputs, plus the
  `13_linkboot_loader.s` validation test) stay around because they
  document the original two-CPU in-process protocol that doesn't
  involve linkbootd at all.

## Where things stand now

- 7 architecture volumes plus the integration contract, revised to
  reflect everything learned in implementation.
- An assembler (with `.set`, label arithmetic, and a `-r` mode that
  emits relocatable `.oro` object files) and a simulator
  (~3,000 lines of Python, stdlib only) with single-CPU,
  in-process multi-CPU, and multi-process modes.
- A linker (`tools/ld/orld`) that combines `.oro` object files
  into executable `.orx`, with per-file local symbol scoping,
  cross-file global resolution, and a small set of relocations
  covering everything the toolchain emits. An archiver
  (`tools/ld/oar`) bundles `.oro` files into `.ora` archives that
  the linker pulls from selectively.
- A validation suite spanning thirteen categories (integer, logical,
  memory, control, oreg, omem, firmware, traps, call, golden,
  multi-CPU including link boot, loadable modules, receive queues),
  all passing.
- A wire-level crossbar daemon (`oriscbar`), separate-process CPU
  runtime (`simorisc --bar`), and a graphical terminal
  (`oriscterm`) that's now genuinely two-way and graphical:
  six capability-shaped service objects (console, keyboard,
  grid-positioned text, vector drawing, raster, pointer) on the
  same port, each a separate ref a CPU can choose to hold.
- A vendored pcc with an Object RISC backend that compiles real C
  programs end-to-end, including a working `__or` storage-class
  qualifier on parameters and register-bound variables (caller and
  callee sides of the calling convention both wired). The C
  pipeline now goes through the linker — no more per-unit label
  mangling.
- A small C library (`liborisc.ora`) covering console I/O, the
  standard string/memory primitives, and host-filesystem access
  via `hostfsd` (`hf_open` / `hf_read` / `hf_write` / `hf_close`).
  Archived for selective inclusion so programs only pay for what
  they call.
- A host-filesystem device server (`hostfsd`) — first
  OS-shaped piece, with optional `--root` jailing — that lets
  CPU-side C programs read and write actual host files via the
  wire-format crossbar.
- A generic link-boot loader that lets you spin up extra CPUs whose
  code is decided at runtime — announce on the crossbar, receive a
  module by SEND, map and JR. The chunked variant
  (`examples/linkboot/chunkboot.s`) handles arbitrarily large
  programs in 256-byte windows and hands off via the
  `InstallProgram` firmware primitive (call #0x009).
- A Python-side link-boot server (`tools/devices/linkbootd`) that
  hosts a boot image and answers any number of loader CPUs over the
  wire, with a control plane that lets the shell load programs by
  path on demand.
- A working `run <path>` command in the MVP shell, served by a
  pre-spawned pool of spare CPUs whose `--reset-on-exit` makes them
  reusable program slots: TaskExit clears state and re-runs the
  loader, which re-announces to linkbootd as "ready for the next
  job".

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
