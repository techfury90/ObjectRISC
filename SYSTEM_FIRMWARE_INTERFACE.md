# The Object RISC Architecture

## Volume VI — System Firmware Interface

*Architecture Reference, Revision 0.1, 1986*

---

## 1. Scope

This volume specifies the system primitive interface — the set of
operations reached from user-mode and supervisor-mode code through the
`CALL` instruction. It defines the calling convention, the
allocation of primitive numbers among functional groups, the
arguments, return values, and error conditions of each primitive in
this revision, and the conformance requirements that any firmware
implementation claiming to be Object RISC firmware must meet.

The architecture-level rationale for the existence of System Firmware
as a layer is given in Volume I, Section 6. The trap mechanism by
which `CALL` is dispatched, and the layout of the firmware vector
table, are specified in Volume II. This volume specifies the
behaviour above the trap.

The reader is assumed to be familiar with all preceding volumes.

## 2. The Primitive Call Convention

### 2.1 Argument and Return Registers

Arguments to a primitive are passed in `R4`–`R7` and `O1`–`O4`, in the
order in which they appear in the primitive's signature: integer
arguments fill `R4`, `R5`, `R6`, `R7` in order, and reference arguments
fill `O1`, `O2`, `O3`, `O4` in order. A primitive of fewer than four
integer or four reference arguments leaves the unused registers
undefined; the caller is responsible for populating only the registers
the primitive consumes.

Return values are returned in `R2` and `R3` (integer) and `O1`
(reference). A primitive that returns no value leaves these registers
unmodified from the firmware's perspective; the caller may not assume
they retain their pre-call value.

The primitive number is encoded in the 26-bit immediate of the `CALL`
instruction itself, as Volume II specifies. It is not passed in a
register.

### 2.2 Caller- and Callee-Saved State

Firmware preserves all registers visible to the caller across a `CALL`
except for `R2`, `R3`, `O1`, and any register the caller has explicitly
populated as an argument. The standard caller-saved temporaries
(`R8`–`R15`, `R24`–`R28`, `O5`–`O8`, `O13`–`O15`) are saved by firmware
on entry and restored on return. This convention is more conservative
than the calling convention of Volume II requires, and is justified by
the desire that `CALL` behave indistinguishably from a procedure call
for purposes of compiler register allocation.

### 2.3 Error Reporting

Every primitive returns a status code in `R2`. Zero indicates success;
a nonzero value identifies an error condition per Section 2.4. A
primitive that returns useful values in `R3` or `O1` does so only on
success; on error those registers carry undefined contents.

A primitive that requires a capability bit on a reference argument
returns `EPERM` if the capability is absent, regardless of whether
other invariants on the argument hold. A primitive that requires a
live reference and is given a stale or null one returns `ESTALE` or
`EFAULT` respectively, before any capability check is performed.

Each primitive carries a *minimum caller mode* (Volume II Section 13).
A `CALL` issued from a mode below the primitive's minimum returns
`EPERM` in `R2` without entering the primitive's body and without
modifying any other architecturally visible state. The current
allocations are: `MapObject`, `InstallProgram`, and `InstallHandler`
require supervisor mode; all other primitives in this revision are
callable from user mode. Future page-table primitives will require
firmware mode.

### 2.4 Standard Error Codes

| Code | Symbol      | Meaning                                          |
|------|-------------|--------------------------------------------------|
| 0    | `OK`        | Success                                          |
| 1    | `EINVAL`    | Invalid argument value                           |
| 2    | `ENOMEM`    | Insufficient memory or table space               |
| 3    | `EPERM`     | Required capability bit absent on a reference    |
| 4    | `ENOSYS`    | Primitive not implemented in this firmware       |
| 5    | `EBUSY`     | Resource is in use and cannot be acquired now    |
| 6    | `ENOENT`    | No such object, device, or named resource        |
| 7    | `ETIMEDOUT` | Bounded wait expired before completion           |
| 8    | `EFAULT`    | Reference is null where prohibited               |
| 9    | `EAGAIN`    | Transient failure; the caller should retry       |
| 10   | `ESTALE`    | Reference's generation does not match the descriptor |
| 11   | `EREMOTE`   | Operation requires a local object; argument is remote |

Error codes 12 through 255 are reserved for future revisions.
Implementations may not use them for implementation-specific errors;
such errors are reported through `EINVAL` with a supplementary
diagnostic written to the firmware log.

### 2.5 Restartability

