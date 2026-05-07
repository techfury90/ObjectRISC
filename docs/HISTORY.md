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

## Phase 21 — Shell polish: cd, more, a usable terminal

Two rounds of "the shell exists, now make it not annoying."

### `cd` / `pwd` / `echo` / `cycles`

The shell maintains its own cwd — absolute, normalized, lives on
main()'s stack (the data segment is mapped R-only, so a global
`cwd[]` would fault on `cmd_cd`'s assignment). `cat` / `ls` / `run`
prepend cwd to relative args and collapse `.` / `..` components in
place before sending the resolved path over the wire. The prompt
mirrors cwd (`/sub>`) so it's always obvious where you are. linkbootd
got matched semantics — absolute paths from the shell are now
treated as root-relative (same as hostfsd's jail) so `cat /foo` and
`run /foo` mean the same file.

`echo` is one printf. `cycles` calls the existing `ReadCycles`
firmware primitive — useful as a poke-the-CPU sanity check.

`InstallProgram`, the firmware primitive that hands control from the
chunkboot loader to the guest, also got renumbered from a Phase-20
draft slot (0x111, which collided with `Unmap` in the architecture
spec) to 0x009 — adjacent to `TaskExit` in the task-management
range, which is where it conceptually belongs.

### `more` and a properly sized terminal

The terminal's text pane was hardcoded to 10 rows in a window with
plenty of vertical space. Bumped to 24×80 with `fill="both",
expand=True`, and shrunk the graphics canvas underneath to a 16-row
strip — still usable for paint / mouse_paint / grid demos but no
longer eating most of the window when the program (e.g. the shell)
doesn't draw anything.

On the shell side, a `more <path>` built-in pages files with a
`--More-- (space/RET, q to quit)` prompt every 20 lines. `help`
reuses the same paginator (somewhat moot now that 24 rows fit it
comfortably, but it's there for future commands). The display is
append-only so the prompt isn't erased — we just newline past it.

### Largecat assertion retuned

The `test_shell_largecat` threshold was always optimistic: ≥95
stanzas only ever proved "rendering survived" and the actual count
varies wildly because fake_terminal's async OBJ_READ_REQs race with
the shell's READ_BUF stack-buffer reuse. Reframed to ≥50 stanzas
plus "saw a stanza ≥090" — same test of "stream ran end-to-end",
but doesn't flake on simulator load. The right fix (double-buffering
or a synchronous flush) is flagged in the test comment.

## Phase 22 — Tightening the libc, finishing the shell

A run of "the shell exists; now make it not annoying" — every
loose end the previous phase had marked as future work, knocked
out one at a time.

### Visual backspace

The Tk text widget had been append-only: typing "exi" + BACKSPACE +
"it" would commit "exit" to the line buffer (correct) but leave
the rendered text reading "exiit". oriscterm's `_append` now
interprets a 0x08 byte as "delete the character immediately
before it"; the shell's `read_line` echoes a literal `\b` when
the buffer was non-empty. Stack: standard terminal-style
semantics, no cursor-positioning escape grammar required, no-op
when nothing's there to erase. fake_terminal got matching `\b`
handling so test transcripts stay readable.

### Per-service receive queue for hostfsd

`hf_init` had been attaching its queue to O4 — the boot
self-svc — alongside `term_init`'s keyboard queue on the same
descriptor. A long `cat` then raced: a keystroke could land
between `hf_read` and the blocking poll, and the shell would
dequeue the keystroke as if it were a hostfsd reply, mis-decoding
R3 as the read count. Dhrystone numbers go very strange when
that happens. `hf_init` now ObjAllocs its own 16-byte mailbox,
attaches a depth-16 queue, derives an R+S sub-ref to subscribe
with, and parks the full ref in O8. All `hf_*` polls hit O8;
keyboard events stay on O4. Same shape as `lb_init`'s mailbox
in linkboot.c.

### `term_print_n_sync` — closing the SEND/READ race

`cmd_cat` had been double-buffering its READ_BUF (fill buf_a,
SEND, fill buf_b, SEND, alternating) to give the receiver a
window before the next `hf_read` overwrote things. That cut the
race down but didn't kill it. The proper fix used oriscterm's
existing reply_cap protocol: `term_print_n_sync(buf, count)`
sends a console SEND with `O3` set to a reply mailbox, then
blocks until the receiver acks. After the call the bytes have
been pulled — safe to reuse the source. The reply mailbox
reuses O8 (the hf one), since `cmd_cat` strictly alternates
hf_read with term_print_n_sync, so the queue holds at most one
outstanding message at any moment. fake_terminal grew matching
ack support; oriscterm already had the reply_cap path.

The largecat assertion went from "≥50 stanzas + saw a stanza in
the 90s" to "exactly 100 stanzas." Five runs in a row report
100/100.

### Real exit codes from spawned guests

The `[exited N]` line that `cmd_run` prints had been hardcoded
to 0. The chain was: guest TaskExit(N) → simorisc captured
`cpu.exit_code` → `reset_cpu` wiped it before the loader's fresh
announce reached linkbootd. Now: simorisc preserves the previous
exit code across reset by priming R6 with it just after re-init.
chunkboot.s reads R6 at boot, saves it across the chunk loop,
and forwards it in the announce SEND's int payload slot 2.
linkbootd parses int_payload[2] on the post-reset "ready"
announce and uses it as the exit code in the spawn-result
message. `cmd_run` prints `[exited N]` with the real `N` now.

Headless test: a 4-instruction guest that returns 42, run from
the shell, asserts the rendered output contains `[exited 42`.

### `cd` / `pwd` / `echo` / `cycles` / `time`

All Phase 21's polish-tier shell commands landed in the same
arc:

- `cd <path>` / `pwd` — the shell maintains an absolute,
  normalized cwd on main()'s stack (the data segment is mapped
  R-only, so a global `cwd[]` would fault on the assignment in
  `cmd_cd`). cat / ls / run prepend cwd to relative args and
  collapse `.` / `..` components in place before sending the
  resolved path over the wire. The prompt mirrors cwd
  (`/sub>`).
