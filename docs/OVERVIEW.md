# The Object RISC Architecture

## A High-Level Overview

*Architecture Reference, Volume I — Revision 0.1, 1986*

---

## 1. Introduction

Object RISC is a 32-bit load-store reduced instruction set architecture
designed for tightly-coupled multiprocessor systems. It draws on the work
emerging from Stanford and Berkeley in the early 1980s — small instruction
sets, fixed instruction widths, single-cycle execution, and pipelined
implementations — and combines those principles with first-class hardware
support for objects and a crossbar-based processor interconnect.

The architecture has three design goals, which it treats as equally
important:

1. A single Object RISC processor should be cheap, simple, and fast to
   build with the process technologies available in the late 1980s.
2. A collection of Object RISC processors connected by a crossbar should
   present a coherent, location-transparent object space to software,
   without requiring a separate communications co-processor or a software
   message-passing layer in the hot path.
3. The combination of hardware and the runtime services common to any
   operating system should present a single, stable upward interface, so
   that software written for one Object RISC configuration runs unmodified
   on others. The layer that provides this interface is part of the
   architecture, and is described in Section 6 as *System Firmware*.

We believe these goals are compatible. The first disciplines the second:
every feature added to support multiprocessing must justify itself against
the cost it imposes on the single-processor pipeline. The third
disciplines both: features that cannot be made cheap and that vary
between implementations are pushed out of silicon and into firmware,
where they can be specified once and reimplemented appropriately for
each generation of hardware.

The result is a design that looks, on a per-processor basis, very much
like its RISC contemporaries — but whose register file, instruction set,
trap model, and surrounding firmware layer are quietly shaped by the
assumption that the processor is one of many.

## 2. Influences and Lessons Learned

Object RISC is not the first architecture to attempt hardware support for
objects. Intel's iAPX 432, introduced in 1981, was the most ambitious
prior attempt, and its commercial failure casts a long shadow over this
work. The 432 demonstrated that an object model implemented in microcode,
with variable-length instructions and an interpretive execution model,
could not compete on price or performance with conventional designs of
the same era — let alone with the new RISC processors then emerging.

We draw three lessons from the 432:

- **The object model belongs in the register file, not in microcode.**
  Object references should be values that the programmer-visible machine
  can hold, compare, and pass around. The cost of dereferencing an object
  reference must be a small, bounded number of pipeline cycles, not a
  microcoded sequence.
- **Bounds and capability checks must be free in the common case.**
  If accessing a field of an object costs more than accessing a word of
  memory through a base register, programmers will route around the
  object system, and the hardware investment is wasted.
- **Generality is the enemy of throughput.** The 432 supported an
  elaborate type system in hardware. Object RISC supports the minimum
  primitive — a bounded, capability-tagged region of memory — and leaves
  type structure to the compiler and runtime.

The crossbar-based multiprocessor model is influenced by the Carnegie
Mellon C.mmp work of the previous decade, and more recently by the INMOS
Transputer, whose serial links between processors demonstrated that
inter-processor communication could be a primitive of the architecture
rather than a peripheral. Object RISC differs from the Transputer in two
respects: communication is by addressed message rather than by named
channel, and the interconnect is a crossbar rather than a fixed graph of
point-to-point links.

The two-level naming arrangement — a global object namespace above,
per-task virtual address spaces below — is influenced by Apollo
Computer's Domain system, in which long-form object identifiers name
storage uniformly across a network of workstations and processes map
those objects into their own thirty-two-bit virtual address spaces on
demand. Object RISC compresses Apollo's network into a crossbar and the
host operating system's mapping machinery into firmware, but the
underlying proposition is the same: the namespace in which storage is
named must be larger than the address space in which any single task
computes, and the bridge between the two is built at runtime rather
than designed into the address layout. A 32-bit architecture intended
to scale beyond a single processor cannot, in our view, do otherwise.

## 3. The Processor

A single Object RISC processor is conventional in most respects.