The architecture distinguishes *restartable* primitives, which firmware
may abandon partway and which the caller may safely re-issue without
duplicating any side effect, from *non-restartable* primitives, which
once entered run to completion. Restartability is documented per
primitive in the descriptions that follow. A primitive interrupted by
an external event (e.g., the expiration of a scheduling quantum) is
either resumed at the point of interruption or restarted from
arguments; the architecture permits either choice and requires only
that the choice be transparent to the caller.

## 3. Primitive Number Allocation

Primitive numbers are partitioned into functional groups:

| Range            | Group                                            |
|------------------|--------------------------------------------------|
| `0x000`–`0x0FF`  | Task management                                  |
| `0x100`–`0x1FF`  | Memory management                                |
| `0x200`–`0x2FF`  | Communication                                    |
| `0x300`–`0x3FF`  | Device and I/O                                   |
| `0x400`–`0x4FF`  | Time and clocks                                  |
| `0x500`–`0x5FF`  | Hypervisor and system management                 |
| `0x700`–`0x7FF`  | Diagnostic                                       |
| All others       | Reserved for future extension                    |

A primitive number outside any allocated range raises the
`reserved-call` trap as Volume II specifies; firmware terminates the
calling task or returns `ENOSYS` per its policy.

## 4. Task Management Primitives

### 4.1 Task Lifecycle

**`0x000  TaskCreate`** — *Restartable.*

> Create a new task and prepare it to run. The task does not run until
> a subsequent `TaskResume`.
>
> Args:
> - `O1`: code object (must carry `X`).
> - `O2`: initial stack object (must carry `R` and `W`).
> - `R4`: byte offset within the code object at which to begin.
> - `R5`: initial value to place in `R4` of the new task.
>
> Returns:
> - `O1`: reference to the created task object, with `V` capability.
> - `R2`: status.
>
> Errors: `EPERM`, `ENOMEM`, `EFAULT`, `EINVAL`.

**`0x001  TaskExit`** — *Non-restartable.*

> Terminate the calling task with the given exit code. The task object
> may be queried for its exit code via `TaskQuery` after termination.
>
> Args:
> - `R4`: exit code.
>
> Does not return.

**`0x002  TaskResume`** — *Restartable.*

> Place the named task in the scheduler's runnable set.
>
> Args:
> - `O1`: task object (must carry `V`).
>
> Returns: `R2`: status. Errors: `EPERM`, `ESTALE`, `EBUSY`.

**`0x003  TaskSuspend`** — *Restartable.*

> Remove the named task from the scheduler's runnable set. A task may
> suspend itself; doing so blocks until another task resumes it.
>
> Args:
> - `O1`: task object (must carry `V`).
>
> Returns: `R2`: status. Errors: `EPERM`, `ESTALE`.

**`0x004  TaskYield`** — *Restartable.*

> Surrender the remainder of the current scheduling quantum. The task
> remains runnable.
>
> No arguments. Returns: `R2`: status (always `OK`).

**`0x005  TaskCurrent`** — *Restartable.*

> Return a reference to the calling task's task object.
>
> No arguments. Returns: `O1`: calling task's reference (caps include `V`).

**`0x006  TaskBindProcessor`** — *Restartable.*

> Suggest a processor on which the task should preferentially run. The
> firmware is not required to honour the suggestion.
>
> Args:
> - `O1`: task object (must carry `V`).
> - `R4`: preferred processor identifier, or 0xFF for "no preference".
>
> Returns: `R2`: status.

**`0x007  TaskWait`** — *Non-restartable.*

> Block the calling task until the named task has terminated. The exit
> code of the awaited task is returned in `R3`.
>
> Args:
> - `O1`: task object (no capability required).
>
> Returns: `R2`: status. `R3`: exit code on success.

**`0x008  TaskQuery`** — *Restartable.*

> Return summary information about a task: its state (runnable,
> suspended, terminated), its current processor, and its exit code if
> terminated.
>
> Args:
> - `O1`: task object.
>
> Returns: `R2`: status. `R3`: packed state word (state in low 8 bits,
> processor in next 8, exit code in upper 16).

**`0x009  InstallProgram`** — *Non-restartable.*