- `echo <text>` — one printf.
- `cycles` and `time` — wrap the existing `ReadCycles` (#0x301)
  and the newly-implemented `TimeNow` (#0x400) firmware
  primitives. simorisc's `System` captures `boot_time =
  time.time()` at construction; `TimeNow` returns microseconds
  since that moment in the low 32 bits of R3 (the spec puts the
  high 32 bits in the side-channel buffer of Vol VI §11, which
  this firmware doesn't yet implement; the low 32 bits wrap at
  ~71 minutes). `ClockResolution` (#0x410) returns 1_000_000.

linkbootd's path resolution also got matched semantics — absolute
paths from the shell are now treated as root-relative (same as
hostfsd's jail) so `cat /foo` and `run /foo` mean the same
file.

### Shell command history

A 16-entry circular buffer of past commands lives on main()'s
stack alongside cwd / line / prompt (~4 KB extra). `read_line`
saves each non-empty line on RET, and TK_UP / TK_DOWN cycle
through history entries — UP walks back, DOWN walks forward
toward the current empty line. The on-screen line is swapped
via the `\b` backspace-erase mechanic the visual-undo fix
introduced, then echoed character by character. Standard
limitations to flag: arrow keys other than UP/DOWN are still
ignored; typing-then-UP loses what you'd typed (bash-like, not
zsh-like); no `!!` / Ctrl-R search yet; no persistence across
shell exits.

### Verbose opt-in for spawned devices

oriscrun was hardcoding `-v` on every spawned device and
oriscterm's `_append` unconditionally emitted a `RENDER:` line
per console insert. Together those flooded the launcher's
combined log when the shell did anything substantial. Now the
`RENDER:` line is gated on `verbose`, and the `--terminal` /
`--hostfsd` / `--linkbootd` specs grow an opt-in `verbose`
field. Without it the device runs quiet; only its READY line and
warnings/errors come out.

### pcc orisc — all ORs caller-saved

The pcc backend marked O9..O12 as `SCREG|PERMREG` to mirror Vol
VII's "callee-preserved" convention, but pcc's spill machinery
for CLASSC (OR) registers can only land an old value in
*another* CLASSC slot — ORs can't live in integer memory. When
user code did `register __or T p __asm__("o11"); p = boot_stack;`,
pcc emitted a prologue that stashed the old O11 into O10,
clobbering whatever ref the runner had placed there. The libc
worked around it everywhere with raw inline asm. Real fix is
OBJSTORE-backed OR spill via OREFLD/OREFST (Vol VII §2.4 specs
this); for now PERMREG → TEMPREG so no spill is emitted. Spec
deviation is documented in macdefs.h and the orisc backend TODO.

### What's not yet done

- **OBJSTORE-based OR spill in pcc.** The PERMREG → TEMPREG
  change is a workaround. The real fix is teaching the backend
  to spill ORs into an `ObjAllocStore`'d OR-typed object via
  `OREFLD`/`OREFST`. Re-enables Vol VII's callee-preserved
  promise.
- **`Unmap` (0x111) / `Protect` (0x112)** spec'd, unimplemented.
  Small closeout each.
- **Side-channel mechanism (Vol VI §11)** — needed for `TimeNow`
  high bits, `Stat`, `ObjQuery` overflow returns. Touches the
  per-task control block.
- **Capability passing in SEND.** Still wants a use case (piping
  is the obvious one).
- **Shell file mutation (mkdir / rm / mv / touch).** Each is one
  hostfsd op + libc wrapper + shell command.
- **Migrate kbd_echo / paint / mouse_paint onto term lib.** Still
  inline asm.
- **Shared Python device library.** oriscterm / linkbootd /
  hostfsd / fake_terminal still duplicate wire-format helpers.

## Phase 23 — Object RISC Dhrystone

Once the shell felt usable end-to-end, the natural next question
was "how would this thing have benchmarked." Dhrystone v2.1
(Reinhold Weicker, 1984; Andrew C. Lowry's 1985 update) is the
canonical 1984/1985 era yardstick — period-perfect for an
alternate-history 1986 architecture.

### What it took to get the source compiling

Three toolchain pieces were needed before Dhrystone would build
unmodified through the pipeline.

**Writable .data.** Dhrystone leans on globals — `Int_Glob`,
`Bool_Glob`, `Arr_1_Glob[50]`, `Arr_2_Glob[50][50]` — and our
`.data` was mapped R-only. Same wall the shell's `cwd[]` had
hit. Fix is split: the data REF stays R|C (per CONTRACT §3 — the
OCAP and direct-OSB-through-O3 validation tests still see the
historical caps), but the MAPPING is now R|W so VA-based stores
to globals work. The intentional split between ref caps and
mapping prot in OR makes this fully consistent.

**`lw rd, LABEL[+OFFSET]` synthetic in asmorisc.** pcc emits
`lw r2, counter` for direct-to-global access and
`sw r4, Arr_2_Glob+1628` for array-with-constant-index. Both
expanded to `lui $at, %hi(eff); ori $at, $at, %lo(eff); INST
rd, 0($at)` — three words, but the lui-ori pair uses unsigned
imm semantics so no HI16 sign-extension correction is needed on
the linker side. `$at` is R1, reserved by the pcc backend's
RSTATUS so the assembler's expansions never collide with caller
state.

**Reloc addends in the .oro / orld format.** The 2 bytes of
historical reloc-record padding now hold a signed 16-bit addend;
old .oro files have zeros there so the change is backward-
compatible at the binary level. Used by the LABEL+OFFSET form
above, also available to other patterns that want to bake a
constant offset into a symbol-relative relocation.

### What it took to get Dhrystone running

Two further pcc backend gaps surfaced once compilation worked.

**STASG (struct assignment).** Dhrystone has `*p = *q;` between
struct values. local2.c::stasg used to comperr; now it lowers to
a memcpy call modeled on the MIPS64 backend — table.c gets a
real STASG entry pinning R4/R5/R6 to the memcpy ABI via NSPECIAL,
order.c::nspecial returns the rspecial, local2.c::stasg sets
R6=size and R4=dest then JAL memcpy with the standard 16-byte
spill area, zzzcode 'Q' dispatches.

**`.align` in defzero.** pcc's uninitialized-global path
(pftn.c::nidcl2) calls defzero directly without first calling
defalign. Two consecutive globals of mismatched alignment
(`char Ch_Glob;` then `int Arr_Glob[50];`) would land the int
array at an odd offset and word loads would trap. defzero now
emits its own leading `.align K` based on the symbol's
talign(). Initialized globals already got aligned via the
locctr → defalign path inside init.c.

### The benchmark itself

[`examples/cc/dhrystone/dhry.c`](examples/cc/dhrystone/dhry.c)
is a faithful port: algorithm and instruction mix unchanged,
adapted only at the I/O boundaries (printf → print_str /
print_int; malloc → two static `Rec_Type` instances; clock() /
time() → `read_cycles` / `time_now_us`; hardcoded
`DHRY_RUNS` instead of a scanf prompt).

Result on the OR-1000's nominal 16/20 MHz clock rates from
Vol I §3:

```
  Cycles per iteration: 4067
  16 MHz: ~3936 dhry/s = ~2.2 DMIPS
  20 MHz: ~4920 dhry/s = ~2.7 DMIPS
```

Plausible for a single-issue, naively-allocated 1986-class
RISC. For period context: VAX 11/780 = 1.0 DMIPS, MIPS R2000
(16.7 MHz) ≈ 8 DMIPS, SPARC v7 (16.7 MHz) ≈ 8, 80386
(20 MHz) ≈ 6. Most of the gap to the canonical RISCs is the
toolchain — pcc's orisc backend is a faithful port of the MIPS
template but hasn't been tuned, so each Dhrystone iteration
costs ~4000 cycles instead of the ~500-cycle figure the
contemporaries reported. Register allocator improvements,
peephole over the lw/sw-label 3-instruction expansion, and
inline struct copy would close most of that gap. Dhrystone-as-
shipped is the regression baseline to measure that work
against.

## Phase 24 onward — Ouroboros

The OS layer being built on top of Object RISC firmware is named
**Ouroboros**. The name evolved from OROS (Object RISC OS) and the
snake-eating-its-tail symbolism is fitting in several ways: the OS
runs on the architecture whose toolchain self-hosts; traps return
to where they came from via `ERET`; and the VM/CMS analogy that
motivated the architectural split (CP = firmware, CMS = Ouroboros)
is itself a recursive shape.

Phases 24+ are the build-up: enforce privilege (Phase 24), wire
trap delivery (Phase 25), then per-task address spaces and the
supervisor scheduler that turn one CPU into a multitasking system.

## Phase 24 — Privilege modes start working

Vol II Section 13 has named the three privilege modes — user,
supervisor, firmware — since the 0.1 revision, with `LCTRL`,
`SCTRL`, `ERET`, `WAIT`, and the four TLB-management instructions
listed as the privileged set. None of it was enforced. The
simulator decoded major opcode `0x10` as reserved-instruction; the
shell, the chunkboot loader, and every guest program ran at an
implicit "everything is allowed" privilege level, indistinguishable
from firmware. To start thinking about an actual operating system
on top of this — a CMS to firmware's CP, in the VM/CMS analogy
that motivated the work — the architecture had to take its own
privilege rules seriously.

This phase is the foundation: the smallest enforcement that a
guest OS can start to lean on, with the spec brought into line
with the implementation so future work doesn't drift again.

### `SYSTEM` opcode (Vol II §13.1)

Major opcode `0x10` is now `SYSTEM`. Sub-decoding follows the
COP0 layout familiar from MIPS-derived ISAs: `rs = 0x00` is
`LCTRL Rt, ctrl(Rd)`, `rs = 0x04` is `SCTRL ctrl(Rd), Rt`, and
`rs = 0x10` is the operand-less `CO` group selected by `funct`
(`TLBR/TLBWI/TLBWR/TLBP/ERET/WAIT`). The floating-point
reservation in Appendix A narrows from `0x10`–`0x1F` to
`0x11`–`0x1F`; the privileged-instruction set is now genuinely
in the architecture rather than implied by §13's prose.

asmorisc gains the eight mnemonics. `LCTRL` / `SCTRL` accept
`$N` for the control-register selector with a new `$` token in
the lexer; the operand-less ones take no operands.

### Privilege state in `simorisc`

CPUs gain a `mode` field (USER / SUPERVISOR / FIRMWARE), defaulting
to supervisor so every existing program — shell, chunkboot loader,
Dhrystone, the validation suite — keeps booting unchanged. A new
`--mode` CLI flag overrides the default at boot, and is honored on
reset (chunkboot-style `--reset-on-exit` slots come back in the
same mode they started in).

The privileged-instruction check fires at decode in `step()`:

- `LCTRL` / `SCTRL` in user mode → `privileged-instruction` (cause 0x0b).
- `LCTRL` / `SCTRL` of a firmware-only control register
  (numbers ≥ 8: VECBASE, TLBHI, TLBLO, INDEX, RANDOM, OBJTAB_*,
  ROUTE_BASE, ODC_*) in supervisor mode → `privileged-instruction`.
- `ERET` / `WAIT` / TLB ops in user mode → `privileged-instruction`.
- TLB ops in supervisor mode → `privileged-instruction`.

The TLB ops execute as no-ops in firmware mode (the simulator
backs VAs through `cpu.mappings`, not a real TLB) so firmware code
that blindly issues `TLBWR` won't fault; this matches the spec's
"reserved encodings" tier where the architectural behaviour is
defined but no machine-visible state changes.

Control-register backing storage covers the supervisor-visible
subset (STATUS, CAUSE, EPC, BADVADDR, CONTEXT, COUNT, COMPARE,
PROCID); the firmware-only registers read as zero. STATUS encodes
the current mode in bits [1:0], so a future scheduler can drop a
child task to user mode by `SCTRL $0, Rsuper` after setting up
its image.

### Per-primitive minimum mode in `CALL`

`dispatch_call` now consults a `PRIMITIVE_MIN_MODE` table before
invoking any primitive. A `CALL` from a mode below the
primitive's minimum returns `ERR_EPERM` in `R2` without entering
the primitive's body. Current allocations:

- **Supervisor required**: `MapObject` (#0x110), `InstallProgram`
  (#0x009), `InstallHandler` (#0x200) — anything that
  manipulates the VA layout or installs trap-like callbacks.
- **User accessible**: everything else this revision —
  `TaskExit`, `ObjAlloc`, `ObjFree`, `ObjDerive`, `ObjAllocStore`,
  `ReceiveQueueAttach`, `ReceiveQueuePoll`, `ReadCycles`,
  `ConsoleWrite` (will move to a service eventually), `TimeNow`,
  `ClockResolution`.

Future page-table primitives will require firmware mode. The
table is the central record of which primitives are intended for
which trust tier; adding a new primitive means deciding its mode
explicitly.

Vol VI §2.3 picks up a paragraph documenting the EPERM-from-mode
behavior so the spec and the simulator agree on how a guest OS
will see the rule.

### Validation

Five new tests in `08_traps`:

- `07_privileged_lctrl_user` — LCTRL in user mode → cause 0x0b.
- `08_privileged_sctrl_user` — same for SCTRL.
- `09_privileged_eret_user` — same for ERET.
- `10_privileged_tlb_supervisor` — TLBWR in supervisor mode →
  cause 0x0b (firmware-only).
- `11_privileged_lctrl_fw_only` — LCTRL of VECBASE in supervisor
  mode → cause 0x0b.

Plus `09_call/05_call_eperm_user_mode` for the CALL EPERM path.
The runner gained a `@mode:` directive so individual tests can
opt into user or firmware mode without touching the harness.

`08_traps/03_reserved_opcode.s` was the only existing test that
asserted `0x10` was reserved; it's been retargeted to `0x11`,
which still is.

115 → 121 sim validation tests passing. The full asm + device +
shell suites still green.

### What's next

Privilege modes are the floor; the OS is the building. Open
questions (in roughly the order the user articulated them):

- **Per-task address spaces.** The shell, the spawned guest, and
  any other concurrent user-mode task today share one VA layout
  per CPU. pcc generates flat-VA code, so isolation has to come
  from the simulator switching `cpu.mappings` on context switch.
  A "task" needs to become a first-class object with its own
  mapping list.
- **Supervisor scheduler.** Single-scheduler design (no double-
  scheduler — VM/CMS without the CP-mediates-distrusting-guests
  pressure). Wake-up boundaries are `TaskYield`, `TaskExit`, and
  blocking IPC. Probably round-robin to start.
- **Page-table primitives.** `MapObject` is the supervisor lever;
  a `MapObjectInTask` for the supervisor to set up a child's
  layout, plus `Unmap`, plus a way to fault on access.
- **Trap delivery.** Right now `Trap` is a Python exception with no
  in-architecture handler hookup. To run user-mode programs that
  recover from arithmetic overflow or capability violations, traps
  need to deliver to a supervisor-installed vector with `EPC` /
  `Cause` / `BADVADDR` populated, and `ERET` actually unwinding
  back. The control-register slots are present and `ERET`
  decodes; the wiring isn't.

## Phase 25 — Trap delivery (Ouroboros, day 2)

Phase 24 made privileged instructions trap. They had nowhere to
go: a `privileged-instruction` trap killed the process the same
way a bus error did, with the simulator's Python exception
bubbling up to `report_trap`. For Ouroboros to actually mediate
between user tasks and the hardware it runs on, traps had to
*deliver* — populate the architectural state, switch mode, and
hand control to a vector that firmware code controls.

### `deliver_trap` and the vector table

Volume II Appendix B has had the vector layout since the 0.1
revision: each cause has a fixed 64-byte slot at a defined offset
from `VECBASE`. The simulator now implements that. On a trap:

1. `EPC` ← faulting PC (also `cpu.saved_pc` for `ERET`)
2. `Cause` ← cause code
3. `BadVAddr` ← faulting VA (memory traps only; 0 otherwise)
4. `Status` saved-mode bits ← current mode
5. Mode → firmware
6. PC → `VECBASE + offset[cause]`

`Status` gained a bit-layout: bits [1:0] carry the current mode
(this is what supervisor SCTRLs to demote itself); bits [3:2]
carry the saved mode (`ERET` pops these into the current). The
spec leaves `Status`'s detailed layout to Volume V — this is the
reference choice. `SCTRL $0, Rx` accepts both halves at once,
which is how supervisor will eventually drop a child to user
mode: pre-load EPC with the user entry, write `Status` with
saved=user/current=supervisor, ERET.

`ERET` was decoded in Phase 24 as a no-op placeholder; it's now
wired: pops saved mode → current, jumps to EPC. The handler
adjusts EPC by 4 first if it wants to skip the trapping
instruction (most causes — TLB miss, future page faults — want
re-execute on resume, which is the default).

### Vector base and control register coverage

`VECBASE` (control register 8) is now writable from firmware via
`SCTRL` and read back via `LCTRL`. `Cause` (1), `EPC` (2),
`BadVAddr` (3) are similarly backed; `Status` (0), `Count` (5),
`ProcID` (7) were already covered. A `LCTRL` of `VECBASE` from
supervisor mode still traps — firmware-only, per Volume V.

`Trap` exceptions gained an optional `bad_vaddr` field. None of
the existing trap sites populate it yet (the memory-trap raise
sites don't carry the EA), but the wire is in place; the next
pass over deref/load/store will fill it in once we have a real
fault scenario that needs it.

### Vol II §14.1

Vol II §14 listed exception causes but didn't describe the
actual delivery sequence. §14.1 now spells out the six-step
trap-delivery contract, the `ERET` reverse path, and the
`VECBASE = 0` fallback (implementation-defined; we halt and
report).

### Validation

Three new tests in `08_traps`:

- `12_trap_delivered_to_vector` — arithmetic-overflow delivers
  to a handler that reads `Cause` and exits with code 9.
- `13_trap_handler_eret_resumes` — handler advances `EPC` by 4
  and `ERET`s; main resumes at the next instruction and exits
  with 42.
- `14_trap_promotes_user_to_firmware` — main demotes to user
  via `SCTRL Status; ERET`, the user-mode `LCTRL` traps with
  cause `0x0b`, the firmware handler executes `TLBWR` (proving
  it's in firmware mode) and reads `Status` bits [3:2] to
  recover the saved mode (expects USER = 0).

Trick the tests share: bias `VECBASE` so the relevant cause's
vector slot lands directly on `handler` (`VECBASE = handler -
offset`). Avoids laying out a full 16-vector trampoline table
for a single-cause test. Real firmware will lay out the full
table.

121 → 124 sim validation tests. All other suites green.

### What's not yet done

- **Trap from supervisor of an in-flight CALL.** When a primitive
  raises `Trap`, the CPU is parked on the CALL with `blocked_on`
  set. `deliver_trap` clears `blocked_on` so the handler runs
  cleanly, but the architectural model isn't pinned down yet:
  on `ERET`, do we re-issue the CALL or proceed past it? Right
  now `EPC` points at the CALL so re-issue happens by default.
- **Supervisor-installable handlers.** `VECBASE` is firmware-
  only, which means a supervisor program can't install handlers
  directly. The Vol II §14 prose mentions registering "with
  firmware through the appropriate primitive" — that primitive
  doesn't exist yet. Next phase, alongside the task scheduler.
- **`bad_vaddr` population at memory-trap sites.** The field
  exists; the `raise Trap(CAUSE_BUS_ERROR_D, ...)` callsites
  still pass faulting_pc but not the EA.

## Phase 26 — Tasks and the cooperative scheduler (Ouroboros, day 3)

Phase 25 made traps deliver to firmware code. Phase 26 makes the
CPU actually multitasking: a `Task` is now a first-class swappable
context, the scheduler picks the next runnable on every
`TaskExit`/`TaskYield`, and per-task address spaces mean
pcc-generated code (which assumes flat VAs) can be loaded into
multiple tasks without colliding.

### `Task` and per-task state

Per-task: register file (gpr/opr/hi/lo), program counter
(pc/next_pc), privilege state (mode/saved_mode/saved_pc), the
trap-side control registers (cause_reg/badvaddr_reg), and the
address-space mappings.

Per-CPU (shared across tasks): descriptor table, inbox, request/
response queues, cycle counter, **VECBASE**. The trap vector is
firmware-installed once per CPU — having every task carry its own
defeats the trap mechanism's purpose.

The CPU's `gpr`/`opr`/`mappings`/etc. fields *are* the running
task's live state. Context switch is a memcpy through the `Task`
struct: `save_cpu_to_task(cpu, outgoing)` then
`load_task_to_cpu(cpu, incoming)`.

### Task primitives

Six of Vol VI §4's nine task primitives are wired:

- **`TaskCreate`** (#0x000, supervisor) — Allocates a task
  descriptor (TAG_TASK = 0x4104), creates a `Task` struct with
  the standard layout (code at `CODE_VA` R+X, stack at
  `STACK_TOP` R+W), seeds R4 with the caller-supplied init value,
  inherits the parent's OPRs (so service refs propagate without a
  separate handoff mechanism), starts in `NEW`. Returns the task
  ref in O1 with `R | V | C` caps.
- **`TaskExit`** (#0x001, user) — Marks current `EXITED`, picks
  next runnable, context-switches. If queue empty, falls back to
  the pre-existing `TaskExitSignal` path (CPU goes inactive with
  exit code captured) so single-task programs still work
  unchanged.
- **`TaskResume`** (#0x002, supervisor) — Validates the task ref
  (live, generation matches, has V cap, is `TAG_TASK`),
  transitions `NEW`/`SUSPENDED` → `RUNNABLE`, appends to the
  back of the round-robin queue.
- **`TaskSuspend`** (#0x003, supervisor) — Removes from the
  runnable queue, marks `SUSPENDED`. Self-suspend triggers an
  immediate context switch; if no other task is runnable, returns
  `EBUSY` rather than wedging.
- **`TaskYield`** (#0x004, user) — Pops the next runnable,
  pushes the caller to the back, swaps. The caller's PC is
  bumped past the CALL before save so on resume it picks up at
  the next instruction. Round-robin FIFO.
- **`TaskCurrent`** (#0x005, user) — Returns a fresh ref to the
  calling task.

Three left for later: `TaskBindProcessor` (multi-CPU; trivially
single-CPU), `TaskWait` (needs the blocked-task wakeup machinery
that the IPC primitives already half-have), `TaskQuery` (small;
held back to bundle with TaskWait's state-word format).

### Bootstrap task and `_call_redirected_pc`

`init_cpu` ends with a call to `make_bootstrap_task`, which
allocates a TAG_TASK descriptor for the implicit "main"
execution context and snapshots the just-primed CPU state into
its `Task` struct. Single-task programs never touch this Task —
they `TaskExit` with no successor and the existing
`TaskExitSignal` tear-down runs. Multi-task programs see the
bootstrap as just another scheduler citizen.

The CALL dispatch site's `_installed_program` flag (set by
`InstallProgram` to suppress the post-CALL PC bump when the
primitive has already moved PC) was renamed to
`_call_redirected_pc` and reused by every context-switching
primitive. Same mechanism, more honest name.

### Pre-existing test casualty

`07_firmware/10_call_zero_enosys` asserted that `CALL #0`
returned `ENOSYS` because no primitive was defined at primitive
number 0. Vol VI §4.1 has always listed `0x000 = TaskCreate`,
so the test's premise was wrong even before Phase 26 — it just
happened to pass because the simulator hadn't implemented
`TaskCreate`. Test retargeted to `#0xFFF` (an unallocated number
in the task range). Now an honest test of the ENOSYS path.

### Validation

Four new tests in a new `14_tasks` category:

- **`01_create_resume_exit_chain`** — Bootstrap creates one
  child, resumes it, exits with code 0. Scheduler picks child;
  child exits with R4 = 42 (its init_r4). No more runnables →
  `TaskExitSignal(42)` → CPU exits 42. Proves
  TaskCreate/TaskResume/TaskExit cooperate.
- **`02_yield_roundtrip`** — Bootstrap creates a child and a
  shared scratch object (inherited by the child via the OPR
  copy in TaskCreate), yields. Child stores 0x42 at scratch[0],
  exits. Bootstrap resumes after the yield, reads scratch back,
  exits with what the child wrote. Proves yield → save → load
  → resume preserves register state and that per-task address
  spaces still resolve shared object refs to the same descriptor.
- **`03_task_current_returns_self`** — Two consecutive
  `TaskCurrent` calls return refs that `oeq` treats as equal
  (same descriptor index, same generation).
- **`04_three_way_yield`** — Bootstrap, A, and B run in the
  scheduled order with the FIFO queue evolving as
  `[A, B] → [B, bootstrap] → [bootstrap] → []`. Each task
  stamps a tag byte into shared scratch; bootstrap reads B's
  tag back as proof B ran. Tests the round-robin order.

124 → 128 sim validation tests passing. Asm + multiprocess +
wire-format + 10 device/shell tests all still green.

### What's not yet done

- **`TaskWait`, `TaskQuery`, `TaskBindProcessor`.** Need to land
  next; `TaskWait` blocks the caller until another task exits
  and surfaces the exit code, which is the obvious complement to
  the EXITED state already tracked.
- **Preemptive scheduling.** Cooperative only for now — a task
  that doesn't `TaskYield` runs forever. Real preemption needs
  the timer-interrupt path (`COUNT`/`COMPARE` control registers,
  external-interrupt cause delivery), which is its own phase.
- **Cross-CPU scheduling.** All tasks today live on the CPU
  whose `TaskCreate` allocated them. Migration is a multi-CPU
  concern; gated on `TaskBindProcessor`.
- **Task object reaping.** EXITED tasks stay in `cpu.tasks`
  until CPU reset. `TaskQuery` will need that data; eventually
  there'll be a reaping primitive.
- **Trap delivery in non-supervisor tasks.** Now that there's
  more than one task, the question of "whose VECBASE/handler"
  gets more interesting. Today VECBASE is per-CPU and
  unconditionally writes the handler with the trapping task's
  context — fine while there's one supervisor managing all
  tasks, but the architecture admits per-task handlers.

## Phase 27 — Wait, reap, supervisor handlers, timer (Ouroboros, day 4)

Phase 26 left four obvious gaps. Phase 27 closes them in a single
pass: the synchronization primitive a parent needs to harvest a
child (`TaskWait`), the storage-management primitive that lets
EXITED tasks not leak (`ObjFree` on `TAG_TASK`), the supervisor
escape hatch from firmware-only `VECBASE` (`InstallTrapHandler`),
and the asynchronous-trap path that timer-driven preemption rides
on top of (`STATUS.IE` + `COMPARE` + `external-interrupt`).

### `TaskWait` (#0x007)

`Task` gained a `waiters: List[Task]` field. `TaskWait`:

1. Validates `O1` as a task ref (no V cap required — Vol VI §4.1).
2. If the target is already `EXITED`, returns immediately with `R3
   = exit_code`.
3. Otherwise marks the caller `BLOCKED`, appends it to
   `target.waiters`, bumps PC past the CALL, and context-switches
   to the next runnable task. If nothing else is runnable, returns
   `EBUSY` instead — waiting would deadlock the CPU.

`TaskExit` gained a `_wake_waiters` pre-step that walks the
exiting task's waiters list, sets each waiter's `R2 = OK` and
`R3 = exit_code` directly in their saved Task struct (so the
values are visible after the next context-switch back), and
moves them `BLOCKED → RUNNABLE`. Then the regular `pick_next_runnable`
runs, possibly picking a just-woken waiter.

### Reaping via `ObjFree`

`ObjFree` is now type-aware for `TAG_TASK`: freeing a task
descriptor evicts the Python `Task` struct from `cpu.tasks`
alongside the descriptor itself. A live task can't be freed
(returns `EBUSY`) — that would silently strand the running
context or leave a dangling pointer in the runnable queue.

Means an Ouroboros pattern of `TaskCreate → TaskResume →
TaskWait → ObjFree` works as a clean reap primitive without
adding a new `TaskReap` syscall.

### `InstallTrapHandler` (#0x520)

Per-CPU `cpu.trap_handlers: Dict[cause, va]`. `deliver_trap`
consults this map *before* falling back to `VECBASE + offset`:
when a per-cause handler is installed, the trap delivers to that
VA in supervisor mode (firmware mode for the VECBASE fallback).
The handler runs in the trapping task's address space, can read
the architectural trap state via `LCTRL`, and returns by `ERET`.

Slot is `#0x520` rather than `#0x500` — the latter was already
spec-allocated to `GuestCreate`. Vol VI §9 picks up the new
primitive next to `SystemReset` / `SystemHalt`; the
"hypervisor and system management" range is the right home for
trap-routing primitives.

### Timer interrupt + `STATUS.IE`

`STATUS` got the bit-layout pinned down in Vol V §2.10:

| Bits      | Field        | Meaning                                       |
|-----------|--------------|-----------------------------------------------|
| `[1:0]`   | `MODE`       | Current privilege mode                        |
| `[3:2]`   | `SAVED_MODE` | Mode to restore on `ERET`                     |
| `[4]`     | `IE`         | Interrupt enable (gates `external-interrupt`) |

`COMPARE` (control register 6) is now actually backed.
`step()` checks at the top of each instruction: if `IE` is set
and `cycles >= compare > 0`, raise `Trap(CAUSE_EXTERNAL_INTERRUPT)`
which `deliver_trap` routes through whatever path is installed.
On delivery of `external-interrupt`, `IE` is auto-cleared so the
handler runs without immediately re-firing. The handler is
expected to re-arm `COMPARE` and re-enable `IE` before `ERET`ing.

The check is suppressed when the CPU is in a branch delay slot —
firing there would lose the branch's effect on control flow
because the simulator doesn't yet track the BD bit. We just defer
one instruction and fire on the branch target.

### Validation

Four new tests:

- **`14_tasks/05_taskwait_returns_exit_code`** — Bootstrap
  creates a child with `init_r4 = 0x37`, `TaskWait`s on it; child
  exits with `R4 = 0x37`; bootstrap wakes with `R3 = 0x37` and
  exits with that.
- **`14_tasks/06_objfree_reaps_exited_task`** — Premature
  `ObjFree` returns `EBUSY`; after `TaskWait`, `ObjFree` returns
  `OK`; a second `ObjFree` returns `ESTALE` (descriptor really
  freed).
- **`08_traps/15_supervisor_trap_handler`** — Bootstrap installs
  a handler for arithmetic-overflow at a code label, triggers
  the trap, handler reads STATUS to confirm supervisor mode
  (not firmware), reads CAUSE, exits with the cause code.
- **`14_tasks/07_timer_preemption`** — Bootstrap arms the timer,
  enables IE, drops into a tight loop reading a counter; the
  handler increments the counter, re-arms COMPARE, re-enables
  IE, ERETs back. After 5 fires the loop exits. Without the
  timer, the loop would never advance.

128 → 132 sim validation tests passing. All other suites green.

### What's not yet done

- **Yield from inside a trap handler.** A handler that does a
  voluntary `TaskYield` clobbers the trap's saved state (the
  context switch saves the handler's mid-execution PC into the
  task struct, which the next `ERET` will then misinterpret).
  Real preemptive scheduling — where the timer handler hands the
  CPU to another task — needs either (a) a deferred-yield
  mechanism (handler sets a flag, ERETs back, the next
  user-mode instruction yields), or (b) a "task-switch" variant
  of `ERET` that fully resolves the saved state. Today the
  timer can only do work that fits in the handler itself.
- **`TaskBindProcessor` and `TaskQuery`.** `TaskQuery` is small;
  `TaskBindProcessor` waits for cross-CPU scheduling.
- **BD bit / delay-slot trap handling.** Sync traps inside delay
  slots currently lose the branch effect on resume. Timer
  interrupts dodge this by deferring one instruction; sync traps
  don't have that luxury and need real BD machinery.
- **`saved-IE` rides ERET.** Real architectures save the IE bit
  alongside saved-mode and restore on ERET. Today the handler
  must explicitly re-set IE before ERETing, which works but is
  one more thing for the OS author to remember.

## Phase 28 — `task.c`: tasks reach C (Ouroboros, day 5)

Phases 24–27 built kernel mechanisms; nothing on the system used
them. Phase 28 lifts them into C: `tools/cc/lib/task.c` wraps
`TaskCreate` / `TaskResume` / `TaskYield` / `TaskCurrent` /
`TaskWait` / `TaskExit` plus the `ObjFree`-of-`TAG_TASK` reaping
path, all callable from pcc-compiled programs.

### API shape

MVP single-child design — each call operates on the task ref
parked in `O12` by `task_spawn`:

```c
void task_init(void);                      /* must be FIRST in main */
int  task_spawn(void (*entry)(int), int arg);
int  task_wait(void);                      /* returns child's exit code */
int  task_free(void);
void task_yield(void);
void task_exit(int code);                  /* never returns */
```

`task_init()` parks `O1` (boot code ref), `O2` (boot stack), and
`O3` (boot data) into `O13`/`O11`/`O15` — slot choices that match
the term.c boot-save convention so a future `term_init + task_init`
program gets one coherent set of saves regardless of init order.

`task_spawn` allocates a fresh stack via `ObjAlloc(TAG_STACK)`,
calls `TaskCreate` with the parent's code ref and the supplied
function pointer (converted to a byte offset off `CODE_VA`),
parks the new task ref in `O12`, restores `O2`/`O3` from
`O11`/`O15` so the caller's subsequent `print_str` / `print_int`
keep working, and `TaskResume`s.

Multi-child programs need to `omov` refs out of `O12` between
spawns. The libc will eventually grow a real task table and an
opaque `task_t` handle; the slot dance is enough for the first
demos.

### Demo: `examples/cc/multitask`

```c
void double_and_exit(int n) { task_exit(n * 2); }

int main(void) {
    int args[3] = {7, 11, 21};
    task_init();
    for (int i = 0; i < 3; i++) {
        task_spawn(double_and_exit, args[i]);
        int result = task_wait();
        print_str("child("); print_int(args[i]);
        print_str(") -> "); print_int(result);
        print_str("\n");
        task_free();
    }
    print_str("parent done\n");
    return 0;
}
```

Output:

```
child(7) -> 14
child(11) -> 22
child(21) -> 42
parent done
```

End-to-end through pcc → asmorisc → orld → simorisc, exercising
the whole Phase 24–27 stack from real C source.

### A bug found en route

The first run printed `child() -> ` (empty integers). Cause:
`task_spawn` was clobbering `O2` (writing the child's stack ref
through it on the way to `TaskCreate`) but never restoring it.
`console_write` reads stack-resident strings via `O2`, so
`print_int`'s `char buf[16]` ended up indexed against the *child's*
empty stack. Fix in the commit: save `O2`/`O3` to `O11`/`O15` at
`task_init` time and restore both in `task_spawn` after the
`TaskCreate` dance.

This is the kind of bug where having a real C-level demo immediately
caught what unit-tested asm would have missed. The supervisor-shell
work that's coming will surface more of these — the OR-hygiene
contract isn't fully captured by the type system yet.

### Validation

- New `tools/devices/tests/test_multitask.sh` builds and runs the
  demo, asserts on the exact stdout.
- All 132 sim validation tests still pass; libc rebuild still
  produces the same 6 modules + the new `task.oro`.

### What's not yet done

- **Multi-child API.** The `O12`-as-handle convention works for
  one child at a time. A real `task_t` handle backed by a libc-
  managed object table is the natural next step — the user wants
  to write code like `task_t kid_a = task_spawn(...);
  task_t kid_b = task_spawn(...); task_wait_any();`.
- **Shell as supervisor.** The current shell runs commands by
  forwarding to linkbootd, which spins them up on a separate
  pre-spawned CPU. Replacing that with `task_spawn` on the same
  CPU would turn the shell into a real OS supervisor — needs
  loading from disk into a code object (currently chunkboot does
  that), then `TaskCreate` over it. Phase 29 candidate.
- **Trap handlers from C.** `InstallTrapHandler` (#0x520) is
  still asm-only. A C-level wrapper plus a way to write the
  handler body in C (probably needs `__attribute__((interrupt))`
  or a shim that does the LCTRL/SCTRL/ERET dance) would let
  Ouroboros catch faulting tasks gracefully.

## Phase 29 — Multi-child task API (Ouroboros, day 6)

Phase 28 left the obvious gap: the `O12`-as-task-handle convention
worked for one child at a time, which made `task_spawn` look more
like `posix_spawn-then-wait` than `fork`. Phase 29 grows it into a
real handle API where the user can hold N children at once and
wait on them in any order.

### A `task_t` handle backed by an OREF table

`task_init` now `ObjAllocStore`s a 128-byte OR-typed storage
object (`TASK_MAX_CONCURRENT * 8` bytes — one OR ref per slot)
and parks it in `O12`. `task_t` is a small int that names a slot
in the table. Slot allocation: a `static unsigned int task_slots_in_use`
bitmap in regular memory, scanned linearly to find a free bit.
The OREF table itself never has to be searched — the bitmap is the
authoritative "in use" record.

```c
task_t kid_a = task_spawn(child_a, 7);
task_t kid_b = task_spawn(child_b, 11);
int    code_a = task_wait(kid_a);
int    code_b = task_wait(kid_b);
task_free(kid_a);
task_free(kid_b);
```

Up to 16 concurrent children. Bumping that just needs a bigger
table (and one more line in the bitmap-bit-count macro).

### OREFLD / OREFST switch dispatch

The architectural wart: `OREFLD`/`OREFST` take a 16-bit constant
offset, not a register. Slot index → offset would need a runtime
multiplication, but the offset has to be baked into the
instruction word. So `task_load_to_o1(slot)` and
`task_store_from_o1(slot)` are 16-case switches with one inline-asm
arm per slot. Verbose but mechanical, and pcc lowers them to a
chain of compare-branches that disappears under the cost of the
TaskCreate / TaskWait calls themselves.

### A linker bug found en route

First run of the new demo trapped `address-misaligned-d` at VA
`0x40007`. Cause: `orld` concatenated `.data` sections from
multiple `.oro` objects without padding between them, so the
`task.c` object's 4-byte-aligned `task_slots_in_use` global landed
at offset 7 in the combined section (right after a 7-byte
`"hello\n\0"` literal from another object). The `.align 2`
directive in `task.c`'s assembly aligned within its own object —
which doesn't help if the object itself starts unaligned in the
combined section.

Fix in `tools/ld/orld`: pad `data_cursor` to 4 bytes between
objects in step 1 (layout), and pad `data_bytes` to match in
step 4 (concatenation). 4 covers everything the current toolchain
emits; coarser per-symbol alignments would need section-alignment
metadata in the `.oro` format.

This is the kind of bug that's been lurking — earlier programs
had data layouts where the offset-7 collision didn't happen to
matter (or no cross-object globals at all). The new task table
forced it into the open.

### Concurrent demo

`examples/cc/multitask/concurrent.c`: parent allocates a 32-byte
shared scratch object, parks it in `O7` *before* spawning, then
spawns five children all at once. Each child reads its slot index
from the packed `R4` argument, stamps a value into
`scratch[slot]` via `OSB`, and exits. Parent waits on each in
turn, OLBUs the scratch byte back, prints. Without the OPR-
inheritance behaviour of `TaskCreate` (Phase 26), the children
wouldn't see `O7` at all — the demo doubles as a regression test
for that.

```
spawned 5 children
scratch[0] = 7
scratch[1] = 11
scratch[2] = 13
scratch[3] = 17
scratch[4] = 19
all done
```

### Validation

- Existing `multitask.c` (sequential) updated to the handle API;
  still passes its `test_multitask.sh`.
- New `examples/cc/multitask/concurrent.c` + `run-concurrent.sh`
  + `tools/devices/tests/test_concurrent.sh` — 12 device/shell
  tests now (was 11).
- 132 sim validation + 7 asm + 5 wire-format + multiprocess +
  hello all still green.

### What's not yet done

- **`task_wait_any` / `task_wait_first`.** The current loop
  pattern is `for each kid: task_wait(kid)`, which works (the
  earliest exit is collected first by short-circuit when its
  `TaskWait` finds the task already EXITED) but doesn't return
  control as eagerly as a real reaper would. A `wait_any`
  variant would scan the table, hand out the first EXITED
  child, and only block if none are ready.
- **Configurable stack size per task_spawn.** Hard-coded 4 KiB
  per child today.
- **Slot reuse across `task_free`.** Already works (the bitmap
  bit clears, next `task_spawn` picks the same slot), but the
  invariant — "the OREF slot reads as null after `task_free`" —
  is enforced by overwriting with `O0` rather than tracked
  separately. Fine but worth documenting if anyone ever pokes
  at the table directly.

## Phase 30 — Shell as supervisor (Ouroboros, day 7)

The "real OS" milestone. The shell's `run cmd` no longer
forwards to `linkbootd` to spin the guest up on a separate spare
CPU; it loads the `.orx` from disk itself, ObjAllocs code/data/
stack objects, and `TaskCreate`s the guest as a child task on the
**same CPU**. The shell is now a real OS supervisor — orchestrating
child tasks, catching their exit codes, and coexisting with them in
one address-space-per-task layout.

### `orx.c` libc loader

`tools/cc/lib/orx.c` adds `int orx_run(const char *path)`. It
opens the file via `hf_open`/`hf_read`, parses the 32-byte `.orx`
header, allocates and populates code + data + stack objects, and
spawns a child task with the standard CONTRACT.md §2 layout (code
at `CODE_VA`, data at `DATA_VA`, stack at `STACK_TOP`). Synchronous:
blocks until the child exits, returns its exit code.

The loader's working storage is an `ObjAllocStore`-backed
32-byte OR-typed scratch object parked in `O7` (the slot vacated
by removing `lb_spawn` from the shell). Slot offsets `0`/`8`/`16`/`24`
hold the code/data/stack/task refs across the long load sequence;
`OREFLD`/`OREFST` with constant offsets shuttle them in and out
of `O1` for primitive calls. The alternative — keeping refs in
named OR slots — would have stomped on `O5`/`O6` (terminal
services) since pcc doesn't track which OR slots are "owned" by
which subsystem.

### `simorisc` extensions

Two firmware additions:

- **`TaskCreate` accepts `O3` = data ref**. When non-null, the new
  task gets a `DATA_VA` mapping (`R+W`) for it alongside the
  existing code+stack mappings. Matches the CONTRACT.md §2 layout
  pcc-compiled programs assume. `O3 = O0` (null) preserves the
  previous behaviour for tasks that don't need globals.
- **`Unmap` (#0x111)**. Drops the mapping that starts exactly at
  `R4` with `R5` length. The simulator's mapping list is
  first-match-wins, so without `Unmap` the loader's temp
  `MapObject` at `0x300000` (used to populate the freshly-allocated
  code object) would shadow itself on the second `run`. `Unmap`
  + re-`MapObject` is the clean replace cycle.

### Bugs found en route

Two separate ones, both in the inline-asm wrappers and both
silent until shell-as-supervisor stress-tested them.

**(1) Sign-extending byte loads.** `beu32` was reading `.orx`
header bytes via `(unsigned int)(unsigned char)p[i]`. pcc's orisc
backend lowered the cast chain to a signed `lb`, which
sign-extended `0xc0` (a real byte in our `text_size` field) to
`0xFFFFFFC0` and OR'd it into the high bits of the result.
text_size came back as `-64`. Workaround: explicit `& 0xff`
after the load. (Real fix is in pcc; tracked.)

**(2) Inline-asm input-reg-vs-clobber-reg conflict.** `orx_alloc_into_slot`
had three input args (size, tag, caps) marshalled into r4/r5/r6
via three sequential `addu` instructions. pcc placed the inputs
in r4/r5/r6 themselves (despite them being in the clobber list);
the first `addu r4, %1, r0` overwrote whatever pcc had picked for
%3 (caps) since pcc had picked r4 for it. The third `addu r6, %3,
r0` then read `%3` from a register holding the just-overwritten
size value. The fix: write the addus in **reverse order** — read
%3 → r6 first, then %2 → r5, then %1 → r4 — so each pcc-chosen
input register is consumed before any of the others overwrites
it. Same pattern recurs throughout `orx.c` and is documented
with a leading comment.

### Object-cleanup story is incomplete

When the guest exits, its data segment may still be in flight in
`oriscterm`'s receive queue (a `term_print` SEND completes async;
the receiver issues `OBJ_READ_REQ` against the data ref some time
later). If `orx_run` immediately `ObjFree`s the data object, the
read fails — which is what the chunkboot path used to solve via
the `RESET_DRAIN_SECONDS` wall-clock window in simorisc.

`orx_run` doesn't have an equivalent. Today it intentionally
**leaks** the code/data/stack objects after the guest exits;
only the loader's scratch and the task descriptor are reaped.
Per-`run` overhead: ~80 KiB. Acceptable for a few interactive
runs; needs a real fix (drain primitive, or a "delayed free"
mechanism) for long-lived shells.

### Shell change

`cmd_run` was a one-line swap: `lb_spawn(path)` → `orx_run(path)`.
The shell still respects the `O7` slot in its `--service` line
(now `0=0@0`, since linkbootd is no longer needed for `run`).

`run_shell.sh` and the test runner drop the spare-CPU pool and
the `linkbootd` daemon from their setup. The shell is the only
CPU; oriscbar + hostfsd + oriscterm round it out.

### Validation

`tools/devices/tests/test_shell_run.sh` rewritten for the new
architecture. The guest is now a `print_str`-only `hello.c`
(firmware `ConsoleWrite` to host stdout — landing in the same
file the shell writes to — rather than a `term_print` SEND that
would compete with the shell for the keyboard subscription).
Expected stdout has 2 "hello from guest" lines; rendered
terminal has 2 `[exited 0]` markers.

term.c's keyboard receive-queue depth was bumped from 16 to 64
along the way — with shell and guest sharing one CPU, keystrokes
queue up while the guest runs, and 16 was tight even for a
two-`run`-then-`exit` test sequence.

132 sim validation + 7 asm + 5 wire-format + multiprocess +
hello + all 12 device/shell tests still green.

### What's not yet done

- **Object cleanup after `orx_run`.** Documented above. Needs a
  drain primitive or libc-side timed-free. Until then, each
  `run` leaks ~80 KiB.
- **Guest can't safely use term_print.** The keyboard
  subscription model conflicts: if the guest subscribes via
  `term_init`, it competes with the shell for keystrokes. A
  `term_print_only_init` (subscribe only to console, not
  keyboard) would let guests print to the terminal without
  hijacking input.
- **Backgrounded `run` (`&`).** `orx_run` is synchronous. An
  `orx_load_async` returning `task_t` would let the shell prompt
  return immediately while the guest runs in the background.
- **Standard I/O abstraction.** The "guest writes to host
  stdout" hack works for the test but isn't OS-shaped. Real
  Ouroboros wants some equivalent of Unix file descriptors that
  the supervisor can wire up per child.
- **Cross-CPU spawn.** `orx_run` hard-codes "spawn on this CPU."
  A `--cpu N` variant could parallelise.

## Phase 31 — Backgrounding + Tk-window guests (Ouroboros, day 8)

Two of Phase 30's "what's not yet done" items closed in one
sweep: guests can now write to the Tk window safely, and the
shell's `run` accepts a trailing `&` to spawn without waiting.

### `term_print_only_init`

[`tools/cc/lib/term.c`](tools/cc/lib/term.c) gains a strict
subset of `term_init`: just three `omov`s parking boot
`O2`/`O3`/`O4` into `O11`/`O14`/`O15`. No receive-queue attach,
no `R+S` derive, no `SEND` to subscribe to the keyboard. A child
that calls this can use `term_print*` (which lands in the parent's
oriscterm window via inherited `O5`) without grabbing keystrokes
out from under the parent shell.

### `orx_spawn` — async loader returning `task_t`

[`tools/cc/lib/orx.c`](tools/cc/lib/orx.c) split: `orx_spawn` does
the load, registers the new task in the libc task table via two
new task.c primitives (`task_register_o1` for OREFST-into-slot,
`task_resume(task_t)` for OREFLD-and-resume), and returns the
handle. `orx_run` becomes a thin wrapper:

```c
int orx_run(const char *path) {
    task_t t = orx_spawn(path);
    if (t < 0) return t;
    int code = task_wait(t);
    task_free(t);
    return code;
}
```

Backgrounded tasks live in the libc task table (in `O12`,
managed by `task_init`); the shell harvests them later via
`task_wait`/`task_free`. `task_init` had to move BEFORE
`term_init`/`hf_init` in the shell's startup so it could capture
the boot code ref from `O1` before anything clobbered it.

### `&` and `wait` in the shell

`cmd_run` parses a trailing `&` (with optional whitespace
before): if present, it `orx_spawn`s the task, prints
`[bg task N]`, then does one `task_yield` to give the child a
quantum to run before the shell blocks on the next keystroke. New
`wait <N>` command takes a task handle, calls `task_wait` +
`task_free`, prints `[task N exited C]`.

```
/> run /programs/hello_term.orx &
[bg task 0]
hello from inside the Tk window
/> wait 0
[task 0 exited 0]
```

The `task_yield` after `orx_spawn` is the entire current
preemption story: short-running children get to complete in that
quantum, after which the shell resumes when the child blocks on
its own I/O or `TaskExit`s. CPU-bound children that don't yield
voluntarily would freeze the shell — real preemptive scheduling
(timer-driven `TaskYield` from the trap handler) is still gated
on the trap-handler-yield architectural fix from earlier phases.

### Validation

- New [`hello_term.c`](examples/cc/programs/hello_term.c) demo:
  `term_print_only_init` + `term_print`, output lands in the Tk
  window between the shell's `[exited]` line and the next
  prompt.
- New `tools/devices/tests/test_shell_bg.sh` exercises the full
  `run X &` → `wait N` round trip and asserts on the rendered
  terminal.
- 12 → 13 device/shell tests; sim/asm/multiprocess/wire-format
  all unchanged at 132/7/1/5.
- The README [`examples/cc/programs/README.md`](examples/cc/programs/README.md)
  rewrote to cover the print_str-vs-term_print split and the
  backgrounding flow.

### What's not yet done

- **Object cleanup after `orx_spawn`.** Same leak as Phase 30 —
  ~80 KiB per spawn. The `&` flow inherits it. Needs the drain
  primitive.
- **Real preemption.** A long-running CPU-bound bg task still
  starves the shell. Timer-driven `TaskYield` from the trap
  handler clobbers the trap's saved state (see Phase 27
  caveats); needs a deferred-yield mechanism.
- **`jobs` listing.** No way to enumerate live bg tasks from
  inside the shell; user has to remember handles.
- **Auto-reap on exit.** When a bg task exits, its exit code
  sits in the table until `wait` collects it. A polling shell
  loop could auto-print "[task N done]" notifications.

## Phase 32 — `ObjFreeDeferred` closes the orx leak (Ouroboros, day 9)

The "what's not yet done" item from Phases 30 and 31 — every
`orx_spawn` leaked ~80 KiB because freeing the loaded code/data/
stack would race the guest's last async SEND being read by
oriscterm. Phase 32 adds the drain primitive that lets `orx_spawn`
actually free its objects after a configurable window.

### `ObjFreeDeferred` (#0x107)

Volume VI §5.1.2 picks up a sibling to `ObjFree`: same arguments
plus an `R4` drain delay in milliseconds (clamped to `[0, 60000]`).
The descriptor stays live and continues to answer `OBJ_READ_REQ`s
during the window, then is freed normally. Same idiom as the
chunkboot CPU's `RESET_DRAIN_SECONDS` window in simorisc, but
exposed as a callable primitive.

`#0x107` rather than the next-after-ObjFree `#0x102` because Vol
VI §5 already has `ObjRevoke` at `0x102` and `ObjMigrate` /
`ObjQuery` at `0x104` / `0x105`. `0x107` is the next free slot
in the object-lifecycle range.

`TAG_TASK` descriptors are explicitly **rejected** with `EINVAL` —
the immediate `ObjFree` path evicts the Python `Task` struct
synchronously, which the deferral path can't do safely (the task
might be referenced from `cpu.runnable` or `cpu.current_task`).
Tasks must be reaped immediately, not deferred.