- **Word size.** 32 bits. Addresses, integers, and instructions are all
  32 bits wide. Byte and halfword loads and stores are supported but
  internally widened.
- **Endianness.** Big-endian. Byte 0 of a word is the most significant.
- **Register file.** Thirty-two general-purpose registers, `R0` through
  `R31`. `R0` reads as zero and discards writes, in the manner of MIPS.
  In addition, sixteen *object registers*, `O0` through `O15`, hold
  object references. Object registers are a distinct register class with
  their own load, store, and move instructions; they cannot be used as
  general-purpose integer registers, and integer registers cannot be
  dereferenced as objects. This separation is the central architectural
  decision of Object RISC and is discussed further in Section 4.
- **Instruction format.** All instructions are 32 bits, aligned on
  4-byte boundaries. Three primary formats are defined: register-register
  (R-type), register-immediate (I-type), and jump (J-type), in the style
  of the Stanford MIPS design.
- **Pipeline.** A five-stage pipeline is the reference implementation:
  instruction fetch, decode and register read, execute, memory access,
  and write-back. Branches are delayed by one instruction slot. Loads
  have a one-cycle delay slot in the reference implementation; the
  compiler is responsible for scheduling around it.
- **Condition codes.** None. Comparisons produce a value in a general
  register, and conditional branches test a register against zero or
  against another register. This avoids the pipeline serialization that
  condition-code architectures suffer at branches and simplifies the
  trap model.
- **Address translation.** Each task executes in a private 32-bit
  virtual address space. The processor includes a translation lookaside
  buffer and a hardware page-table walker; page tables are per-task and
  are installed by System Firmware on context switch, with page faults
  trapping to firmware. The translation hardware serves ordinary loads
  and stores against the current task's address space. Loads and stores
  through an object register take a separate translation path described
  in Section 4, and the two paths share no hardware state.
- **Privilege.** Three modes: *user*, *supervisor*, and *firmware*. The
  object register file is available in all three, but capability rights
  checked at object dereference distinguish trusted from untrusted code.
  Firmware mode — the privilege at which System Firmware (Section 6)
  executes — is the only mode permitted to mint new object references,
  to install entries in the crossbar routing table, or to address the
  device space directly. User and supervisor code reach those facilities
  by trapping into firmware via the `CALL` instruction.

A reference implementation in 1.5-micron CMOS is expected to fit in
approximately 110,000 transistors and to run at 16 to 20 MHz, comparable
to the first-generation MIPS R2000 and SPARC parts.

## 4. Objects

An *object*, in Object RISC, is a contiguous region of memory together
with a hardware-enforced descriptor. The descriptor records the object's
base address, its length in bytes, its type tag (an opaque 16-bit value
interpreted only by software), the home processor on which it physically
resides, and a set of capability bits — read, write, execute, send, and
revoke.

An *object reference* is a 64-bit value held in an object register. It
combines an index into the system-wide object table with a generation
counter that is incremented when the underlying object is freed. The
generation counter prevents a stale reference from accidentally naming a
later object that happens to reuse the same table slot. Object references
are produced only by System Firmware, in response to primitive calls
issued from user or supervisor code; they are otherwise opaque to the
caller, which can copy them between object registers and pass them as
arguments but cannot manufacture them.

Loads and stores through an object register take an offset and a width.
The hardware checks, in parallel with address computation, that the
offset and width fall within the object's length and that the requesting
processor holds the appropriate capability bit. A failed check raises a
synchronous trap before the memory access is committed. In the common
case — a valid access to a local object — these checks add no cycles to
the load or store; the descriptor is held in a small associative cache
local to the processor, distinct from the per-task TLB and shared across
all tasks resident on the processor.