> Replace the calling task's running program with a new one and
> transfer control to its entry point. Used by chunked-boot loaders
> that have just finished assembling a guest program in two freshly
> allocated objects (one code, one data) and now need to map that
> guest at the standard `CODE_VA` / `DATA_VA` and jump in.
>
> Effects:
> 1. Every existing mapping in the calling task whose backing
>    descriptor is not of type `STACK` is dropped. The stack
>    mapping is preserved so the new program inherits a usable
>    stack.
> 2. The code reference is mapped at the platform's standard
>    `CODE_VA` (Vol I §5.4) with protection `R|X`. If a non-null
>    data reference is provided, it is mapped at the standard
>    `DATA_VA` with protection `R`.
> 3. All general registers are zeroed except `R7` (set to PROCID),
>    `R29` (set to `STACK_TOP - 16`), `R30` (set to 0), and `R31`
>    (set to 0) — the same initial state firmware would set for a
>    freshly loaded program. `O1` is set to the new code reference,
>    `O2` to the inherited stack reference, `O3` to the new data
>    reference (or null). `O4..O15` are *not* cleared; the loader
>    is expected to have arranged the service-reference layout the
>    new program expects before calling.
> 4. PC is set to `CODE_VA + R4` and the next instruction fetched
>    is the new program's entry point. The primitive does not
>    return to its caller in any meaningful sense — by the time it
>    completes, the caller's code is no longer mapped.
>
> Args:
> - `O1`: code reference (must have effective `R|X` capabilities;
>   home must be the calling processor).
> - `O3`: data reference (must have `R`; null permitted), or zero.
> - `R4`: entry offset within the new code (typically zero).
>
> Returns (visible only if a fault occurred before the jump): `R2`:
> status. On success there is no observable return because the next
> retired instruction is in the new program.
>
> Errors: `EREMOTE` (code or data home is not the calling
> processor), `EPERM` (insufficient capabilities on the supplied
> references), `ESTALE` (descriptor freed or generation mismatched),
> `EINVAL` (entry offset out of range, or code reference null).

### 4.2 Synchronization

**`0x010  SemAlloc`** — *Restartable.*

> Allocate a counting semaphore with the given initial count. Returns a
> reference to the semaphore as an object; capabilities are determined
> by the policy of the caller's address space.
>
> Args:
> - `R4`: initial count.
>
> Returns: `O1`: semaphore reference. `R2`: status.

**`0x011  SemAcquire`** — *Non-restartable.*

> Decrement the semaphore, blocking the calling task if the count is
> zero. The blocking is bounded by an optional timeout.
>
> Args:
> - `O1`: semaphore reference (must carry `S`).
> - `R4`: timeout in clock ticks, or zero for no wait, or 0xFFFFFFFF
>   for unbounded wait.
>
> Returns: `R2`: status. Errors include `ETIMEDOUT`.

**`0x012  SemRelease`** — *Restartable.*

> Increment the semaphore, possibly waking a blocked acquirer.
>
> Args:
> - `O1`: semaphore reference (must carry `S`).
>
> Returns: `R2`: status.

**`0x020  EventAlloc`**, **`0x021  EventWait`**, **`0x022  EventNotify`**

> A binary event is a semaphore with the constraint that its count
> never exceeds one. The three primitives mirror the semaphore
> primitives in their argument and return conventions.

## 5. Memory Management Primitives

### 5.1 Object Lifecycle

**`0x100  ObjAlloc`** — *Restartable.*

> Allocate a new object on the calling processor.
>
> Args:
> - `R4`: requested length in bytes.
> - `R5`: type tag (low 16 bits significant).
> - `R6`: initial maximum capabilities (low 8 bits significant).
>
> Returns:
> - `O1`: reference to the new object, with capabilities equal to the
>   requested maximum.
> - `R2`: status.
>
> Errors: `ENOMEM`, `EINVAL` (length zero or larger than implementation
> maximum).

#### 5.1.1 `0x106  ObjAllocStore` — *Restartable.*

> Like `ObjAlloc` but the new object's storage is *OR-typed*: its
> descriptor is allocated with the `OBJSTORE` flag set (Volume III
> Section 3.3). Integer `OL*`/`OS*` instructions trap on it; only
> `OREFLD`/`OREFST` (Volume II Section 10) may read or write the
> storage. This is the architectural mechanism by which object
> references can be saved to memory without breaching the capability
> invariant — see Volume III Section 5.4.
>
> Args:
> - `R4`: requested length in bytes; must be a non-zero multiple of
>   8 (one OR slot) and ≤ 2^24.
> - `R5`: type tag.
> - `R6`: initial maximum capabilities.
>
> Returns:
> - `O1`: reference to the new OR-typed object.
> - `R2`: status.
>
> Errors: `EINVAL` (length zero, not a multiple of 8, or too large),
> `ENOMEM`.

