# Object RISC Toolchain Contract

This document pins the minimum agreement between the Object RISC
**assembler** (under `tools/asm/`) and the Object RISC **simulator**
(under `tools/sim/`). Two agents are working on these pieces in
parallel; this contract is the only thing they coordinate on.

It does not replace the architecture spec (Volumes I–VII in this
repository). It pins the small handful of things the spec deliberately
leaves to the integrator — the on-disk binary format, the loader's
choice of where to map code/stack/data, and the host-side semantics of
the firmware primitives that the simulator implements directly in
Python instead of in actual Object RISC firmware.

## 1. The `.orx` Binary Format

A single file. All multi-byte fields are **big-endian** (matching the
architecture's endianness convention from Volume I).

| Offset | Size | Field                                                |
|--------|------|------------------------------------------------------|
| `0x00` | 8    | Magic: ASCII bytes `O R I S C` followed by three NULs (`0x4F 0x52 0x49 0x53 0x43 0x00 0x00 0x00`) |
| `0x08` | 4    | Format version, currently `1`                        |
| `0x0C` | 4    | Flags, currently `0`                                 |
| `0x10` | 4    | Entry offset within the text section, in bytes       |
| `0x14` | 4    | Text section size, in bytes                          |
| `0x18` | 4    | Data section size, in bytes (may be zero)            |
| `0x1C` | 4    | Stack size, in bytes (`0` means use the loader default of `0x10000` = 64 KiB) |
| `0x20` | T    | Text section bytes (T = text size); each instruction is a 32-bit big-endian word |
| `0x20+T` | D  | Data section bytes (D = data size); raw bytes        |

Total file size is `0x20 + T + D`. The header is exactly 32 bytes.
Text size must be a multiple of 4. Data size has no alignment
requirement.

The assembler produces this format. The simulator consumes it. A
loader that finds a magic mismatch, an unsupported version, or a
file-size discrepancy must reject the file.

## 1.1 The `.oro` Object File Format

The intermediate format consumed by the linker. An `.oro` file
holds the assembled output of one source file together with the
symbol and relocation metadata needed to combine it with other
`.oro` files into a single `.orx`. The simulator never sees this
format; it's strictly a contract between the assembler
(`asmorisc -r`) and the linker (`orld`). Big-endian throughout.

| Offset | Size | Field                                                                |
|--------|------|----------------------------------------------------------------------|
| `0x00` | 8    | Magic: ASCII bytes `ORISCOBJ` (`0x4F 0x52 0x49 0x53 0x43 0x4F 0x42 0x4A`) |
| `0x08` | 4    | Format version, currently `1`                                        |
| `0x0C` | 4    | Flags, currently `0`                                                 |
| `0x10` | 4    | Text section size, in bytes (multiple of 4)                          |
| `0x14` | 4    | Data section size, in bytes (no alignment requirement)               |
| `0x18` | 4    | Symbol-table entry count (each entry is 16 bytes)                    |
| `0x1C` | 4    | Relocation-table entry count (each entry is 12 bytes)                |
| `0x20` | T    | Text section bytes — instruction words for resolved sites; zero-padding at relocation sites |
| `0x20+T` | D  | Data section bytes                                                   |
| `0x20+T+D` | 16·N | Symbol table — N entries of 16 bytes each (layout below)         |
| `0x20+T+D+16N` | 12·R | Relocation table — R entries of 12 bytes each (layout below) |
| `0x20+T+D+16N+12R` | rest | String table — concatenated NUL-terminated symbol names    |

### Symbol entry (16 bytes)

| Offset | Size | Field                                                              |
|--------|------|--------------------------------------------------------------------|
| `0x00` | 4    | Name offset within the string table                                |
| `0x04` | 4    | Value (offset within the symbol's section, or zero for undefined)  |
| `0x08` | 1    | Section: `0` undefined, `1` text, `2` data, `3` absolute           |
| `0x09` | 1    | Binding: `0` local, `1` global                                     |
| `0x0A` | 1    | Type: `0` none, `1` function, `2` data                             |
| `0x0B` | 5    | Reserved (zero)                                                    |

A symbol entry with `section = 0` is an undefined reference — the
file expects another `.oro` to provide a definition at link time.
Local symbols are kept in the table only so relocations within the
same file can name them; the linker discards them after applying
the relocations.

### Relocation entry (12 bytes)

| Offset | Size | Field                                                              |
|--------|------|--------------------------------------------------------------------|
| `0x00` | 4    | Offset within the target section                                   |
| `0x04` | 1    | Target section: `1` text, `2` data                                 |
| `0x05` | 1    | Relocation type (see below)                                        |
| `0x06` | 2    | Reserved (zero)                                                    |
| `0x08` | 4    | Symbol-table index (zero-based)                                    |

### Relocation types

| Code   | Name              | Effect                                                                            |
|--------|-------------------|-----------------------------------------------------------------------------------|
| `0x00` | `R_ORISC_NONE`    | No-op (reserved)                                                                  |
| `0x01` | `R_ORISC_ABS32`   | Patch the full 32-bit word at the offset with the symbol's absolute virtual address |
| `0x02` | `R_ORISC_HI16`    | Patch the low 16 bits of the instruction word with the upper 16 bits of the symbol's VA (for `lui`) |
| `0x03` | `R_ORISC_LO16`    | Patch the low 16 bits of the instruction word with the lower 16 bits of the symbol's VA (for `ori`/`addiu`) |
| `0x04` | `R_ORISC_BRANCH16`| Patch the low 16 bits of the instruction word with the signed (target_VA − (instr_VA + 4)) >> 2; assembled with this reloc only when the target is undefined in this file |
| `0x05` | `R_ORISC_J26`     | Patch the low 26 bits of the instruction word with `(target_VA >> 2) & 0x03FFFFFF` (for `j`/`jal`) |

`HI16` and `LO16` use the simple high/low split — the upper 16
bits are `(addr >> 16) & 0xFFFF`, no sign-extension adjustment.
This is correct for `lui` followed by `ori` (which zero-extends);
do not use the LO16 reloc with `addiu` unless the address is
known to fit in the low 16 bits unsigned.

`BRANCH16` and `J26` patches expect the existing low bits to be
zero (the assembler has emitted a placeholder).

## 1.2 The `.ora` Archive Format

A bundle of one or more `.oro` object files plus a symbol-to-member
index. Functions like `ar(1)` plus `ranlib(1)` rolled into one file:
the linker (`orld`) consults the symbol index to pull in only the
members that satisfy unresolved external references, and ignores the
rest. Big-endian throughout.

| Offset | Size | Field                                                                |
|--------|------|----------------------------------------------------------------------|
| `0x00` | 8    | Magic: ASCII bytes `ORISCARC` (`0x4F 0x52 0x49 0x53 0x43 0x41 0x52 0x43`) |
| `0x08` | 4    | Format version, currently `1`                                        |
| `0x0C` | 4    | Flags, currently `0`                                                 |
| `0x10` | 4    | Member count (M)                                                     |
| `0x14` | 4    | Symbol-index entry count (S)                                         |
| `0x18` | 4    | Index size in bytes (member dir + symbol index + string table)       |
| `0x1C` | 4    | Reserved (zero)                                                      |
| `0x20` | 12·M | Member directory — M entries of 12 bytes each (layout below)         |
| `0x20+12M` | 8·S | Symbol index — S entries of 8 bytes each (layout below)          |
| `0x20+12M+8S` | rest of index | String table — concatenated NUL-terminated names      |
| `0x20 + index_size` | … | Member data — each member is a complete `.oro` file inline   |

### Member directory entry (12 bytes)

| Offset | Size | Field                                                      |
|--------|------|------------------------------------------------------------|
| `0x00` | 4    | Name offset within the string table                        |
| `0x04` | 4    | Member offset (absolute byte offset in the `.ora` file)    |
| `0x08` | 4    | Member size in bytes                                       |

### Symbol index entry (8 bytes)

| Offset | Size | Field                                                      |
|--------|------|------------------------------------------------------------|
| `0x00` | 4    | Name offset within the string table                        |
| `0x04` | 4    | Member index (zero-based) that defines this symbol         |

The index lists every defined global from every member, so `orld`
can decide selective inclusion in a single pass: scan its list of
unresolved externals, look each up in the index, pull in the
member that defines it, repeat until no new externals appear.

Local symbols, undefined symbols, and the per-member relocation
records are not in the archive index — they live inside each `.oro`
member as usual.

## 2. The Initial Task State

When the simulator loads an `.orx`, it sets up the initial task as if
the following firmware sequence had run:

1. `code = ObjAlloc(text_size, type_tag=0x4100, max_caps=R|X|C)` and the
   text section bytes are copied into the new object's storage.
2. `MapObject(code, va=0x00010000, offset=0, length=text_size, prot=R|X)`.
3. `stack = ObjAlloc(stack_size, type_tag=0x4101, max_caps=R|W|C)`.
4. `MapObject(stack, va=0x00200000-stack_size, offset=0, length=stack_size, prot=R|W)`
   — i.e., the stack is mapped so that its highest byte address is just
   below `0x00200000`.
5. If `data_size > 0`: `data = ObjAlloc(data_size, type_tag=0x4102, max_caps=R|C)` and the
   data section bytes are copied into the new object's storage; then
   `MapObject(data, va=0x00040000, offset=0, length=data_size, prot=R)`.
   Otherwise `data = null`.
6. The initial task begins execution at:
   - `PC = 0x00010000 + entry_offset`
   - `SP (R29) = 0x00200000 - 16` (16-byte alignment, leaving the
     standard outgoing-arg-spill area at the top of the stack)
   - `FP (R30) = 0`
   - `RA (R31) = 0`
   - `O0 = null` (architecturally hardwired)
   - `O1 = code` (with full max_caps)
   - `O2 = stack` (with full max_caps)
   - `O3 = data` (with full max_caps if data_size > 0, otherwise null)
   - `O4`–`O15`, `R1`–`R6`, `R8`–`R28` = 0
   - `R7` = the calling processor's identifier (`PROCID`); see Section 2.1
   - All control registers per their reset values

The capabilities listed are the **maximum** capabilities of each
object's descriptor; the references handed to the task carry those
capabilities verbatim (the task may weaken them through `ObjDerive` if
desired).

A program that returns from `main` (i.e., executes `JR RA` with
`RA = 0`) traps on instruction-fetch from address `0x00000000`, which
the simulator reports as "main returned without TaskExit". Programs
should always end with an explicit `CALL #0x001`.

### 2.1 Multi-CPU Mode

When the simulator is invoked with `--processors N` (N ≥ 2), the same
`.orx` is loaded onto every processor, and additional state is set up
to let processors talk to each other:

1. **`R7` carries each CPU's `PROCID`** (0, 1, …, N−1). Single-CPU
   mode also writes `R7 = 0`. Programs use this register to branch on
   role (server vs client, etc.).
2. **One *service* object is allocated per processor**, of length 64
   bytes and type tag `0x4103`, with `max_caps = R|W|S|V|C`.
3. **`O4` holds my own service object's reference**, with full
   capabilities, so I can install a SEND handler on it and write to
   its storage.
4. **`O5`, `O6`, … `O5+N−2` hold references to the other processors'
   service objects**, in PID order skipping my own, with capabilities
   restricted to `R|S` only — I can SEND to them and read them, but
   I cannot install handlers on them, modify their storage through
   `OS*`, or revoke them.

Service objects are the bootstrap channel: they let processors
exchange messages without needing a name-server protocol. A typical
two-CPU program installs a handler on its own service (via `O4` and
`InstallHandler`), then any peer SENDs through its `O5`-shaped
reference to invoke it.

Handler dispatch on a CPU happens when (a) the CPU has no active task
(its main has called `TaskExit`) and (b) its inbox holds a delivered
SEND. The simulator restores the calling CPU's register snapshot from
the moment of the most recent `TaskExit`, then overlays the handler-
specific registers per Volume III Section 7.2 (`O1` = fresh full-caps
self-reference, `O2`–`O4` = first three wire OR payload slots,
`R4`–`R7` = wire integer payload). This is a simulator implementation
choice, not architectural — it gives main a clean way to hand state
to a handler by setting registers (e.g., `OMOV O15, O3` to preserve
the data ref) before exiting.

## 3. The Firmware Primitives the Simulator Must Implement

The authoritative set of primitives this firmware implements is the
`PRIMITIVE_MIN_MODE` dispatch gate in `tools/sim/simorisc`: a `CALL`
number present there runs its primitive (subject to the listed minimum
caller mode); **any number not present returns `R2 = 4` (`ENOSYS`)**.
The complete set is tabulated below. The numbered subsections that
follow (§3.1–§3.10) give host-side detail for a representative subset;
every primitive marked "Vol VI" follows that volume's ABI unless a
subsection here notes a simulator-specific deviation.

| `CALL` | Primitive | Mode | Specified in | Notes |
|--------|-----------|:----:|--------------|-------|
| `0x000` | `TaskCreate`          | S | Vol VI §4.1 | |
| `0x001` | `TaskExit`            | U | Vol VI §4.1; §3.1 | |
| `0x002` | `TaskResume`          | S | Vol VI §4.1 | |
| `0x003` | `TaskSuspend`         | S | Vol VI §4.1 | |
| `0x004` | `TaskYield`           | U | Vol VI §4.1 | |
| `0x005` | `TaskCurrent`         | U | Vol VI §4.1 | |
| `0x007` | `TaskWait`            | U | Vol VI §4.1 | |
| `0x008` | `TaskQuery`           | U | Vol VI §4.1 | |
| `0x009` | `InstallProgram`      | S | Vol VI §4.1 | |
| `0x00A` | `TaskKill`            | U | Vol VI §4.1 | |
| `0x100` | `ObjAlloc`            | U | Vol VI §5.1; §3.2 | |
| `0x101` | `ObjFree`             | U | Vol VI §5.1; §3.3 | |
| `0x103` | `ObjDerive`           | U | Vol VI §5.1; §3.4 | |
| `0x106` | `ObjAllocStore`       | U | Vol VI §5.1.1; §3.2.1 | |
| `0x107` | `ObjFreeDeferred`     | U | Vol VI §5.1.2 | |
| `0x108` | `ObjFetchBytes`       | U | Vol VI §5.1.3 | |
| `0x109` | `ObjStoreBytes`       | U | Vol VI §5.1.4 | |
| `0x10A` | `ObjAllocFramebuffer` | U | Vol VI §5.4 | Phase 60; formerly `0x102` (§3.0.1) |
| `0x10B` | `ObjAllocInputSink`   | U | Vol VI §5.4 | Phase 60 |
| `0x10C` | `ObjBlitGlyphs`       | U | Vol VI §5.4 | Phase 60 |
| `0x10D` | `ObjFillRect`         | U | Vol VI §5.4 | Phase 60 |
| `0x10E` | `ObjFbScroll`         | U | Vol VI §5.4 | Phase 60 |
| `0x10F` | `ObjBlitCopy`         | U | Vol VI §5.4 | Phase 60 |
| `0x110` | `MapObject`           | S | Vol VI §5.2; §3.5 | |
| `0x111` | `Unmap`               | S | Vol VI §5.2 | |
| `0x200` | `InstallHandler`      | S | Vol VI §6; §3.6 | |
| `0x203` | `ReceiveQueueAttach`  | U | Vol VI §6; §3.7 | |
| `0x204` | `ReceiveQueuePoll`    | U | Vol VI §6; §3.8 | |
| `0x206` | `WaitAnyQueue`        | U | Vol VI §6; §3.8.1 | Readiness wait over a queue set |
| `0x301` | `ReadCycles`          | U | Vol VI §7; §3.9 | |
| `0x320` | `ConsoleWrite`        | U | Vol VI §7; §3.10 | |
| `0x400` | `TimeNow`             | U | Vol VI §8 | |
| `0x410` | `ClockResolution`     | U | Vol VI §8 | |
| `0x520` | `InstallTrapHandler`  | S | Vol VI §9 | |

(Mode: U = user, S = supervisor, per Volume VI §2.4.) Volume VI
primitives **not** in this table — `ObjRevoke`, `ObjMigrate`, `ObjQuery`,
`Protect`, `DeviceQuery`, the semaphore/event/timer/guest groups, and
others — are **not implemented** by this firmware and return `ENOSYS`.

### 3.0.1 Spec/implementation number conflicts

All three divergences from Volume VI's number allocation have been
resolved. **The implemented numbers in the table above are authoritative
for this firmware.** This subsection is kept as the rationale log for why
those numbers sit where they do.

**(a) `0x102` — RESOLVED.** `ObjAllocFramebuffer` formerly occupied
`0x102`, which Volume VI §5.1 names `ObjRevoke` (generation-bump
capability revocation). Because `dispatch_call` resolves a number to
exactly one primitive (else `ENOSYS`), a spec-conformant `CALL #0x102` to
**revoke** a capability would have silently run the framebuffer allocator
and never revoked — a latent capability-safety trap. `ObjAllocFramebuffer`
was relocated to `0x10A`, and `0x102` is now reserved for `ObjRevoke`
(still unimplemented → `ENOSYS`). `CALL #0x102` is therefore safe again:
it returns `ENOSYS` rather than mis-dispatching.

**(c) `0x10A`–`0x10F` reserved-number use — RESOLVED.** The display and
input primitives occupy numbers in the memory-management group. Volume VI
§5.4 now names them and §12 blesses them as an optional group, so they are
spec-defined primitives rather than non-portable local extensions.

**(b) `0x301` — RESOLVED.** This firmware runs `ReadCycles` at `0x301`
(§3.9); Volume VI §7 formerly named `0x301` `DeviceQuery`, which this
firmware does not implement. Because `ReadCycles` backs a public
`read_cycles()` API, it was kept in place and instead **promoted into
Volume VI at `0x301`**, with `DeviceQuery` **renumbered to `0x302`**. (Of
the two sanctioned dispositions, renumbering was chosen over retiring
`DeviceQuery` because `DeviceList` (`0x300`) still needs a query companion
and Volume VI §12 requires the pair.)

### 3.1 `0x001 TaskExit`

Inputs: `R4` = exit code (low 8 bits used).

Effect: the simulator process exits with the given exit code.

### 3.2 `0x100 ObjAlloc`

Allocate a new object on the calling processor.

Inputs:
- `R4` = length in bytes (1 ≤ R4 ≤ 2^24).
- `R5` = type tag (low 16 bits).
- `R6` = initial maximum capabilities (low 8 bits).

Returns:
- `O1` = reference to the new object (carries `init_caps`).
- `R2` = `OK` on success, `EINVAL` for zero or oversized length,
  `ENOMEM` on allocator failure.

### 3.2.1 `0x106 ObjAllocStore`

Like `ObjAlloc` but the new object's storage is *OR-typed*
(`OBJSTORE` flag set in the descriptor): integer `OL*`/`OS*` trap
on it with `capability-violation`, and only `OREFLD`/`OREFST`
(Section 5.12) may read or write the storage. This is the mechanism
by which object references can be saved to memory without breaching
the capability invariant — the bytes are never observable as
integers, and the storage cannot be written with arbitrary patterns.

Inputs:
- `R4` = length in bytes (must be a non-zero multiple of `8` and
  ≤ 2^24).
- `R5` = type tag (low 16 bits).
- `R6` = initial maximum capabilities (low 8 bits).

Returns:
- `O1` = reference to the new OBJSTORE object.
- `R2` = `OK` or `EINVAL` (length zero, not a multiple of 8, or too
  large) or `ENOMEM`.

### 3.3 `0x101 ObjFree`

Increment the descriptor's generation and recycle its slot. All
outstanding references to the object become stale immediately.

Inputs: `O1` = reference (must carry `V` and have `home == PROCID`).

Returns: `R2` = `OK`, or `EFAULT` (null), `EPERM` (no V),
`EREMOTE` (foreign home), `ESTALE` (already freed).

### 3.4 `0x103 ObjDerive`

Produce a new reference to the same object with weakened capabilities.

Inputs: `O1` = reference (must carry `C`); `R4` = capability mask
(low 8 bits significant).

Returns: `O1` = derived reference with caps = `O1.caps & R4`;
`R2` = `OK` or `EPERM`/`EFAULT`.

### 3.5 `0x110 MapObject`

Install page-table entries that resolve a chosen virtual range to the
storage of a target object. The object's home must be the calling
CPU. Future revisions may add an `Unmap` companion primitive.

Inputs:
- `O1` = target object (home must be calling CPU).
- `R4` = virtual address hint, or `0` to let firmware choose. With a
  non-zero hint the address is honoured verbatim and the program is
  responsible for collision avoidance; with `0` firmware picks a
  fresh region above the loadable-module floor (`0x00100000`).
- `R5` = byte offset within the object at which the mapping begins.
- `R6` = protection bits (low three bits: R, W, X). Must be a subset
  of the calling reference's effective capabilities restricted to
  R/W/X.
- `R7` = length of the mapping in bytes.

Returns:
- `R2` = `OK`, `EFAULT`, `EREMOTE`, `EPERM`, `ESTALE`, or `EINVAL`.
- `R3` = the virtual address at which the mapping was installed.

### 3.6 `0x200 InstallHandler`

Install a SEND handler on a target object. The handler runs as a
fresh task on the target's home CPU when a SEND_DELIVER arrives and
that CPU is idle.

Inputs:
- `O1` = target object (must carry both `S` and `V`; home must be the
  calling CPU).
- `O2` = handler code object (must carry `X`). The code object must
  have an executable mapping (`prot & X`) installed on the calling
  CPU that contains the byte offset specified in `R4`. The boot text
  satisfies this trivially (`init_cpu` maps it at `0x00010000`);
  loadable modules satisfy it once they have been `MapObject`'d as
  R+X (Section 3.5).
- `R4` = byte offset within the handler code object at which the
  handler begins.

Returns: `R2` = `OK`, `EPERM` (caps), `EREMOTE` (target lives
elsewhere), `EINVAL` (handler offset not in any executable mapping
of the handler code object on this CPU), `EFAULT`, or `ESTALE`.

### 3.7 `0x203 ReceiveQueueAttach`

Replace handler-dispatch on a target object with a per-object queue.
Subsequent `SEND_DELIVER`s addressed to the object are appended to
the queue rather than spawning a handler task; the queue is drained
by `ReceiveQueuePoll` (Section 3.8). This is the *pull* alternative
to handler dispatch, used for synchronous RPC reply patterns
(Volume VII Section 4.3).

Inputs:
- `O1` = target object (must carry both `S` and `V`; home must be
  the calling CPU).
- `R4` = maximum queue depth (1..1024). Arrivals beyond this depth
  are dropped on receipt.

Returns: `R2` = `OK`, `EFAULT`, `EPERM`, `EREMOTE`, `ESTALE`, or
`EINVAL`.

### 3.8 `0x204 ReceiveQueuePoll`

Dequeue the next message from a previously attached receive queue.
Blocks the calling CPU at the `CALL` instruction until a message
arrives, the timeout expires, or — with `timeout = 0` — returns
`ETIMEOUT` immediately.

Inputs:
- `O1` = target object (must carry `V`; must have had
  `ReceiveQueueAttach` called on it; home must be the calling CPU).
- `R4` = timeout in scheduler ticks. `0` = no wait;
  `0xFFFFFFFF` = wait forever; otherwise the queue is checked each
  scheduler tick and the timeout is decremented.

Returns:
- `R2` = `OK` (message dequeued), `ETIMEOUT`, `EFAULT`, `EPERM`,
  `EREMOTE`, `ESTALE`, or `EINVAL`.
- On success, the dequeued message's wire payload is delivered
  *verbatim* into registers (no `O1` self-override, unlike handler
  dispatch — by exception to the usual return convention because the
  caller already knows what queue it polled):
  - `O1`–`O4` = the SEND's four wire OR payload references.
  - `R3`–`R6` = the SEND's four wire integer payload words. Note
    that `R3` carries the first integer payload word, *not* a return
    value — the status occupies `R2` alone.

### 3.8.1 `0x206 WaitAnyQueue`

The multi-queue companion to `ReceiveQueuePoll`: a **pure readiness
wait** over a *set* of receive queues. It blocks the calling CPU until
**any** listed queue is non-empty, then returns `OK` **without
dequeuing** — the caller drains the actual messages with its own
per-queue `ReceiveQueuePoll`s. This is the missing "wait on many queues
at once" primitive: a multiplexing server (the window manager) can
idle-block instead of busy-polling each of its queues on a short timeout
(the old ~100 ms `WM_POLL_TICKS` interactive-latency floor).

The queue set is dynamic and outgrows the object registers (a window
manager has a per-window queue that comes and goes), so it is passed by
memory, not in registers.

Inputs:
- `O2` = an `OBJSTORE` object whose first `R5` 8-byte slots hold packed
  64-bit queue references (the `OREFST` layout). Must carry `R`.
- `R5` = the number of references to read (count).
- `R6` = optional timeout in **microseconds** — `0` = infinite block (the
  default and the historic behaviour); `>0` resumes the `CALL` with
  `ETIMEOUT` if no listed queue becomes ready within that span. Lets the
  WM idle-block yet still wake for a timer (e.g. scrollbar auto-repeat)
  without reviving the old busy-poll floor.

Each listed reference is validated exactly like `ReceiveQueuePoll`'s
`O1` target — `V` cap, home == the calling CPU, live descriptor,
matching generation, a receive queue attached — but an entry that
**fails** validation (a null slot, or the stale ref of a freed
per-window queue, or a non-queue object) is **skipped, not fatal**: the
set legitimately holds slots that come and go between iterations, and
the caller rebuilds it each loop. A malformed *list object* (`O2`
itself) is still an error.

Returns:
- `R2` = `OK` once at least one listed queue is non-empty (immediately
  if one already is); `ETIMEOUT` if a finite `R6` timeout expired first;
  `EFAULT` / `EPERM` / `EREMOTE` / `ESTALE` / `EINVAL` if the list object
  itself is unusable.
- `R3` = `0`. No payload is delivered — this is a readiness signal, not
  a dequeue; registers and object registers are otherwise untouched.

With `R6 = 0` the block is **infinite**: the CPU parks at the `CALL`
until a queue fills, then advances. The wake works cross-CPU — when a
remote `SEND` lands on a listed home queue via the crossbar, the
scheduler resumes the parked caller the same tick. With `R6 > 0` the wait
additionally carries a **wall-clock deadline** (real microseconds, not
scheduler ticks — so it fires even while the caller is parked and other
CPUs run); once that span elapses with no queue ready, the `CALL` resumes
with `ETIMEOUT`, within the scheduler's ~1 ms idle re-poll. `R6` is a
backward-compatible extension: existing callers that leave it `0` keep
the original infinite-block semantics.

**Numbering.** `0x206` is the first free hole in the Communication group
(§6) after `ReceiveQueuePoll`; `0x205` is reserved by Volume VI for
`MessagePayloadOR4` and was skipped (an unnamed hole was claimed, per
the §3.0.1 discipline). Volume VI §6 has been extended to define
`0x206` so the spec and this implementation stay in lockstep.

### 3.9 `0x301 ReadCycles`

Read the calling CPU's retired-instruction counter — its local
"cycle count" in the simulator. This is a measurement primitive
useful for benchmarks and bring-up; it is not modelled as a
production architectural facility.

Inputs: none.

Returns:
- `R2` = `0` (always succeeds; no error path).
- `R3` = the calling CPU's cycle count, low 32 bits.

The counter starts at zero on simulator boot, advances by one per
retired instruction (including `nop`, including the instruction
implementing this `CALL`), and wraps at 2^32. For benchmark loops
that take more than ~4 billion instructions to complete, sample
twice and check for wrap explicitly; for everything that fits in a
single 32-bit window, subtract.

`ReadCycles` is also specified in Volume VI Section 7, promoted there at
`0x301` (displacing `DeviceQuery` to `0x302`); see Section 3.0.1.

### 3.10 `0x320 ConsoleWrite`

Inputs:
- `O1` = source object reference. Must be non-null and carry `R`. Must
  not be stale. May be remote — the simulator routes to the home CPU's
  descriptor table.
- `R4` = byte offset within the object.
- `R5` = byte count.

Effect: the simulator writes `R5` bytes starting at byte `R4` of the
object's storage to the host process's stdout, then flushes.

Returns:
- `R2` = `0` on success, error code otherwise.
- `R3` = number of bytes actually written (equals `R5` on success).

Errors:
- `EFAULT` (8) if `O1` is null.
- `EPERM` (3) if `O1` does not carry `R`.
- `ESTALE` (10) if `O1`'s generation does not match the descriptor.
- `EINVAL` (1) if `R4 + R5 > length(O1)` or arithmetic overflow.

## 4. Assembly Syntax

### 4.1 Lexical

- Whitespace separates tokens. Indentation is not significant.
- Comments begin with `;` and run to end of line.
- Identifiers match `[A-Za-z_.][A-Za-z0-9_.]*`. They are case-sensitive.
- Numeric literals: decimal `42`, hex `0x2A` or `0X2A`, binary `0b101010`,
  ASCII character `'X'` (value of one byte). A leading `-` makes the
  literal negative.
- String literals: double-quoted, with C-style escapes recognized: `\n`,
  `\t`, `\r`, `\\`, `\"`, `\0`, `\xHH` (two hex digits).

### 4.2 Directives

| Directive             | Effect                                              |
|-----------------------|-----------------------------------------------------|
| `.text`               | Switch to the text section                          |
| `.data`               | Switch to the data section                          |
| `.entry <label>`      | Set the program entry point to the byte offset of `<label>` within the text section |
| `.set NAME, EXPR`     | Define a numeric constant (see Section 4.8)         |
| `.byte b, b, ...`     | Emit one or more bytes (each may be an expression)  |
| `.half h, h, ...`     | Emit one or more 16-bit halfwords (big-endian)      |
| `.word w, w, ...`     | Emit one or more 32-bit words (big-endian)          |
| `.string "..."`       | Emit the string bytes (no null terminator)          |
| `.asciz "..."`        | Emit the string bytes followed by a `0x00`          |
| `.align N`            | Align the current section to a 2^N byte boundary by inserting zero bytes (N is small, e.g. 0–4); N must be constant |
| `.skip N`             | Emit N zero bytes; N must be constant               |

A label of the form `name:` defines a symbol whose value is the
current section's byte offset (and resolves at use sites to its
absolute virtual address — `0x10000 + offset` for text labels,
`0x40000 + offset` for data labels).

### 4.3 Register Aliases

| Canonical | Aliases                                                  |
|-----------|----------------------------------------------------------|
| `r0`      | `zero`                                                   |
| `r1`      | `at`                                                     |
| `r2`–`r3` | `v0`, `v1`                                               |
| `r4`–`r7` | `a0`, `a1`, `a2`, `a3`                                   |
| `r8`–`r15`| `t0`–`t7`                                                |
| `r16`–`r23`| `s0`–`s7`                                               |
| `r24`–`r28`| `t8`–`t12`                                              |
| `r29`     | `sp`                                                     |
| `r30`     | `fp`                                                     |
| `r31`     | `ra`                                                     |
| `o0`      | `null`                                                   |

### 4.4 Pseudo-Instructions

Expanded by the assembler at parse time.

| Pseudo                   | Expansion                                             |
|--------------------------|-------------------------------------------------------|
| `nop`                    | `sll r0, r0, 0`                                       |
| `move rd, rs`            | `addu rd, rs, r0`                                     |
| `b label`                | `beq r0, r0, label`                                   |
| `li rd, imm`             | If `imm` fits in a sign-extended 16-bit field: `addiu rd, r0, imm`. Otherwise: `lui rd, hi(imm); ori rd, rd, lo(imm)` (where `hi` and `lo` are the upper and lower 16 bits, with `hi` adjusted upward by 1 if the low half is negative when sign-extended). |
| `la rd, label`           | Same as `li rd, abs_address(label)`                   |

### 4.5 Branch Displacements

Conditional branches and `J`/`JAL` take label operands. The assembler
computes the displacement from the address of the *delay-slot
instruction* (i.e., `PC + 4` of the branch) and encodes it as a 16-bit
signed word offset for conditional branches and as a 26-bit absolute
word index for `J`/`JAL`. The label's section must match the current
section.

### 4.6 `CALL`

`call #N` and `call N` are equivalent; both encode primitive number
`N` (0 ≤ N < 2^26) into the J-type instruction.

### 4.7 `SEND`

`send Os` takes a single object register operand.

### 4.8 Expressions and `.set`

Anywhere a numeric immediate is accepted — instruction operands
(`addiu`, `lui`, `li`, load/store/OREFLD/OREFST offsets, etc.) and
`.byte`/`.half`/`.word` data values — the operand may be an
*expression*: a sum of numeric literals and identifiers, joined by
`+` and `-` operators (with whitespace).

```
expression  := term (('+' | '-') term)*
term        := numeric_literal | identifier
identifier  := label_name | constant_name
```

An identifier resolves to:
- the value of a `.set` constant, recursively (cycles are detected);
- otherwise, the absolute virtual address of a label.

`.set NAME, EXPR` defines a numeric constant. The expression is
stored unevaluated and resolved at pass 2, so it may freely refer to
labels (including ones defined later in the source) and to other
constants.

Expressions accepting forward references (immediates, `.byte/.half/
.word` values) are resolved in pass 2 via the assembler's existing
deferred-encoding path — the layout is fixed at parse time, so a
symbolic immediate to `li` always emits the two-instruction
`lui`+`ori` form regardless of whether the resolved value would have
fit in a single ADDIU.

`.align` and `.skip` arguments must be *constant* expressions (no
symbol references), since they affect layout and resolve eagerly at
parse time.

Examples:

```
.set N, 2000
.set FOO, N - 1              ; constants may chain

addiu r4, r0, N              ; immediate from .set
addiu r5, r0, str_end - str  ; length via label arithmetic
li    r6, prime_handler - CODE_BASE   ; offset into a code object
.word my_label               ; symbolic .word (deferred fixup)
```

This revision supports `+` and `-` only. Multiplication, division,
and shifts are not yet implemented.

## 5. Instruction Encodings

### 5.1 Major Opcodes

| Op    | Mnemonic | Op    | Mnemonic |
|-------|----------|-------|----------|
| `0x00`| SPECIAL  | `0x20`| LB       |
| `0x01`| REGIMM   | `0x21`| LH       |
| `0x02`| J        | `0x23`| LW       |
| `0x03`| JAL      | `0x24`| LBU      |
| `0x04`| BEQ      | `0x25`| LHU      |
| `0x05`| BNE      | `0x28`| SB       |
| `0x06`| BLEZ     | `0x29`| SH       |
| `0x07`| BGTZ     | `0x2B`| SW       |
| `0x08`| ADDI     | `0x30`| OBJECT   |
| `0x09`| ADDIU    | `0x31`| OLB      |
| `0x0A`| SLTI     | `0x32`| OLH      |
| `0x0B`| SLTIU    | `0x33`| OLW      |
| `0x0C`| ANDI     | `0x34`| OLBU     |
| `0x0D`| ORI      | `0x35`| OLHU     |
| `0x0E`| XORI     | `0x36`| OREFLD   |
| `0x0F`| LUI      | `0x37`| OREFST   |
|       |          | `0x38`| OSB      |
|       |          | `0x39`| OSH      |
|       |          | `0x3B`| OSW      |
|       |          | `0x3C`| SEND     |
|       |          | `0x3D`| CALL     |

### 5.2 SPECIAL Funct Codes (opcode = `0x00`)

| Funct  | Mnemonic | Funct  | Mnemonic |
|--------|----------|--------|----------|
| `0x00` | SLL      | `0x20` | ADD      |
| `0x02` | SRL      | `0x21` | ADDU     |
| `0x03` | SRA      | `0x22` | SUB      |
| `0x04` | SLLV     | `0x23` | SUBU     |
| `0x06` | SRLV     | `0x24` | AND      |
| `0x07` | SRAV     | `0x25` | OR       |
| `0x08` | JR       | `0x26` | XOR      |
| `0x09` | JALR     | `0x27` | NOR      |
| `0x10` | MFHI     | `0x2A` | SLT      |
| `0x12` | MFLO     | `0x2B` | SLTU     |
| `0x18` | MULT     |        |          |
| `0x19` | MULTU    |        |          |
| `0x1A` | DIV      |        |          |
| `0x1B` | DIVU     |        |          |

### 5.3 REGIMM `rt` Sub-Codes (opcode = `0x01`)

| `rt` value | Mnemonic |
|------------|----------|
| `0x00`     | BLTZ     |
| `0x01`     | BGEZ     |
| `0x10`     | BLTZAL   |
| `0x11`     | BGEZAL   |

### 5.4 OBJECT Funct Codes (opcode = `0x30`)

| Funct  | Mnemonic |
|--------|----------|
| `0x0`  | OMOV     |
| `0x1`  | ONULL    |
| `0x2`  | OEQ      |
| `0x3`  | OISN     |
| `0x4`  | OLEN     |
| `0x5`  | OTAG     |
| `0x6`  | OHOME    |
| `0x7`  | OCAP     |
| `0x8`  | OFENCE   |

`OFENCE` takes no operands and is encoded with all zero `os`/`ot`/`rd`/
`rs`/`rsv` fields. In an architecturally-strict simulator it serves as
a memory ordering barrier between object-register access (`OL*`/`OS*`)
and ordinary mapped-page access (`LW`/`SW` etc.) targeting the same
underlying storage; in this single-threaded simulator there is no
reordering and `OFENCE` retires as a no-op, but its presence makes
ordering-sensitive programs portable across implementations.

### 5.5 R-Type Field Layout

```
 31      26 25  21 20  16 15  11 10   6 5      0
+----------+------+------+------+------+--------+
|  opcode  |  rs  |  rt  |  rd  | shamt|  funct |
+----------+------+------+------+------+--------+
     6        5      5      5      5       6
```

For shift-by-immediate (`SLL`, `SRL`, `SRA`): `rs = 0`, `shamt` =
shift count. For variable shift (`SLLV`, `SRLV`, `SRAV`): `shamt = 0`,
shift count taken from `Rs`. For `JR`: `rt = rd = shamt = 0`. For
`JALR Rd, Rs`: `rt = shamt = 0`. For `MFHI`/`MFLO`: only `rd` is used.

### 5.6 I-Type Field Layout

```
 31      26 25  21 20  16 15                  0
+----------+------+------+----------------------+
|  opcode  |  rs  |  rt  |      immediate       |
+----------+------+------+----------------------+
     6        5      5             16
```

For arithmetic immediates (`ADDI`, `ADDIU`, `SLTI`, `SLTIU`),
immediate is sign-extended. For bitwise immediates (`ANDI`, `ORI`,
`XORI`), immediate is zero-extended. For loads/stores, immediate is
sign-extended and added to `Rs` to form the effective address. For
conditional branches, immediate is the signed word displacement from
the delay-slot instruction; the byte displacement is `imm * 4`. For
`LUI`: `rs = 0`, immediate is loaded into the upper 16 bits of `Rt`
with the lower 16 cleared.

For `BLTZ`, `BGEZ`, `BLTZAL`, `BGEZAL`: opcode is `0x01`, the `rt`
field carries the sub-code from Section 5.3, and `Rs` is the register
being tested.

### 5.7 J-Type Field Layout

```
 31      26 25                                 0
+----------+------------------------------------+
|  opcode  |              target                |
+----------+------------------------------------+
     6                       26
```

`target` is the absolute target word index (target byte address shifted
right by 2) of the *delay-slot instruction*, with the high four bits
of the resulting address taken from the high four bits of the
delay-slot instruction's address. `CALL` reuses this format with
`target` interpreted as the primitive number.

### 5.8 O-Type Register-Register Field Layout (opcode = `0x30`)

```
 31      26 25 22 21 18 17  13 12   8 7   4 3   0
+----------+----+----+------+------+-----+-----+
|  opcode  | os | ot |  rd  |  rs  |funct| rsv |
+----------+----+----+------+------+-----+-----+
     6       4    4     5      5     4     4
```

The first object register named in the mnemonic occupies the `os`
field; the second (if any) occupies the `ot` field. Unused fields are
zero. Specifically:

| Mnemonic              | `os` | `ot` | `rd`  | `rs` |
|-----------------------|------|------|-------|------|
| `OMOV  Od, Os`        | `Od` | `Os` | 0     | 0    |
| `ONULL Od`            | `Od` | 0    | 0     | 0    |
| `OEQ   Rd, Os, Ot`    | `Os` | `Ot` | `Rd`  | 0    |
| `OISN  Rd, Os`        | `Os` | 0    | `Rd`  | 0    |
| `OLEN  Rd, Os`        | `Os` | 0    | `Rd`  | 0    |
| `OTAG  Rd, Os`        | `Os` | 0    | `Rd`  | 0    |
| `OHOME Rd, Os`        | `Os` | 0    | `Rd`  | 0    |
| `OCAP  Rd, Os`        | `Os` | 0    | `Rd`  | 0    |

The `rsv` field is always `0`.

### 5.9 O-Type Load/Store Field Layout (opcodes `0x31`–`0x3B`)

```
 31      26 25 22 21    16 15                  0
+----------+----+--------+----------------------+
|  opcode  | os | rt'    |        offset        |
+----------+----+--------+----------------------+
     6       4      6              16
```

The `rt'` field is six bits, of which the high bit is the reserved
"future register-file widening" bit and must be zero. The remaining
five bits are the GPR number. The `offset` is a signed 16-bit byte
offset from the start of the object.

### 5.9.1 OREFLD/OREFST Layout (opcodes `0x36`, `0x37`)

```
 31      26 25 22 21 18 17 16 15                  0
+----------+----+----+-----+----------------------+
|  opcode  | os | od | rsv |        offset        |
+----------+----+----+-----+----------------------+
     6       4    4    2             16
```

Both `os` and `od` are object register fields (4 bits each). `OREFLD
Od, offset(Os)` reads an 8-byte object reference at byte `offset` of
the object referenced by `Os` and writes it to `Od`. `OREFST Od,
offset(Os)` reads `Od` and writes its bits to the same location.

The target object must have its `OBJSTORE` flag set in the
descriptor (allocated through `ObjAllocStore`); the access traps
`capability-violation` otherwise. Conversely, integer `OL*`/`OS*`
on an `OBJSTORE` object also traps `capability-violation`. The
offset must be 8-byte aligned; misalignment traps
`address-misaligned-d`. Bounds and capability checks are otherwise
identical to `OL*`/`OS*` (Section 5.9): `R` is required for
`OREFLD`, `W` for `OREFST`, plus the live-generation and
in-bounds-by-8-bytes checks.

The `rsv` bits (17:16) must be zero in this revision; non-zero
traps `reserved-instruction`.

### 5.10 SEND Layout (opcode = `0x3C`)

```
 31      26 25 22 21                            0
+----------+----+------------------------------+
|  opcode  | os |          reserved (0)        |
+----------+----+------------------------------+
     6       4              22
```

### 5.11 CALL Layout (opcode = `0x3D`)

```
 31      26 25                                 0
+----------+------------------------------------+
|  opcode  |       primitive number (imm26)     |
+----------+------------------------------------+
     6                       26
```

## 6. Object Reference Layout

The 64-bit object reference (Volume III Section 2.1):

```
 63                 48 47        40 39                  16 15  8 7   0
+--------------------+------------+----------------------+------+----+
|     generation     | home proc  |   local table index  | caps | rsv|
+--------------------+------------+----------------------+------+----+
        16                 8                24              8     8
```

In multi-CPU mode, `home proc` carries the `PROCID` of the CPU on
which the object was allocated; references to remote objects keep
that home value as they cross the crossbar, and the home CPU's
descriptor table is the authoritative source for length, generation,
and capabilities. `caps` are the eight bits R, W, X, S, V, M, C,
reserved (LSB to MSB). `rsv` is always `0`.

The null reference is the all-zeroes 64-bit value.

## 7. Canonical `hello.s`

The assembler agent's `examples/hello.s` should contain exactly:

```asm
; hello.s — Object RISC hello world
;
; The host loader sets up the initial task with:
;   O1 = code object (this program), R+X
;   O2 = stack object, R+W
;   O3 = data object containing "Hello, world!\n", R
;   SP = top of stack region
;   PC = entry point named by .entry directive
;
; The program writes the contents of the data object to the console
; and exits cleanly.

.entry main

.text
main:
    omov  o1, o3            ; ConsoleWrite arg 1: source = data object
    addiu r4, r0, 0         ; offset = 0
    addiu r5, r0, 14        ; count = 14 (length of "Hello, world!\n")
    call  #0x320            ; ConsoleWrite

    addiu r4, r0, 0         ; exit code = 0
    call  #0x001            ; TaskExit
    nop                     ; (unreachable)

.data
hello_str:
    .string "Hello, world!\n"
```

Assembling this against the contract should produce an `.orx` of
exactly:

- Header (32 bytes), with `text_size = 28` (seven 4-byte instructions —
  six for the program plus the trailing `nop` in the source), `data_size
  = 14`, `entry_offset = 0`, `stack_size = 0`.
- Text: seven 32-bit big-endian instruction words.
- Data: the 14 bytes `H e l l o , space w o r l d ! newline`.

Total file size: `32 + 28 + 14 = 74` bytes.

## 8. End-to-End Test

The integration check, performed once both agents have produced their
deliverables:

```sh
tools/asm/asmorisc tools/asm/examples/hello.s -o /tmp/hello.orx
tools/sim/simorisc /tmp/hello.orx
```

Expected: stdout reads `Hello, world!` followed by a newline; exit
code is `0`.