### Run-loop integration

`CPU` gained `deferred_frees: List[(deadline, idx)]`. The run
loop scans it once per tick after the OBJ_READ_REQ drain and
before the per-CPU step, freeing any whose deadline has passed.

Pending deferred frees deliberately do NOT keep the loop alive
in idle mode — that would block clean shell shutdowns (the test
suite depends on this; a `run X &` followed by `exit` would
otherwise wait the full 30s drain window before sim exits).
Live CPUs already keep the loop turning via `any_progress`; once
everyone's idle, the host process is winding down anyway and
the OS reclaims everything.

### `orx.c` integration

`orx_spawn` now ends with three `ObjFreeDeferred` calls (code,
data, stack) using a 30-second drain window. Each loaded object's
descriptor stays live for 30 seconds after spawn, plenty of time
for the guest to run, exit, and have any in-flight async SENDs
fully consumed by oriscterm or hostfsd.

The 30-second window is the practical upper bound on how long an
interactive guest is expected to run; longer-lived guests would
have their data pulled out from under them. A future
`orx_unload(task_t)` that schedules the deferred frees only
*after* `task_wait` returns — when we know the guest has actually
exited — would handle the long-lived case. For now: 30 seconds
covers everything we run interactively.

### Validation

Two new tests in `05_oreg`:

- **`11_objfree_deferred`** — Allocate an object, schedule a
  0-ms-deferred free, burn cycles to let the run loop's scan
  process it, then call `ObjFree` on the same ref and assert
  `ESTALE` (the descriptor is gone).
- **`12_objfree_deferred_rejects_task`** — Try to `ObjFreeDeferred`
  a `TAG_TASK` ref, expect `EINVAL`.

132 → 134 sim validation tests passing. Asm + multiprocess +
wire-format + 13 device/shell tests all still green; in
particular `test_shell_run` and `test_shell_bg` now exercise the
real free path (was leak-and-let-the-OS-clean-up).

### What's not yet done

- **`orx_unload` for long-lived guests.** The 30-second window
  is enough for typical interactive use but not principled.
  An explicit `orx_unload(task_t)` that schedules the deferred
  frees after `task_wait` would handle arbitrary run times.
- **Ref counting.** The drain window is wall-clock, not
  arrival-driven. A receiver that takes >30s to drain still
  loses bytes. Real ref counting would need bookkeeping in the
  simulator's request-processing path.
- **Guest-callable.** `ObjFreeDeferred` is `MODE_USER` so
  anyone can call it; that's mostly fine since it can't free
  someone else's task. Long-term it might want to be capability-
  gated (require a specific cap on the ref).