**`0x101  ObjFree`** — *Non-restartable.*

> Increment the descriptor's generation counter and return its storage
> to the local free pool. All outstanding references to the object
> become stale.
>
> Args:
> - `O1`: object reference (must carry `V`).
>
> Returns: `R2`: status. Errors: `EPERM`, `EREMOTE`.

**`0x102  ObjRevoke`** — *Non-restartable.*

> Increment the descriptor's generation counter without freeing the
> storage. The object's slot remains allocated and may be re-bound by
> firmware to a fresh storage region in a subsequent operation.
>
> Args:
> - `O1`: object reference (must carry `V`).
>
> Returns: `R2`: status. Errors: `EPERM`, `EREMOTE`.

**`0x103  ObjDerive`** — *Restartable.*

> Produce a new reference to the same object, with capabilities equal
> to the bitwise AND of the input reference's capabilities and the
> supplied mask.
>
> Args:
> - `O1`: reference (must carry `C`).
> - `R4`: capability mask (low 8 bits significant).
>
> Returns: `O1`: derived reference. `R2`: status.

**`0x104  ObjMigrate`** — *Non-restartable.*

> Move the named object to the named processor's local memory, leaving
> a forwarding stub at the original home as Volume III, Section 6.4
> describes.
>
> Args:
> - `O1`: object reference (must carry `M`).
> - `R4`: destination processor identifier.
>
> Returns: `R2`: status. Errors: `EPERM`, `EBUSY` (migration already in
> progress), `ENOMEM` (destination cannot allocate slot or storage).

**`0x105  ObjQuery`** — *Restartable.*

> Return summary information about an object: its current home, its
> length, its type tag, and its maximum capabilities. The reference's
> own effective capabilities are not returned (the caller already
> holds them).
>
> Args:
> - `O1`: reference.
>
> Returns:
> - `R2`: status.
> - `R3`: packed (home in low 8, max_caps in next 8, type_tag in upper 16).
> - On success a second word is delivered through the implementation's
>   side-channel — see Section 11.

### 5.2 Address-Space Mapping

**`0x110  MapObject`** — *Restartable.*

> Install page-table entries that resolve a chosen virtual range to the
> storage of the named object. The object's home must be the calling
> processor.
>
> Args:
> - `O1`: object reference. The bits selected by `R6` of this
>   reference's capabilities must include those mode bits requested.
> - `R4`: virtual address hint, or zero for "any".
> - `R5`: byte offset within the object at which the mapping begins.
> - `R6`: protection bits requested (low 3 bits: R, W, X).
> - `R7`: length of the mapping in bytes.
>
> Returns: `R2`: status. `R3`: virtual address at which the mapping
> was installed.
>
> Errors: `EREMOTE`, `EPERM`, `EINVAL`, `ENOMEM`.

**`0x111  Unmap`** — *Restartable.*

> Remove a mapping previously established by `MapObject`. The object's
> identity is recovered from the page table; no reference is required.
>
> Args:
> - `R4`: virtual address.
> - `R5`: length.
>
> Returns: `R2`: status.

**`0x112  Protect`** — *Restartable.*

> Modify the protection bits of an existing mapping. The new protection
> must be a subset of the capabilities the underlying reference
> originally carried at `MapObject` time; this constraint is enforced
> by firmware against the per-mapping record.
>
> Args:
> - `R4`: virtual address.
> - `R5`: length.
> - `R6`: new protection bits.
>
> Returns: `R2`: status.

### 5.3 Object Register Spill

The architecture provides no instruction by which an object register's
contents may be stored to or loaded from a general-register address,
on the grounds given in Volume III: to permit it would defeat the
capability invariant. The firmware therefore mediates the only
permitted form of object-register spill, into a per-task private
buffer that is not visible to user-mode integer access.

**`0x121  ORegSpill`** — *Restartable.*

> Save the contents of a named object register to a slot in the
> calling task's private spill buffer. Slots are addressed by index
> within the buffer; sixteen slots are available in any conforming
> firmware.
>
> Args:
> - `R4`: object register number to spill (0–15).
> - `R5`: spill buffer slot index (0–15).
>
> Returns: `R2`: status. Errors: `EINVAL`.

**`0x122  ORegRestore`** — *Restartable.*