For sustained access patterns a task may instead *map* an object, or a
window into one, into its own virtual address space through the
appropriate firmware primitive. Firmware installs page-table entries
that resolve the chosen virtual range to the object's physical storage
on the home processor, honouring the capability bits recorded in the
descriptor. Subsequent ordinary loads and stores within that range run
at full MMU speed, with no further reference to the object register
file. The choice between the two access paths is one of working-set
characteristics, not of capability: the protection guarantees are
identical in either case. Mapping is restricted to objects whose home
is the local processor; remote objects remain accessible through the
object register file, which transparently routes the access across the
crossbar.

This two-path arrangement is the practical consequence of being a
32-bit architecture in a multi-processor system. A flat space of four
gigabytes cannot accommodate, even in the medium term, the aggregate
storage reachable across an Object RISC crossbar. Object references
therefore live in a global namespace far larger than any single task can
address, and per-task address spaces are populated from that namespace
by mapping at runtime. Within a task, ordinary pointer arithmetic
remains 32-bit and remains fast; between tasks, and between processors,
storage is named by reference rather than by address.

This is what we mean by "Object RISC": objects are first-class hardware
entities, but they are kept architecturally lean. There is no inheritance,
no method dispatch, no garbage collector, and no type lattice in the
hardware. Those mechanisms belong to the compiler and the runtime, where
they can evolve without an instruction-set revision.

## 5. The Crossbar Interconnect

An Object RISC system comprises one or more processors, one or more
memory modules, and a *crossbar* connecting them. The reference
configuration is a 16-port crossbar with single-cycle arbitration and a
peak aggregate bandwidth proportional to the port count and the cycle
time. Smaller and larger configurations are permitted; the architecture
defines the protocol on the wire, not the physical topology.

The crossbar is visible to software in two ways.

First, every object reference names a *home processor*. When a processor
issues a load or store through an object reference whose home is itself,
the access is local and proceeds through the cache and memory hierarchy
in the usual way. When the home is a different processor, the access is
packaged as a message, routed across the crossbar to the home, executed
there against the local memory, and the response routed back. The
processor stalls the issuing instruction in the same way it would stall
on a cache miss. From the programmer's perspective the load or store
behaves identically; it is merely slower.

Second, a `SEND` instruction transmits an explicit message — an object
reference together with a small payload of general-register values — to
the home processor of that reference. The receiving processor delivers
the message to a handler that System Firmware has installed on behalf of
the addressed object. This primitive is the intended substrate for
higher-level constructs: remote procedure calls, actor-style message
passing, and operating-system requests across the crossbar all reduce
to `SEND`.

The crossbar arbiter, the routing table, and the message buffers are
specified by the architecture but are not part of any individual
processor. A small system may implement them on a single companion chip;
a larger system may distribute them across several. The protocol is the
contract.

## 6. System Firmware

The hardware described above is necessary but not sufficient to
constitute an Object RISC system. The architecture also defines a
*virtual machine interface*: a fixed set of system primitives, invoked
from ordinary code via a single trap-style `CALL` instruction, that
together specify the services any conforming implementation must
provide. The layer that implements them — and that, in any
multiprocessor or multi-tenant configuration, also acts as the
hypervisor under which guest operating systems are hosted — is called
*System Firmware*.

The primitive set covers the work that is common to every system above
the bare metal but that varies in detail between implementations:

- **Task management.** Creation, scheduling, suspension, and termination
  of tasks; binding of tasks to processors; primitives for synchronization
  between tasks both on the same processor and across the crossbar.
- **Memory management.** Allocation and deallocation of objects from
  the physical memory pool; assignment of objects to home processors;
  migration and revocation of references; mapping and unmapping of
  objects into the virtual address space of a task, and modification of
  the access rights with which they are mapped; service of page faults
  raised by the address translation hardware.
- **Communications.** Installation of handlers for incoming `SEND`
  messages; delivery, queuing, and flow control of messages whose
  recipient is not currently scheduled; routing-table maintenance.
- **Device and I/O interaction.** Discovery and naming of devices as
  objects; mediation of interrupts; mapping of device registers behind
  capability-checked object references so that drivers, like all other
  code, address hardware only through the object register file.