## Phase 33 — `orx_unload`: post-exit cleanup (Ouroboros, day 10)

Phase 32's drain primitive worked, but the timer started at
spawn time with a generous 30 s window — long enough for typical
interactive guests but not principled. A guest running >30 s
would have its data segment pulled out from under it. Phase 33
fixes this by deferring the timer to **after** the guest exits.

### Per-task manifest in the persistent state

`orx.c` no longer ObjAllocStores a fresh scratch on each spawn
and frees it on return. Instead:

- `orx_state_init()` — lazy, idempotent. ObjAllocStores a
  408-byte OR-typed object on first call, parks in `O7`, sets
  the static `orx_state_initialized = 1`. Subsequent calls noop.
- Layout of the state object:
  - bytes `0..23`: scratch slots (`SLOT_CODE`/`SLOT_DATA`/`SLOT_STACK`)
    used during the load — overwritten on each spawn.
  - bytes `24..407`: per-task **manifest** — 16 entries × 24
    bytes each, indexed by the libc `task_t` handle. Each entry
    holds the loaded code/data/stack refs at offsets +0/+8/+16.

`orx_spawn` ends by OREFLDing the three scratch refs into
`O1`/`O2`/`O3` and OREFSTing them into `manifest[t]` via a single
`manifest_save(t)` switch. No deferred-frees scheduled at spawn —
the loaded objects live as long as the task does.

### `orx_unload(task_t t)`

```c
int orx_unload(task_t t) {
    int code = task_wait(t);            /* block until guest EXITED */
    if (code < 0) return code;
    manifest_load(t);                   /* OREFLD m[t] → O1/O2/O3 */
    freedef_o1();                       /* ObjFreeDeferred(O1=code, 1500ms) */
    asm("omov o1, o2"); freedef_o1();   /* O1 = data,  ObjFreeDeferred */
    asm("omov o1, o3"); freedef_o1();   /* O1 = stack, ObjFreeDeferred */
    manifest_clear(t);                  /* OREFST O0 (null) → m[t] */
    task_free(t);
    return code;
}
```

Drain delay shrinks from 30 s (start of spawn) to **1.5 s**
(post-`task_wait`), which is plenty for any in-flight `OBJ_READ_REQ`
to drain — `task_wait` already proved the guest finished sending.
Long-lived guests are no longer at risk.

### Manifest helpers (mechanical 16-case switches)

`OREFLD`/`OREFST` take literal 16-bit offsets so a runtime `task_t`
has to dispatch through a switch. Three helpers — `manifest_save`,
`manifest_load`, `manifest_clear` — each with one case per task
slot. Each case is a single asm block doing all three OREFs at
once for that slot. Verbose-but-tidy: 48 cases total, each one line.

### `orx_run` collapses to two calls

```c
int orx_run(const char *path) {
    task_t t = orx_spawn(path);
    if (t < 0) return t;
    return orx_unload(t);
}
```

The shell's `cmd_wait` swapped its `task_wait` + `task_free` pair
for `orx_unload`. Safe on tasks that weren't orx-spawned (manifest
entries are null → `ObjFreeDeferred(null)` returns `EFAULT`,
which we swallow silently). Means user can `wait <N>` on any task
without knowing whether it was `orx_spawn`'d.

### Validation

134 sim validation + 7 asm + 13 device/shell + others all still
green. `test_shell_run` and `test_shell_bg` now exercise the
post-exit cleanup path through `orx_unload`. No new tests — the
existing flow covers it end-to-end.

### What's not yet done

- **State object on `--reset-on-exit`.** A reset CPU wipes its
  descriptor table; `orx_state_initialized` (in C memory) stays
  truthy but the state object is gone, so subsequent
  `orx_spawn`s would crash on first `OREFST`. Not a real issue
  for the current use case (the shell never resets), but worth
  noting.
- **Manifest size = `TASK_MAX_CONCURRENT`.** Hardcoded to 16.
  If the task table ever grows, the orx manifest needs to grow
  too (and the switch tables need more cases).
- **Backgrounded shell `wait` doesn't auto-yield.** Same caveat
  as Phase 31 — CPU-bound bg tasks freeze the shell. Real
  preemption is still gated.

## Phase 34 — `jobs` + auto-reap: backgrounding feels real (Ouroboros, day 11)

The Phase 31 backgrounding worked but was clunky: spawn with
`&`, then user has to remember the task number, then `wait` to
harvest the exit code. Phase 34 adds the two pieces that turn
this into a recognizable Unix-y shell: a `jobs` listing and an
auto-reaper that prints `[task N done CODE]` whenever a bg task
exits, before the next prompt.

### `TaskQuery` (#0x008)