> Restore an object register from a previously-spilled slot. The
> contents of the slot are unaffected; subsequent restores from the
> same slot return the same reference.
>
> Args:
> - `R4`: object register number to restore (0–15).
> - `R5`: spill buffer slot index (0–15).
>
> Returns: `R2`: status. Errors: `EINVAL`, `ENOENT` (slot empty).

The spill buffer is sized at sixteen slots in this revision. Code
that requires deeper spilling must marshal references through a
heap-allocated array object as Volume VII Section 2.4 describes.

## 6. Communication Primitives

**`0x200  InstallHandler`** — *Restartable.*

> Install a send handler on the named target object. The handler is
> identified by a code object and a byte offset.
>
> Args:
> - `O1`: target object (must carry `S` and `V`).
> - `O2`: handler code object (must carry `X`).
> - `R4`: byte offset within the handler code object.
>
> Returns: `R2`: status.

**`0x201  RemoveHandler`** — *Restartable.*

> Remove the send handler from the named target object. Subsequent
> `SEND`s to the object are dropped per the policy of Volume III,
> Section 7.2.
>
> Args:
> - `O1`: target object (must carry `V`).
>
> Returns: `R2`: status.

**`0x202  SendBuffered`** — *Restartable.*

> Issue a `SEND` whose target is known to be at risk of crossbar
> back-pressure, queueing the message in firmware-managed memory if
> the immediate `SEND` would stall or trap. The arguments mirror those
> of the `SEND` instruction.
>
> Args:
> - `O1`: recipient object (must carry `S`).
> - `O2`–`O4`: object payload.
> - `R4`–`R7`: integer payload.
>
> Returns: `R2`: status. Errors: `EAGAIN` (firmware queue is full).

**`0x203  ReceiveQueueAttach`** — *Restartable.*

> Replace the dispatch behaviour of the named target object: instead
> of invoking a handler, the firmware enqueues incoming messages on a
> per-object queue from which the calling task may explicitly pull.
>
> Args:
> - `O1`: target object (must carry `S` and `V`).
> - `R4`: maximum queue depth.
>
> Returns: `R2`: status.

**`0x204  ReceiveQueuePoll`** — *Non-restartable.*

> Dequeue the next message from a previously attached receive queue,
> blocking up to a timeout if the queue is empty.
>
> Args:
> - `O1`: target object (must carry `V`).
> - `R4`: timeout in clock ticks.
>
> Returns:
> - `R2`: status (`ETIMEDOUT` if no message arrived).
> - `O1`–`O4`: object payload of the dequeued message.
> - `R3`–`R6`: integer payload (note `R3` carries the first integer
>   argument here, not a return value, by exception to the usual
>   convention).

**`0x205  MessagePayloadOR4`** — *Restartable.*

> Return the fourth object reference from the SEND payload that
> dispatched the calling handler task. Volume III Section 7.2 specifies
> that handler dispatch overrides `O1` with a self-reference and
> delivers the wire's first three object references in `O2`–`O4`; the
> fourth reference is held in the side-channel buffer (Section 11) and
> retrieved through this primitive.
>
> Valid only when called from within a handler task whose dispatch
> originated as a `SEND_DELIVER` packet.
>
> No arguments.
>
> Returns: `O1`: the fourth payload reference (or null if the SEND's
> fourth payload slot was null). `R2`: status. Errors: `EINVAL` (not
> in a handler dispatch context).

**`0x210  ProcessorList`** — *Restartable.*

> Populate a buffer with the identifiers of every processor in the
> system reachable through the crossbar.
>
> Args:
> - `O1`: buffer object (must carry `W`).
> - `R4`: byte offset at which to begin writing.
> - `R5`: maximum number of identifiers to write.
>
> Returns: `R2`: status. `R3`: number of identifiers actually written.

## 7. Device and I/O Primitives

The set of devices present on a particular system is determined by
the integrator and discovered at runtime through the primitives of
this section. The architecture does not enumerate device classes; an
operating system identifies devices by the type tags of the device
objects returned from `DeviceList`, with conventions established by
the firmware integrator and documented in supplementary materials.

**`0x300  DeviceList`** — *Restartable.*

> Populate a buffer with references to every device object currently
> registered with firmware.
>
> Args:
> - `O1`: buffer object (must carry `W`).
> - `R4`: byte offset.
> - `R5`: maximum number of references to write.
>
> Returns: `R2`: status. `R3`: number of references actually written.