- **Time, clocks, and console services**, and the small remainder of
  facilities that any operating system would otherwise have to
  reinvent.

System Firmware runs on the same processors as ordinary code, in the
firmware privilege mode introduced in Section 3. It is the only software
with direct access to the object table, the crossbar routing table, the
device address space, and the physical memory map. Supervisor-mode code
(an operating system) and user-mode code (an application) reach those
resources only through primitive calls. The primitive interface is part
of the architecture; the firmware that implements it is supplied by the
hardware vendor or system integrator.

This stratification has three consequences worth stating up front.

**The hardware ISA can be smaller.** Many features that would
conventionally belong to the processor — virtual address translation,
paging, scheduling support, interrupt prioritization, communication
buffer management — are firmware concerns. The processor is not
required to implement them in silicon, only to provide the trap
mechanism by which the firmware is reached.

**The system has a stable upward interface.** Code written to the
primitive set is portable across Object RISC implementations and across
configurations of different scale, from a single processor with a small
crossbar to a sixteen-processor system. The firmware absorbs the
differences, and a recompilation against a new hardware generation
should not, in general, be necessary.

**Multiple operating systems can coexist.** Because the firmware is the
hypervisor, an Object RISC system can host more than one operating
system simultaneously, each in its own protection domain, sharing the
processors and crossbar through firmware-mediated scheduling. We expect
this capability to be of particular interest in research and educational
settings, and in the gradual migration of existing software bases.

The full specification of the primitive interface — the call numbers,
parameter conventions, return values, and trap conditions — is the
subject of Volume VI of this reference. The present volume notes only
that, in the architecture as a whole, "Object RISC system" refers to
the combination of conforming hardware and conforming firmware: neither
is meaningful alone.

## 7. What Object RISC Is Not

To narrow the scope of subsequent volumes, we list explicitly what the
architecture does not provide.

- **Cache coherence across the crossbar.** Processors cache only local
  memory. Remote accesses are uncached at the issuing processor and rely
  on the home processor's cache. A future revision may relax this; the
  current revision does not.
- **Hardware floating point.** A floating-point co-processor interface
  is reserved, but the reference processor does not implement floating
  point. Software emulation traps are provided.
- **Demand paging to backing store.** The architecture defines a page
  fault trap and routes it to firmware, but specifies no convention by
  which virtual pages are backed with disk or other slow storage.
  Whether to support such a mechanism — and the policy by which dirty
  pages are evicted, prefetched, or pinned — is a firmware concern that
  may legitimately differ between implementations.
- **Garbage collection.** The architecture provides no hardware support
  for tracing or reclaiming unreferenced objects. References are
  released explicitly through firmware primitives, and stale references
  are caught at use through the generation counter described in
  Section 4.
- **Dynamic dispatch.** The hardware does not implement method lookup,
  inheritance, or any other form of indirect call beyond the `JALR`
  instruction. The object register file makes such mechanisms cheap to
  build in software; it does not implement them.

## 8. Roadmap of Subsequent Volumes

This volume gives the high-level overview. Detailed specifications are
deferred to:

- **Volume II — Instruction Set.** The full instruction encoding,
  semantics, and trap conditions for the integer and object subsets,
  including the `CALL` instruction by which firmware is reached.
- **Volume III — Object System.** The descriptor format, the object
  table, capability semantics, and the firmware primitives for minting,
  revoking, and migrating references.
- **Volume IV — Interconnect Protocol.** The wire-level message format,
  arbitration, routing-table semantics, and timing requirements for the
  crossbar.
- **Volume V — Reference Implementation.** A pipeline-level description
  of a representative single-issue processor and a 16-port crossbar,
  including expected gate counts and cycle times.
- **Volume VI — System Firmware Interface.** The complete specification
  of the system primitive set: call numbers, parameter conventions,
  return semantics, trap conditions, and the requirements that a
  conforming firmware implementation must meet on top of any conforming
  hardware.

— *The Object RISC Architecture Group, 1986*