Volume VI §4.2's `TaskQuery` was the obvious primitive for both
features — it's the non-blocking inspection counterpart to
`TaskWait`. Returns a packed state word in `R3`: state in low
8 bits, processor id in next 8, exit code in upper 16 (only
meaningful when state == EXITED). Restartable, no V cap
required (matching `TaskWait`'s spec).

Wired in simorisc as the canonical spec slot — first time we got
a primitive number right on the first attempt without colliding
with something already allocated.

### libc bindings

`task.c` gains two functions and an unpacked struct in
`liborisc.h`:

```c
struct task_info {
    int state;       /* TASK_STATE_* */
    int processor;
    int exit_code;
};

int          task_query(task_t t, struct task_info *out);
unsigned int task_active_mask(void);  /* in-use slot bitmap */
```

`task_active_mask` exposes the libc's internal `task_slots_in_use`
bitmap so callers can iterate the table without trial-and-error
calls. `TASK_STATE_*` constants moved into the header so user
code can reference them by name.

### Shell `jobs` + auto-reaper

`cmd_jobs` walks the active mask, calls `task_query` on each
live slot, prints `[task N] state-name (exit C)` per entry. If
the mask is empty: `(no live tasks)`.

`reap_exited_tasks` runs at the top of each prompt iteration —
same scan, but for any slot with `state == EXITED` it calls
`orx_unload` (proper cleanup of code/data/stack via deferred
free) and prints `[task N done CODE]`. The slot frees up for
the next spawn. Mirrors bash's "[1] Done" notification.

The interaction: a guest you `&`-spawned gets harvested before
the next prompt without the user lifting a finger. `wait <N>` is
still there for the rare case where you want to block on a
specific task synchronously, but most of the time the
auto-reaper handles it.

### test_shell_bg → test_shell_jobs

The Phase 31 `test_shell_bg.sh` typed `wait 0` after `run X &`
and asserted on `[task 0 exited 0]`. With auto-reap, task 0 is
gone by the time `wait` lands — the test now asserts on
`[task 0 done 0]` (the auto-reaper's message) instead.

New `test_shell_jobs.sh` exercises the full sequence: spawn,
auto-reap, `jobs` shows `(no live tasks)`. 13 → 14 device/shell
tests.

New `14_tasks/08_taskquery_packed_word` validates the primitive
itself: spawns a child that exits with `R4 = 0x42`, queries the
EXITED descriptor, asserts state=5 (EXITED) and exit_code=0x42
out of the packed word. 134 → 135 sim validation tests.

### What's not yet done

- **Per-job background metadata.** The shell only knows about
  tasks via the libc table — no name, no spawn time. `jobs`
  shows `[task 0] runnable` rather than `[1]+ Running run hello.orx &`.
  A side table keyed by `task_t` could carry the name string.
- **`jobs` enumeration race.** Auto-reap fires before each
  prompt; `jobs` runs after. Practically, you'd never see a
  live bg task in `jobs` because it would have been reaped
  first. Acceptable for an MVP — `jobs` is mostly a "did
  anything I think is alive go away" check.
- **CPU-bound bg tasks**. Same caveat as Phase 31 — real
  preemption is gated on the trap-handler-yield architectural
  fix.

## Phase 35 — Real preemption: deferred yield from trap handlers (Ouroboros, day 12)

The architectural blocker since Phase 27 was that a timer
handler couldn't safely call `TaskYield`: the immediate context
switch saved the handler's mid-execution PC into the outgoing
task's struct, so on resume the task would re-enter the handler
mid-instruction (and then ERET would land at stale `saved_pc` /
`saved_mode` because `cpu.saved_*` had been overwritten by the
incoming task's own trap state, if any). Phase 35 fixes this
with a deferred-yield mechanism, unblocking honest preemptive
scheduling.

### The fix

Three changes to the simulator, no spec change:

1. **`deliver_trap` checkpoints user state into the task struct.**
   Before overwriting `cpu.pc` / `cpu.mode` with the handler entry,
   `save_cpu_to_task(cpu, cpu.current_task)` copies the trapped
   task's full register file, mappings, and trap-side ctrl regs
   into its `Task` struct. From this point on, the task struct is
   a valid resume image of the user task; the handler can clobber
   `cpu.gpr` freely.
2. **TaskYield-from-handler sets `yield_pending` instead of
   context-switching.** New `cpu.in_trap_handler` flag (set by
   `deliver_trap`, cleared by `ERET`) tells `primitive_TaskYield`
   to take the deferred path: just set `cpu.yield_pending = True`
   and return `OK`. The handler's call returns immediately so it
   can finish its work and `ERET` normally.
3. **`ERET` honors `yield_pending`.** Before exiting trap-handler
   context, `ERET` reflects any handler-side `SCTRL` of EPC /
   STATUS into `current_task.{pc, next_pc, mode}` so the resume
   image stays current. Then:
   - If `yield_pending`: pick the next runnable task (without
     re-saving cpu state — the outgoing task's struct is already
     clean from the trap-entry checkpoint), `load_task_to_cpu` the
     incoming task, mark the outgoing one runnable.
   - Otherwise: `load_task_to_cpu(current_task)` to restore user
     GPRs/OPRs the handler may have clobbered, then override
     `cpu.pc` / `cpu.mode` from the (possibly SCTRL-modified)
     `cpu.saved_pc` / `cpu.saved_mode`.

The asymmetry in the yield path — load the new task without
saving the old — is the key insight. `deliver_trap` already saved
the user state, and the handler's mid-execution state isn't worth
preserving (the handler is done; it's exiting via ERET).

### Bonus: trap handlers no longer leak GPR state

A side effect of (3): handlers can no longer accidentally clobber
the trapped task's GPRs. Phase 27's tests carefully avoided
cross-clobber by partitioning register usage between handler and
main loop. With full restore, that discipline is no longer
required — a real OS pattern.

### `14_tasks/09_preemptive_yield_via_timer`

The validation test that proves preemption works:

- `main` and `child` share a 4-byte scratch object (parked in
  `O7` before TaskCreate so child inherits it via OPR copy).
- `main` creates and resumes child, installs a timer handler,
  arms `COMPARE = COUNT + 50` and enables `IE`, then drops into
  a tight `olw + beqz` loop polling `flag = scratch[0]`.
- `main` never voluntarily yields. Without preemption the loop
  runs forever and `child` never runs.
- Timer fires → handler re-arms timer, re-enables IE, calls
  `TaskYield` (sets yield_pending), `ERET`s.
- `ERET` sees `yield_pending`, switches to `child`. `child`
  writes `flag = 0x37` and `TaskExit`s.
- Scheduler picks `main` (only runnable). `main` resumes its
  loop, sees `flag = 0x37`, exits with that.

135 → 136 sim validation tests. All other suites unchanged.

### What's now unblocked

CPU-bound bg tasks no longer freeze the shell — once the timer
is wired into the shell, the auto-reaper would notice exits and
the supervisor stays responsive even when guests don't yield
voluntarily. (Wiring the timer into the shell itself is a
separate small change; this phase just makes the architecture
support it.)

### What's not yet done

- **Shell isn't wired with a timer yet.** The mechanism is there
  but the shell doesn't install a timer handler / arm COMPARE /
  enable IE. A small follow-up adds a "supervisor preemption
  tick" so CPU-bound bg tasks don't starve interactive use.
- **Preemption while `blocked_on`.** The shell blocking on
  `term_getkey` (ReceiveQueuePoll) parks the CPU; bg tasks
  don't run during that time even with preemption installed.
  The scheduler would need to switch to runnable tasks when
  `current_task` is BLOCKED. Real OS pattern; future work.
- **Saved-IE rides ERET.** Phase 27 caveat still applies: the
  handler explicitly re-sets IE before ERETing rather than the
  saved-IE bit riding back automatically. Cosmetic.

## Phase 36 — Timer in the shell, blocked-task preemption (Ouroboros, day 13)

Phase 35 made preemption *architecturally* possible — a timer
handler can call `TaskYield` and ERET will honor it on the way
out. Phase 36 cashes that in: the shell installs the timer at
boot, and the scheduler stops getting stuck on a blocked
`current_task` when other tasks are runnable. Together, those are
the two pieces a real interactive supervisor needs to stay alive
under load.

### Wiring the timer into the shell

Three small additions:

1. **`tools/cc/lib/preempt_handler.s`** — generic timer-interrupt
   handler. Re-arms `COMPARE = COUNT + 5000`, re-enables `STATUS.IE`
   (cleared by `deliver_trap` for cause `0x01`), calls `TaskYield`
   (which Phase 35 turns into `yield_pending` from a trap context),
   and `ERET`s. The ERET path notices the flag and switches to the
   next runnable task before resuming user mode.
2. **`task_install_preempt_timer(quantum)`** — libc helper that
   `InstallTrapHandler`s the above for cause `0x01`, arms
   `COMPARE = COUNT + quantum`, and sets `STATUS.IE`. Caller must be
   in supervisor mode. The shell calls this once at boot with a 5000-
   cycle quantum.
3. **`tools/cc/lib/build.sh`** now also sweeps `*.s` files in
   `tools/cc/lib/`, so the assembler-only handler ends up in
   `liborisc.ora` alongside the C-compiled members and the linker
   pulls it in only when something references the symbol.

### Trap-handler address-space fix

Wiring the timer surfaced a subtle issue. The shell's preempt
handler lives at a VA mapped in the *shell's* address space, but
when the timer fires while a guest task is running, the CPU is
using the guest's mappings — the handler's VA isn't there, so the
first instruction of the handler took a `tlb-miss-i`.

The fix: `primitive_InstallTrapHandler` snapshots
`cpu.mappings` into a per-cause `cpu.trap_handler_mappings[cause]`
at install time. `deliver_trap`, when it routes to an installed
supervisor handler, swaps that snapshot in for the duration of the
handler. ERET's `load_task_to_cpu` then restores the trapping
task's mappings as part of its normal job.

This is the simulator's stand-in for what real hardware would do
with an MMU + privilege boundary: the handler runs in its own
address-space view, and the user task's view is restored on return.

### Blocked-task preemption

The other half of "preemption" is what happens when the *current*
task can't make progress — say the shell calls `term_getkey`, which
parks it on the kbd queue waiting for a SEND. Before this phase, a
single-CPU configuration would just sit on that `CALL`, and any
runnable peer (e.g., a backgrounded CPU-bound task) wouldn't run
until the queue got something.

Now: when `_try_unblock` can't satisfy `cpu.blocked_on` and there's
another runnable task on this CPU, `save_cpu_to_task` checkpoints
the blocked task (its `blocked_on` rides along into the Task struct
via Phase 36's `Task.blocked_on` field) and `load_task_to_cpu`
swaps in the next runnable. When the original condition eventually
becomes satisfiable, `_wake_blocked_tasks` finds the BLOCKED task,
promotes it back to RUNNABLE, and the scheduler picks it up; on
its next dispatch the saved `blocked_on` is reinstated and
`_try_unblock` delivers the response.

The split between "wake the blocked task" (queue/response check)
and "deliver the unblock side effects" (R2/R3 writes, PC advance)
falls out naturally: the latter only happens when the task is
actually scheduled, so it can't race the saved CPU state.

### Tests

- **`14_tasks/10_blocked_task_yields_cpu`** (sim validation) —
  single CPU, two tasks. Main allocates a service object, attaches
  a queue, parks itself on `ReceiveQueuePoll` with infinite
  timeout. Without blocked-task preemption that's a deadlock —
  child can't run, queue stays empty forever. With it, the
  scheduler switches to child, child SENDs `0x55` to the queue,
  child exits, scheduler reschedules main, `_wake_blocked_tasks`
  promotes it, `_try_unblock` delivers the message, main exits with
  `0x55`. 137 sim validation tests.
- **`tools/devices/tests/test_shell_preempt.sh`** (integration) —
  end-to-end proof that the shell stays responsive under a CPU-
  bound bg task. The test spawns `spinner.orx &` (a 5,000,000-
  iteration tight loop, much longer than the test's wall-clock
  budget), then types `pwd`. The rendered terminal is asserted to
  contain `/` on a line by itself — meaning the shell got the CPU
  back from the spinner, processed the keystrokes, and printed
  `cwd`. Without the timer, the spinner would hold the CPU and
  `pwd` would never reach the prompt.

### What's not yet done

- **Saved-IE rides ERET.** Still cosmetic — the handler explicitly
  re-sets `STATUS.IE` rather than the saved-IE bit auto-restoring.
  Phase 27 caveat carries forward.
- **Quantum is a magic number.** 5000 cycles is hard-coded in
  `preempt_handler.s` and at the shell's call site. A small
  follow-up would either thread it through the handler or make it
  a libc configuration knob.

## Phase 37 — `kill <task>`: closing the supervisor loop (Ouroboros, day 14)

A backgrounded `run spinner.orx &` is unkillable in the previous
shell. The spinner gets preempted (Phase 36), the prompt stays
responsive (Phase 36), but the slot stays held until the spinner
voluntarily exits — which a tight loop never does. Phase 37 closes
that loop with an external-termination primitive and a one-line
shell command on top of it.

### `TaskKill` (#0x00A)

A new task primitive: external termination by ref. `O1` names the
target, `R4` carries the exit code, `R2` returns status.
Semantics:

- Marks the target `EXITED` with the supplied code.
- Removes it from `cpu.runnable` if it was queued there, and clears
  any saved `blocked_on` so a stale `_try_unblock` can't fire on a
  corpse.
- Wakes anyone parked in `TaskWait` on the target via
  `_wake_waiters` (same machinery `TaskExit` uses on its own
  termination), seeding their `R2`/`R3` so they observe the kill
  code as if the target had exited normally.
- Idempotent: killing an already-`EXITED` task returns `OK` so a
  shell can race a `wait` against a `kill` without erroring.
- Self-kill is rejected with `EINVAL` — `TaskExit` is for that.
  Killing across processors is rejected with `EREMOTE`.

The descriptor stays valid (the killer's `task_t` ref is still
live) so `TaskQuery` / `TaskWait` work post-kill the same way they
work post-`TaskExit`. Reclamation goes through the usual parent-
side path: `task_wait` (immediate, since target is already
`EXITED`), `ObjFreeDeferred` for code/data/stack, then `task_free`
to release the libc slot. `orx_unload` already wraps that, so the
shell's auto-reaper picks killed tasks up the same way it picks up
voluntarily-exited ones.

### libc + shell

A thin `task_kill(task_t t, int code)` libc helper looks up the
slot's ref (`OREFLD` from the table parked in `O12`), sets `R4`,
and `CALL #0x00A`. Runs in user mode — it's a `MODE_USER`
primitive, since the authority to kill comes from holding a
ref-with-`V`, not from any privilege bit.

The shell adds `cmd_kill`:

```
/> run spinner.orx &
[bg task 0]
/> kill 0
[task 0 done 137]
/>
```

Exit code 137 = 128 + 9, mirroring the POSIX shell convention for
SIGKILL — a small wink to the analogy without claiming Object RISC
has signals. `cmd_kill` itself prints nothing on success; the
auto-reaper at the top of the next prompt iteration prints the
standard `[task N done CODE]` line, so the kill path looks
identical to a voluntary exit from the user's perspective.

### Tests

- **`14_tasks/11_taskkill_marks_exited`** (sim validation) — main
  spawns a child whose body is `j spin; nop`. The killer issues
  `TaskKill` with `R4 = 0x42`, then a second one (idempotent
  no-op), then `TaskWait`s on the corpse and exits with the
  recovered code. **138 sim validation tests.**
- **`tools/devices/tests/test_shell_kill.sh`** — bg a 5M-iteration
  CPU-bound spinner, `kill 0` it, and assert the auto-reaper
  prints `[task 0 done 137]`. Without `TaskKill` the shell would
  be stuck with the spinner runnable until either it finished or
  the test timed out.

### What's now unblocked

- A user can clean up after themselves. `run thing &` is no
  longer a one-way commitment.
- The shell can grow a `Ctrl-C` story — same primitive, just
  trigger it on a kbd interrupt. (Not in this phase: needs a
  way to map a keystroke to "kill the foregrounded task," which
  in turn wants a notion of "foreground" that the shell doesn't
  have yet.)
- Future signal-style work — `TaskSignal`, custom handlers — would
  use the same shape (`O1` = target, `R4` = signal code), but is
  much more invasive than `TaskKill`'s "just mark it dead." Out of
  scope for now.

## Phase 38 — Grid plumbing + a full-screen viewer (Ouroboros, day 15)

A real terminal UI starts feeling possible once you can paint
character cells at arbitrary `(col, row)` positions. We've had
the grid service in oriscterm since the early graphics work but
nothing on the CPU side touched it. Phase 38 wires the grid into
liborisc and builds the first full-screen app on top: a
`view <path>` shell builtin.

### Wiring

- **Canvas resized to 80×24.** Was 80×16 with 24 rows of stream
  text on top — fine for a small graphics demo, cramped for an
  app. The text pane is unchanged; the canvas now matches it.
- **`liborisc` gains `grid_print` / `grid_print_n` / `grid_clear`.**
  Same async pull-based dance as `term_print`: we SEND to the grid
  service in `O7`, the terminal `OBJ_READ_REQ`s the bytes, and
  drops them at `(col, row)` on the Canvas.
- **Boot ABI: `O7 = oriscterm grid` (idx 3).** The shell's
  `run_shell.sh` and the test launchers all carry the new spec.
  The original plan was a separate `O8 = vector` slot for
  `VEC_CLEAR`, but `hf_init` already claims `O8` for its private
  mailbox — caused a mysterious "no idx=4 SENDs" symptom during
  bring-up. Rather than reshuffle libc's slot accounting, we
  fold clear-all into the grid service: a `SEND` with
  `col == row == -1` (sentinel) wipes the canvas, and the
  oriscterm side honours it. One ref does both jobs.

### `cmd_view`

The shell builtin. Reads the file into an 8 KB stack buffer,
indexes line starts (cap 512 lines), and paints a window of it
on the grid. 23 content rows + 1 status row showing
`view: <path>  <line>/<n>  q=quit`. Navigation:

```
j / DOWN          one line down
k / UP            one line up
SPACE             page down
b / BACKSPACE     page up
g / G             top / bottom
q / Q / ESC       quit
```

Files larger than the buffer (or files with > 512 lines) get a
`(truncated)` tag in the status. Lines longer than 80 columns
are clipped at column 80 — both honest about what's shown
without trying to be clever. The viewer wipes the canvas on
quit so the shell prompt isn't sitting next to a stale frame.

A pcc note: the orisc backend only passes the first four args in
registers and doesn't yet spill arg 5+ to stack, so the render
helper takes a `struct view_state *` instead of six positional
arguments. Same trick we used in Phase 38's grid library wrappers.

### `fake_terminal` learns the grid

The test fake-terminal previously only modelled the console (idx 1),
keyboard (idx 2), and pointer (idx 6) services. Phase 38 adds:

- An 80×24 in-memory cell grid.
- The grid SEND handler (with the same `OBJ_READ_REQ` pull dance
  as console).
- The clear-sentinel handling.
- A `grid_last_frame` snapshot stashed before each clear, since
  full-screen apps wipe on exit and the final post-quit grid is
  empty.
- A grid + last-frame dump in the trailing render block.

This makes `test_shell_view.sh` directly assertable against the
exact characters that landed on the canvas.

### Tests

- **`tools/devices/tests/test_shell_view.sh`** — types
  `view greeting.txt`, then `q`, then `exit`. Asserts the last-
  frame snapshot has the file's three lines on rows 0..2 and the
  expected status line on row 23, and that the shell returned to
  its prompt afterwards. **17 device/shell tests, all pass.**
  The 138 sim validation tests are unaffected.

### What's not yet done

- **Lines wider than 80 cols are clipped, not wrapped.** For a
  viewer this is fine; an editor will probably want a horizontal
  scroll mode.
- **Cell tracking on the oriscterm side.** Right now overlapping
  paints stack visually — a third paint at the same `(col, row)`
  draws on top of the previous two. The viewer dodges this by
  always preceding a frame with `grid_clear`. The eventual
  editor will repaint constantly and want proper cell-replace
  semantics so brute-force clear-and-redraw doesn't flicker.
  Earmarked for Phase 39.
- **Mouse on the canvas.** The pointer service exists. The
  viewer doesn't use it. Could be neat (click-to-scroll), not
  urgent.

## Phase 39 — A small full-screen editor (Ouroboros, day 16)

Phase 38 built the read-only viewer; Phase 39 turns the same
canvas into something you can type into. `edit <path>` in the
shell is a modeless, nano-style editor — printable keys insert,
arrows move, `^S` writes, `^X` quits. The supporting work is
two pieces of plumbing: the oriscterm grid finally tracks cells,
and the libc was already enough to express the rest.

### oriscterm grid: per-cell text-item tracking

Phase 38's grid handler created a fresh `create_text` Canvas item
for every paint, with the explicit caveat that overlapping paints
were the caller's problem. Fine when the only client was a viewer
that always preceded a frame with `grid_clear`. Not fine for an
editor that repaints after every keystroke — successive frames
would visually pile up.

Now the grid handler keeps a `(col, row) → Canvas item id` dict.
Each painted character either replaces the existing item at that
cell or adds a new one. A space is treated as "clear this cell"
(no new item is added). `grid_clear` and `VEC_CLEAR` wipe the
dict in lockstep with `canvas.delete("all")`.

Text-item count therefore stays bounded by what's actually shown,
and the editor can blast a full 80×24 frame on every keystroke
without flickering or leaking items.

### `cmd_edit`

A self-contained shell builtin. State lives in a `struct
edit_state` on `cmd_edit`'s stack:

- `lines[100][96]` — the buffer (≈9.4 KB).
- `line_lens[100]` — per-line length.
- `n_lines`, `cur_row`, `cur_col`, `top_row`, `dirty`,
  `truncated`, `path`.

Operations are direct array manipulations — no rope, no gap
buffer, no virtual EOL. Insert shifts a line's tail right by one
byte; backspace at column 0 merges with the previous line by
copying bytes and shifting subsequent lines up; ENTER splits the
current line at the cursor and shifts later lines down.

Rendering each frame:

1. `grid_clear()` (single SEND with the col=row=-1 sentinel).
2. One `grid_print_n` per visible line (≤ 23 SENDs).
3. A single `_` painted at the cursor cell so the user can see
   where the next insert will land. The cursor glyph overlays
   whatever character is under it — accepted limitation, fine
   for a first cut.
4. Status line at row 23: `edit: <path> [*] L,C   ^S=save ^X=quit`.
   The `*` flips on at the first edit and clears after a
   successful save.

Save uses `hf_open(... HF_O_WRONLY|CREAT|TRUNC)` then one
`hf_write` per non-empty line plus a `\n`. No atomic-rename
dance; corruption on a mid-save crash is theoretically possible
but unlikely on a single-host integration.

The editor is constrained to 100 lines × 95 chars. Larger
files load up to the cap with the tail dropped (and a
`(truncated)` tag in the status). Same convention as the
viewer — honest about the buffer rather than partial-loading
silently.

### `fake_terminal` already does cell tracking

The grid model in `fake_terminal` is a 2-D `bytearray[24][80]`
indexed by `(col, row)`. Overwriting a cell just rewrites the
byte — implicit cell-replace semantics. So no work needed there
for Phase 39. The `grid_last_frame` snapshot machinery from
Phase 38 still gives the editor test a reliable view of what
was on the canvas before the wipe-on-quit.

### Tests

- **`tools/devices/tests/test_shell_edit.sh`** — pre-creates a
  two-line file, opens it via `edit`, navigates to the end of
  line 2 with one DOWN + many RIGHT presses, types `!`, hits
  `^S`, hits `^X`, then exits the shell. The post-test asserts
  read the file back off the host filesystem and verify both
  lines are intact and line 2 ends with `!`. The grid
  last-frame snapshot also confirms the status line was painted
  with the right path.
- All 18 device/shell tests pass; **138 sim validation tests**
  unaffected.

### What's not yet done

- **No undo, no search, no copy/paste, no horizontal scroll.**
  All natural follow-ups; none in scope here.
- **Cursor obscures the character under it.** A vector-service
  rectangle behind the cell would solve this cleanly, but
  vector lives at idx 4 — and `hf_init` has already claimed
  `O8`. Either grow the boot-ABI slot count or fold a cursor
  primitive into the grid service the same way `clear` was.
- **Save is not atomic.** A crash mid-write would leave the
  file truncated. Trivial to fix with a write-temp-and-rename
  once `hostfsd` grows a `rename`.
- **Long-running typing might want preempt-aware drain.**
  Phase 36 keeps the shell responsive against bg tasks; the
  editor's full-frame repaint is a few dozen SENDs per
  keystroke, which is well below the timer quantum.

## Phase 40 — Editor as a backgrounded program + focus switching (Ouroboros, day 17)

The Phase 39 editor was a shell builtin: cmd_edit ran in the
shell's task. That meant the shell was blocked while editing,
and there was no separation between "what's running where". Phase
40 breaks edit out into its own `.orx` and adds a hotkey to
switch keyboard focus between the shell (upper text pane) and
whichever app owns the lower grid canvas.

### oriscterm: F1 cycles kbd focus

When more than one program subscribes to the keyboard service,
the terminal now routes each key to a single *focused*
subscriber. The F1 hotkey cycles the focus index; F1 itself is
always consumed by oriscterm, never delivered. Title bar shows
`kbd focus N/M (F1 to cycle)` whenever there's more than one
subscriber. With one subscriber (the common case), the hotkey
is a no-op and behaviour is unchanged.

### `examples/cc/programs/edit.c`

Same body as the old cmd_edit, repackaged as a standalone
guest. Calls full `term_init()` (including the kbd subscribe
SEND) so it shows up as oriscterm subscriber #2 alongside the
shell. Hardcoded to a fixed scratchpad path (`/scratch.txt`)
since the shell still doesn't pass argv to guests — that's
Phase 41 material.

```
/> run /programs/edit.orx &
[bg task 0]
/>                       ← shell still has focus, type away here
                          press F1 →
[ canvas now shows the editor; type goes to it ]
                          ^S ^X to save+quit, F1 to return to shell
[task 0 done 0]
```

### Multi-instance plumbing fixes

Two bugs surfaced as soon as a second program tried to use
oriscterm + hostfsd at the same time as the shell:

1. **`term_init` shared O4 with the parent.** Both shell and
   editor were deriving the keyboard subscribe-cap from O4
   (boot self-svc), inherited verbatim by the child task. Same
   source → same derived ref → oriscterm dedup'd them as one
   subscriber. Fix: `term_init` now ObjAllocs its own private
   service object and parks it in **O9** (term mailbox); the
   subscribe-cap derives from there, so each instance gets a
   distinct ref. `term_getkey` polls O9 instead of O4.

2. **`hostfsd` keyed sessions by sender pid.** Both shell and
   editor live on CPU 0, so hostfsd routed both their requests
   to whichever subscribed first — the shell. The editor's
   `hf_open` reply went to the shell's mailbox; the editor
   blocked forever. Fix: each `hf_*` SEND now carries the
   caller's mailbox in O3 as a per-call reply_cap. hostfsd
   matches sessions by underlying object (same home + index
   as the subscribed sub-ref), falling back to the per-pid
   lookup when O3 is null.

These changes touched `term_init` / `term_getkey` /
`term_print_n_sync` / all four `hf_*` operations / `hf_init` —
several of which used to clobber O9 to park scratch sub-caps.
They now derive directly into O2 or O3 (the SEND payload slots)
without parking in long-lived OPRs.

### orx state moves out of O7

Adjacent fix forced by Phase 38: the grid service ref lives at
O7, but `orx.c` had been claiming O7 for its own per-spawn
manifest (lazily allocated on first `orx_run`). When the shell's
`cmd_run` ran first and then a child tried to use the grid, the
grid SEND went to orx's data object (no `S` cap) and trapped.

orx's persistent state now lives at the back end of `task.c`'s
objstore (O12, oversized by `ORX_STATE_BYTES = 408` to fit). All
orx OREFLD/OREFST offsets shift by +128 (`TABLE_BYTES`) to land
past the libc task table. `orx_state_init` becomes a no-op —
`task_init` already allocated everything orx needs. O7 is freed
for the grid ref to keep across cmd_run invocations.

### `fake_terminal`

- Replaces `kbd_sub` (single ref) with `kbd_subs` (list) +
  `kbd_focus` (index).
- New `--event focus` toggles focus locally (mirrors F1 — no SEND
  goes out, just changes routing).
- New `--event wait-kbd:N` blocks until at least N kbd
  subscribers have registered. Tests use this between launching
  a backgrounded program and sending it keystrokes; without it
  you race the program's `term_init`.

### Tests

- **`tools/devices/tests/test_shell_edit.sh`** — completely
  rewritten. Pre-creates `/scratch.txt`, spawns
  `run /programs/edit.orx &`, waits for the editor's kbd
  subscribe (`wait-kbd:2`), `focus`-cycles the keyboard to the
  editor, navigates to end-of-line + types `!`, hits `^S` and
  `^X`, focus-cycles back, then `exit`s the shell. Asserts the
  on-disk `scratch.txt` reflects the insert and that the focus
  log line shows the cycle landed.
- All 18 device/shell tests pass; **138 sim validation tests**
  unaffected.

### What's not yet done

- **Argument passing to guests.** The editor opens a hardcoded
  `/scratch.txt`. A real `edit foo.txt` flow needs `cmd_run` to
  hand its remaining args to the spawned program — likely via a
  shared service object the libc unpacks into argv[]. Phase 41.
- **Focus indicator on the canvas itself.** The title bar
  reflects focus, but that's outside the canvas — easy to miss.
  A small marker in the corner of the grid would help.
- **Stale subscriber cleanup.** When the editor exits, oriscterm
  still has its sub_ref in `kbd_subscribers`. Cycling focus to
  it would silently drop keys (the underlying descriptor is
  gone). Needs a "remove sub on send-fail" cleanup pass.
- **hostfsd protocol cleanup.** The per-call reply_cap is a
  workaround over the legacy per-pid session lookup. A future
  pass should remove the legacy fallback and require all calls
  to carry reply_cap.

## Phase 41a — Argv plumbing (top half) (Ouroboros, day 18)

The shell can now parse `run /programs/edit.orx /etc/hosts` —
the path/args split happens in `cmd_run`, the args string
threads through `orx_run` / `orx_spawn`, and a new libc helper
`program_args()` is the documented API the spawned program
uses to read them.

What's wired:

- **Spec**: `TaskCreate` (#0x000) gains an optional `O4` slot.
  When non-null at TaskCreate time, the firmware maps the
  referenced object R-only at `ARGV_VA` (0x000A0000) in the
  child's address space. Documented in
  `SYSTEM_FIRMWARE_INTERFACE.md`.
- **simorisc**: validates `O4`, adds an `(ARGV_VA, ARGV_VA+len)`
  mapping with `CAP_R` to the child's mapping list.
- **`liborisc/argv.c`**: `program_args()` returning a `const
  char *`. Returns a pointer the program can pass to
  `strlen` / dereference safely.
- **`orx_run` / `orx_spawn`**: signatures gain an `args` param.
  `cmd_run` parses the rest of the line after the path and
  passes it through.
- **`edit.c`** uses `program_args()` to pick its target file,
  falling back to `/scratch.txt` when no args are given.

### What's not wired (deferred to Phase 41b)

The actual ARGV_VA mapping — the libc piece that allocates an
args buffer, copies the args into it, and parks the ref in `O4`
just before TaskCreate — is stubbed in this phase. `program_args()`
returns a static empty string for now, so `edit /foo.txt` still
opens `/scratch.txt`.

The infrastructure work hit a tricky cross-test interaction
where the original `orx_setup_args` (ObjAlloc + MapObject +
memcpy + Unmap) plus the new two-buffer `cmd_run` triggered a
regression in `test_shell_bg` — turned out the second
`PATH_MAX` buffer on `cmd_run`'s stack pushed it past the
shell's stack budget under certain banner lengths. A
single-buffer rewrite of `cmd_run` (NUL-terminate the path/args
in place, slice instead of copy twice) fixed that, but by then
the budget for this PR was spent. The full `orx_setup_args`
implementation gets its own follow-up.

### Tests

- All 18 device/shell tests still pass; **138 sim validation
  tests** unaffected.
- `test_shell_edit.sh` continues to verify the focus-switch
  flow with the editor opening `/scratch.txt` (the default
  when `program_args()` is empty).

### What's not yet done

- **Phase 41b**: actual ARGV_VA mapping. The
  `simorisc`/spec/libc API surface is in place; the missing
  piece is `liborisc/orx.c::orx_setup_args` allocating + copying
  + parking the ref. Needs care around pcc's register
  allocation across helper-function boundaries (OPRs are
  caller-saved scratch in the orisc backend, so the args ref
  has to ride through an objstore slot, not a register).
- **Manifest tracking** of the args object so `orx_unload` can
  free it. With ~256 bytes per spawn it's a tiny leak, but
  worth cleaning up.

## Phase 41b — Argv buffer wiring + a TaskExit scheduler fix (Ouroboros, day 19)

The bottom half of argv. `program_args()` now returns a real
pointer into a real ARGV_VA mapping; `edit /foo.txt` opens
`/foo.txt`. Got there via two unexpected detours.

What's wired:

- **`liborisc/orx.c`**: a single shared 256-byte args object
  (`TAG_DATA`, R+W+V+C). ObjAlloc'd once on first spawn (or
  eagerly via the new `orx_init()`), persistently mapped R+W
  in the parent at `ARGS_PARENT_VA = 0x00500000`, and parked
  in `ORX_SLOT_ARGV` (orx-state offset 408 → `o12+536`) for
  the per-spawn `orefld o4` into `O4` immediately before
  `TaskCreate`. The per-spawn cost shrinks to a memcpy — no
  `MapObject`/`Unmap` pair on every `orx_run`.
- **`orx_init()`** (new public libc fn): pre-allocates the
  buffer + sets up the parent mapping. Optional, but the
  shell calls it at boot so the alloc/map cost lands BEFORE
  the preempt timer is armed and the per-spawn path stays
  predictable. The lazy fallback in `orx_setup_args` keeps
  single-shot programs working without the boot dance.
- **`liborisc/argv.c::program_args`** synthesizes
  `(char *)0xa0000` via `lui r,0xa; ori r,r,0` because pcc's
  natural `(char *)CONSTANT` lowering emits an `la r,N`
  pseudo that asmorisc rejects. Same workaround pattern as
  the existing `liborisc/host_io.c`.
- **`task.c`**: `ORX_STATE_BYTES` bumped from 408 to 416 to
  reserve the 8-byte `ORX_SLOT_ARGV` past the manifest area.
  Total `o12` storage is now 544 bytes
  (`TABLE_BYTES=128 + 416`).

### The detour: TaskExit on a lone-blocked CPU

Phase 41a's argv plumbing (the empty-string stub) shipped
because every attempt to wire the actual mapping broke
`test_shell_bg` non-deterministically. Bisecting the failure
to a single `orx_argv_is_null()` call (an `orefld` + `oisn`
inside a function-call frame, ~30 cycles) finally unmasked
the real bug — and it wasn't in the libc.

`primitive_TaskExit` was tearing down the CPU as soon as
`pick_next_runnable()` returned `None`, even when other
tasks were `BLOCKED` and might yet wake. Concretely: shell
spawns a bg task, calls `task_yield`, child runs. If the
child runs to completion in one quantum (baseline timing),
the shell is still `RUNNABLE`, `TaskExit` picks it, and
everything works. If a preempt fires mid-child (which is
what those extra ~30 cycles enabled), the shell finishes its
`cmd_run`, returns to the prompt, and blocks in
`RecvQueuePoll` BEFORE the child completes. When the child
finally `TaskExit`s, no runnable task → `TaskExitSignal` →
`cpu.active = False` → CPU brick. The shell's keystroke
arrives, `_wake_blocked_tasks` happily promotes it to
`RUNNABLE`, but no path in `run()` schedules it — the CPU
is dead.

Fix in `simorisc`:

- New `BlockedOnExitWait` sentinel. When `TaskExit` finds
  no runnable but `any(t.state == TASK_STATE_BLOCKED for t
  in cpu.tasks.values())`, park the CPU on this sentinel
  instead of raising. The blocked-but-runnable branch in
  `run()` then context-switches into whichever task wakes
  first.
- The same blocked-but-runnable branch was also flipping the
  `cur` task to `TASK_STATE_BLOCKED` blindly during the
  switch, which would corrupt `cpu.current_task` if it was
  already `EXITED`. Now skipped for exited tasks — they stay
  exited, no `save_cpu_to_task` call.

### The other detour: the auto-reaper race

Even with the simulator fix, `test_shell_bg` still failed —
just differently. The test asserts `[task 0 done 0]` (the
auto-reaper's wording) appears. With the new timing, the
shell's main-loop top-of-iteration `reap_exited_tasks()` now
runs while the child is still mid-term-print, so it has
nothing to reap; by the time the user-typed `wait 0` arrives
and the shell wakes from `read_line`, the next loop top's
reap finds nothing because `cmd_wait` already harvested it
(printing `[task 0 exited 0]` instead).

Fix in `examples/cc/shell.c`: a second `reap_exited_tasks()`
call right after `read_line` returns, before dispatching the
typed command. Catches tasks that exited while we were
blocked in `getkey`. Mirrors the bash convention of
announcing background-job completion at the prompt
boundary, not just at the top of the read loop.

### Tests

- All 18 device/shell tests pass, stable across 3 back-to-
  back full-suite runs.
- New manual smoke check: a guest that copies
  `program_args()` into a stack buffer and `term_print`s it
  receives the exact command-line slice (`got: [hello world]`
  for `run echo_args.orx hello world`). Note that calling
  `term_print` directly on the `program_args()` pointer
  doesn't work — `term_print` assumes its source is in the
  data segment or stack, and computes a wrong offset for the
  ARGV_VA region. Programs that want to display args need to
  copy first.

### What's not yet done

- **Manifest tracking** for the args object (still). The
  shared-buffer design moots the per-spawn leak — there's
  exactly one args object per CPU, freed when the CPU
  itself goes away — but if a future spawn ever needs a
  larger buffer than 256 bytes, the helpers grow trivially.
- The conditional fast-path for empty args (skip `memcpy`,
  zero just byte 0) was tempting but couldn't be made
  unconditionally correct without the orx_init dance — and
  with the simulator fix in place the timing race no longer
  forces the optimization. Shipping the simpler always-call
  path.

## Phase 41c — Dhrystone runs from the shell (Ouroboros, day 19)

A small follow-up on the back of 41b: `run dhry.orx` from the
shell now actually finishes instead of dying mid-benchmark
with `simorisc: max cycles exceeded (10000000)`. The fix is a
one-liner in spirit:

- `simorisc --max-cycles` defaults to **0 (unlimited)** when
  `--connect` is set, and 10M only in standalone mode. The
  10M cap exists as a CI safety net for runaway `.s` tests
  via `tools/sim/tests/validation/runner.py` (which sets its
  own bound per-test, default 100k); it never made sense for
  an interactive socket-driven session whose lifetime is
  owned by the crossbar, not by an instruction count. The
  `system.run` loop now treats `max_cycles == 0` as "loop
  until the natural exit conditions fire".

Result on this dev box (Apple M-series Python 3.13):

    Dhrystone Benchmark, Version 2.1 (Object RISC port)
    Iterations: 5000
    ...
    Cycles elapsed:       20371748
    Cycles per iteration: 4074
    16 MHz: ~3927 dhry/s   = ~2.2 DMIPS
    20 MHz: ~4909 dhry/s   = ~2.7 DMIPS

So a Dhrystone iteration costs ~4k Object RISC cycles, which
puts the imagined silicon roughly between an early-1980s
68000 (~1.4 DMIPS at 12.5 MHz) and a late-1980s 68020
(~3 DMIPS at 16 MHz). About what we wanted from a
1986-vintage chip.

### Tests

- All 18 device/shell tests still pass.
- All 138 sim validation tests still pass (they each set
  their own `@max-cycles`, unaffected by the default
  change).

## Phase 41d — cwd passthrough + targeted kbd unsubscribe (Ouroboros, day 19)

Two related "program lifecycle hygiene" fixes from real
in-shell testing:

**Bug 1 — `edit hello.c` from a non-root cwd showed an empty
buffer.** The shell resolved the executable path against cwd
but passed the user's `args` string verbatim to the spawned
program. `edit` called `hf_open("hello.c")` and hostfsd
resolved that against its jail root (it has no per-task cwd
concept), so the file came up missing.

Fix: pass cwd to spawned programs. The shared argv buffer
now holds two NUL-terminated strings back-to-back:

    [0..]                  args   (NUL-terminated)
    [strlen(args)+1 ..]    cwd    (NUL-terminated)

`program_args()` keeps its existing contract (returns a
pointer to the first segment — old programs see no change).
A new `program_cwd()` walks past the first NUL to find the
launcher's working directory. `orx_run` / `orx_spawn` gained
a `cwd` parameter; the shell threads its current cwd through
in `cmd_run`. `edit` now prefixes relative paths with cwd
before opening — so `cd /sub; run /programs/edit.orx
hello.c` opens `/sub/hello.c` as expected.

**Bug 2 — quitting a program left its keyboard subscription
in oriscterm's focus list.** There's no process-death
notification in the wire protocol; oriscterm only learns
about subscriber lifecycle through explicit
subscribe/unsubscribe SENDs. A program that just `TaskExit`s
leaves a dead entry — F1 cycles still land on it, keys
silently drop into a stale queue, and the user has to figure
out something is wrong.

Fix: a new libc `term_shutdown()` that derives the same
`R|S` sub-cap term_init originally registered (ObjDerive is
deterministic over (gen, home, idx, caps), so the bytes are
identical) and SENDs it back to the keyboard service with
`R4 = 1` — the new "targeted unsubscribe" command. Edit
calls `term_shutdown()` right before returning from main.

oriscterm's keyboard handler grew the R4 dispatch:

    R4 = 0  →  subscribe (existing default)
    R4 = 1  →  unsubscribe by sub-ref (find in list, remove
              just that one entry; quietly ignored if no
              match — supports defensive double-call)

The legacy `O2=null → unsubscribe-all` path stays as-is for
back-compat, but new callers should use the targeted form so
they don't kick the shell out of the focus list while
cleaning up after themselves. fake_terminal mirrors both
paths so the tests see realistic behaviour.

Programs that crash before reaching `term_shutdown` will
still leave a dead subscriber. A wire-level NACK from the
home CPU on stale-target SENDs would let oriscterm prune
dynamically; deferring that to a later phase.

### Tests

- `test_shell_edit.sh` updated to exercise both fixes:
  `cd /sub` first, then `run /programs/edit.orx scratch.txt
  &` (relative path), and asserts (a) the inserted `!`
  lands in `/sub/scratch.txt` (not `/scratch.txt`), (b) the
  unsubscribe log line appears with subscriber count back to
  1, and (c) the shell receives the post-edit `exit` keys
  WITHOUT a manual second F1 (proving focus auto-returned).
- All 18 device/shell + 138 sim validation tests still pass.

## Phase 42 — Repo reorg (Ouroboros gets its own roof)

Pure plumbing pass — no behaviour changes, every test still
passes. The repo had been growing Ouroboros (the OS) inside
`examples/cc/`, and the architecture documentation was a flat
pile of `.md` files at the repo root. Two moves and a top-level
Makefile:

- `examples/cc/shell.c` → `ouroboros/shell.c`
- `examples/cc/programs/` → `ouroboros/programs/`
- `examples/cc/run_shell.sh` → deleted; replaced by `make boot`
- `examples/cc/programs/build-one.sh` → deleted; subsumed by
  Makefile pattern rules
- All architecture `.md` files (CONTRACT, INSTRUCTION_SET, the
  seven volumes, HISTORY) → `docs/`
- PDFs + `build_pdf.py` + `preamble.tex` → `docs/`
- `scripts/boot.sh` is the new launcher (was `examples/cc/run_shell.sh`);
  computes today-minus-40, rebuilds the shell with the
  alternate-history banner, symlinks `build/programs/` into the
  jail at `/programs/`, and execs `tools/oriscrun`.

The new `Makefile` at the top level is the canonical build entry
point:

    make            — build liborisc, the shell, and every program
                     (incremental — pcc + asmorisc + orld pipeline
                     wrapped in pattern rules)
    make boot       — build, then start Ouroboros
    make lib        — just liborisc.ora
    make shell      — just the shell
    make programs   — just the programs
    make clean      — wipe build/

Build artefacts now land under `build/` (gitignored): `build/lib/`
for individual liborisc objects, `build/runtime/` for crt0 +
console_io, `build/programs/` for `.orx` guests, and
`build/{liborisc.ora,shell.orx}` at the top.

Test scripts no longer inline the cpp/ccom/asm/ld pipeline for
the shared parts — they call `make -s lib` to ensure liborisc is
built, then build their per-test shell + guests against
`build/liborisc.ora`. Custom shells (with test-specific banners)
and one-off guest programs still get built inline so each test
stays self-contained.

`tools/cc/lib/build.sh` is now a thin `exec make lib` wrapper —
old muscle memory keeps working, but the artefact lives at
`build/liborisc.ora` (was `tools/cc/lib/liborisc.ora`).

Discoverable entry point — first thing in the README is now
"`make boot` to start Ouroboros." The `examples/` tree is back
to being just demos: standalone C and assembly programs,
`linkboot/`, the Dhrystone smoke test that runs without the
shell, the multitask demos.

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
- An MVP shell that's actually pleasant to use: `cd` / `pwd` /
  `echo` / `cycles` / `time` alongside `cat` / `more` / `ls` /
  `run` / `help` / `exit`, paths normalized against a shell-side
  cwd, the prompt mirroring the cwd, command history with up/down
  arrow recall (16-entry circular buffer), visual undo on
  backspace, and a `more <path>` paginator with a `--More--` /
  q-to-quit prompt. The Tk terminal's text pane is 24×80 so help
  and casual cat output fit without scrolling off; `cmd_run`
  surfaces the guest's actual exit code in the `[exited N]`
  line.
- A `Dhrystone v2.1` port that runs end-to-end through pcc →
  asmorisc → orld → simorisc, reporting cycle counts the
  benchmark would have produced on actual silicon at the
  OR-1000's claimed 16/20 MHz nominal rates. Current numbers:
  ~4067 cycles/iter, 2.2 / 2.7 DMIPS, plausible for a single-
  issue, naively-allocated 1986 RISC.
- Wall-clock primitives: `TimeNow` (#0x400) and
  `ClockResolution` (#0x410) implemented in firmware with μs
  resolution; the spec's side-channel high bits are the only
  remaining gap.

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