**`0x301  DeviceQuery`** — *Restartable.*

> Return identifying information about a device: its class tag, its
> revision, and a small fixed-format identifier string.
>
> Args:
> - `O1`: device reference.
> - `O2`: caller-supplied buffer for identifier string (must carry `W`).
>
> Returns: `R2`: status. `R3`: packed (class in low 16, revision in upper 16).

**`0x310  InterruptInstall`** — *Restartable.*

> Install a handler for the interrupt source named by the device
> object. The handler is invoked in the same manner as a `SEND`
> handler when the interrupt fires.
>
> Args:
> - `O1`: device reference (must carry `V`).
> - `O2`: handler code object (must carry `X`).
> - `R4`: handler offset.
>
> Returns: `R2`: status.

**`0x311  InterruptMask`** and **`0x312  InterruptUnmask`** — *Restartable.*

> Disable or enable the interrupt source. Both take a device reference
> bearing `V` and return a status. Repeated mask operations are
> idempotent.

**`0x320  ConsoleWrite`** and **`0x321  ConsoleRead`** — *Restartable
on read-side timeout, otherwise non-restartable.*

> Read or write the firmware-provided console. The console is the
> minimum I/O capability that any conforming firmware must expose; its
> precise device class is implementation-defined.
>
> `ConsoleWrite` arguments:
> - `O1`: source buffer (must carry `R`).
> - `R4`: byte offset.
> - `R5`: byte count.
>
> `ConsoleRead` arguments mirror these with `W` on the destination.
>
> Both return `R2`: status, `R3`: bytes actually transferred.

## 8. Time and Clock Primitives

**`0x400  TimeNow`** — *Restartable.*

> Return the current value of the system time, expressed as a 64-bit
> count of clock ticks since boot.
>
> No arguments. Returns: `R2`: status (always `OK`). `R3`: low 32
> bits. The high 32 bits are returned through the side-channel of
> Section 11.

**`0x401  TimerCreate`** — *Restartable.*

> Schedule the dispatch of a handler at a specified absolute system
> time.
>
> Args:
> - `O1`: handler object (must carry `X`).
> - `R4`: handler offset.
> - `R5`: low 32 bits of the absolute deadline.
> - `R6`: high 32 bits of the absolute deadline.
>
> Returns: `O1`: timer object (carries `V`). `R2`: status.

**`0x402  TimerCancel`** — *Restartable.*

> Cancel a previously scheduled timer. Cancellation is best-effort: a
> handler whose dispatch has already begun is permitted to run to
> completion.
>
> Args:
> - `O1`: timer object (must carry `V`).
>
> Returns: `R2`: status.

**`0x403  SleepUntil`** — *Non-restartable.*

> Block the calling task until the system time reaches the named
> absolute deadline.
>
> Args:
> - `R4`: low 32 bits of deadline.
> - `R5`: high 32 bits.
>
> Returns: `R2`: status.

**`0x410  ClockResolution`** — *Restartable.*

> Return the number of clock ticks per second.
>
> No arguments. Returns: `R2`: status. `R3`: ticks per second.

## 9. Hypervisor and System Management Primitives

The hypervisor primitives are optional. Firmware that does not
implement guest hosting returns `ENOSYS` from each of them; the
presence or absence of hypervisor support is reported by the
diagnostic primitive `Stat` (Section 11).

**`0x500  GuestCreate`** — *Restartable.*

> Allocate a guest container, into which a guest operating system
> image may subsequently be loaded.
>
> Args:
> - `R4`: maximum memory in bytes the guest may consume.
> - `R5`: bitmap of processors permitted to run guest tasks (low 16 bits).
>
> Returns: `O1`: guest reference (carries `V`). `R2`: status.

**`0x501  GuestLoad`** — *Restartable.*

> Load an executable image into the guest's address space. The guest
> must be in the unstarted state.
>
> Args:
> - `O1`: guest reference (must carry `V`).
> - `O2`: image object (must carry `R`).
>
> Returns: `R2`: status.

**`0x502  GuestStart`** — *Non-restartable.*

> Begin execution of the guest at its image's entry point.
>
> Args:
> - `O1`: guest reference (must carry `V`).
>
> Returns: `R2`: status.

**`0x503  GuestStop`** — *Non-restartable.*

> Halt the guest. The guest's state is preserved and may be inspected
> through `GuestQuery`.
>
> Args:
> - `O1`: guest reference (must carry `V`).
>
> Returns: `R2`: status.

**`0x504  GuestQuery`** — *Restartable.*

> Return summary information about a guest: its state, its memory
> consumption, and the number of tasks it has created.
>
> Args:
> - `O1`: guest reference.
>
> Returns: `R2`: status. `R3`: packed (state in low 8 bits, processor
> count in next 8, task count in upper 16).

**`0x510  SystemReset`** — *Non-restartable.*

> Reset the local processor as if by hardware reset. Other processors
> are unaffected.
>
> No arguments. Does not return.

**`0x511  SystemHalt`** — *Non-restartable.*

> Halt the local processor pending external intervention. Other
> processors are unaffected.
>
> No arguments. Does not return.

## 10. Diagnostic Primitives

**`0x700  Stat`** — *Restartable.*

> Return a counter from the firmware's instrumentation: total
> primitives invoked, total `SEND`s issued, total page faults serviced,
> and so on. The set of available counters is implementation-defined;
> well-known counters in the range 0–63 are listed in supplementary
> documents.
>
> Args:
> - `R4`: counter identifier.
>
> Returns: `R2`: status. `R3`: counter low 32 bits. High 32 bits are
> returned through the side-channel.

**`0x701  ProcessorID`** — *Restartable.*

> Return the calling processor's identifier and a bitmap of features
> the firmware reports on it.
>
> No arguments. Returns: `R2`: status. `R3`: packed (identifier in low
> 8, feature bits in upper 24).

**`0x702  DebugPrint`** — *Restartable.*

> Append a fixed-format diagnostic record to the firmware log. The
> primitive is intended for debugging and is permitted to be a no-op
> in production firmware images.
>
> Args:
> - `O1`: source buffer (must carry `R`).
> - `R4`: byte offset.
> - `R5`: byte count.
>
> Returns: `R2`: status.

## 11. The Side-Channel

Several mechanisms in this revision require carrying more state into
or out of a primitive than fits in the architectural argument and
return registers. Those mechanisms use a *side-channel*: a small
fixed-size object, allocated at task creation, whose reference is held
implicitly in the task's control block and whose contents firmware
reads or writes during the relevant operation.

The side-channel object is `RBUF` bytes long, where `RBUF` is at
least 64 in any conforming firmware. The layout of its contents is
specified per usage; this revision uses the side-channel in two
distinct ways.

**Overflow returns.** `TimeNow`, `Stat`, and `ObjQuery` deposit return
values that do not fit in `R2`, `R3`, or `O1` into the side-channel,
where the caller reads them on return. The high 32 bits of the
64-bit values returned by `TimeNow` and `Stat` occupy the first eight
bytes of the buffer; `ObjQuery` writes its second word at offset 16.

**Implicit input from dispatch.** When firmware dispatches a handler
task in response to `SEND`, the wire-format fourth object reference
of the payload (which has no register slot in the dispatch convention
of Volume III Section 7.2) is written to the dispatched task's
side-channel at offset 32. The handler retrieves it through
`MessagePayloadOR4` (Section 6).

The side-channel exists to keep the common-case calling convention
simple at the cost of an extra firmware-internal indirection in the
rare case where the architectural registers do not suffice. We expect
future revisions either to widen the convention or to standardize a
richer side-channel structure.

## 12. Conformance Requirements

A firmware implementation conforming to this revision shall implement
every primitive in groups `0x000`–`0x4FF` and `0x700`–`0x701`,
including the object-register spill primitives `0x121` and `0x122`
and the handler side-channel primitive `0x205`. The hypervisor group
(`0x500`–`0x5FF`) is optional; an implementation that does not
provide it returns `ENOSYS` from every primitive in the group and
reports the absence through `Stat` counter 16 ("hypervisor present",
returning zero).

The device discovery primitives `DeviceList` and `DeviceQuery` are
required, but the set of devices they enumerate is not specified by
this volume. Every conforming firmware exposes at least the console
device on which `ConsoleRead` and `ConsoleWrite` operate.

Implementations may extend the primitive set with locally defined
operations in any of the reserved ranges. Code using such extensions
is not portable to other Object RISC firmware implementations and
should be flagged as such by the toolchain.

The reserved primitive numbers within allocated groups (those not
named in this volume) shall return `ENOSYS` and shall not be
allocated by implementations to local extensions; future revisions
of this volume will consume them.

— *The Object RISC Architecture Group, 1986*
