# The Object RISC Architecture

## Volume VII — Programming Practice

*Architecture Reference, Revision 0.1, 1986*

---

## 1. Scope

This volume describes how the architecture defined in Volumes I through
VI is *programmed in practice*: the standard application binary
interface, the idioms that have emerged from early use of the object
system and the inter-processor communication primitives, a worked
example demonstrating the mechanics end-to-end, and a small body of
notes on compiler output and performance.

It is more discursive than the preceding volumes, in keeping with its
subject. Where the formal specifications are silent on matters of
convention, this volume records the conventions adopted by the
reference toolchain and recommended for portable code. The conventions
here are not part of the architectural contract; they are the
recommendations of a reference compiler against which user programs are
expected to interoperate.

The reader is assumed to be familiar with all six preceding volumes.

## 2. The Standard ABI

### 2.1 Register Usage

The conventions of Volume II Section 3 are summarized below for ease of
reference, with the additional information of what each register's
contents may be assumed to be across a procedure call.

| Register      | Role                          | Across a call             |
|---------------|-------------------------------|---------------------------|
| `R0`          | Constant zero                 | Unchanged                 |
| `R1`          | Assembler temporary           | Undefined                 |
| `R2`–`R3`     | Integer return values         | Set by callee             |
| `R4`–`R7`     | Integer arguments             | Undefined after call      |
| `R8`–`R15`    | Caller-saved temporaries      | Undefined after call      |
| `R16`–`R23`   | Callee-saved                  | Preserved                 |
| `R24`–`R28`   | Caller-saved temporaries      | Undefined after call      |
| `R29` (`SP`)  | Stack pointer                 | Preserved                 |
| `R30` (`FP`)  | Frame pointer                 | Preserved                 |
| `R31` (`RA`)  | Link register                 | Set by `JAL`/`JALR`       |
| `O0`          | Constant null                 | Unchanged                 |
| `O1`–`O4`     | Object arguments / first OR return | Undefined after call |
| `O5`–`O8`     | Caller-saved temporaries      | Undefined after call      |
| `O9`–`O12`    | Callee-preserved              | Preserved (see Section 2.4) |
| `O13`–`O15`   | Caller-saved temporaries      | Undefined after call      |

The frame pointer is optional. Functions whose stack frame is of
constant size and whose alloca-style allocations are absent omit it
and use `SP` as the sole frame anchor; functions with variable-size
frames maintain `FP` and address locals through it.

### 2.2 Stack Frame Layout

The stack grows toward lower addresses. A function's frame, allocated
in its prologue, contains the following regions in descending memory
order:

```
+----------------------------+  ← incoming SP
|   caller's argument spill  |
|   16 bytes                 |
+----------------------------+
|   saved RA                 |  4 bytes
|   saved FP (if used)       |  4 bytes
|   saved callee-preserved   |  up to 32 bytes
|   GPRs (R16–R23)           |
+----------------------------+
|   local variables          |
|   (compiler-managed)       |
+----------------------------+
|   outgoing argument spill  |
|   16 bytes                 |
+----------------------------+  ← new SP
```

The 16-byte spill area at the top of every frame is reserved by the
caller for the callee to spill its incoming integer arguments
(`R4`–`R7`) to memory if it needs their addresses, or if it must spill
them across an inner call. The matching area at the bottom is reserved
by the current function for the next inner call in the same way.

The stack pointer is required to be 8-byte aligned at all call
boundaries. Frame sizes are accordingly rounded up to a multiple of 8.

### 2.3 Procedure Prologue and Epilogue

A canonical prologue for a function with a frame size `F` that uses
the link register and two callee-preserved GPRs `R16` and `R17`:

```
funcname:
    addiu sp, sp, -F           ; allocate frame
    sw    r31, F-4(sp)         ; save link register
    sw    r17, F-8(sp)         ; save callee-preserved
    sw    r16, F-12(sp)
    ; ... body ...
```

The matching epilogue:

```
    lw    r17, F-8(sp)         ; restore callee-preserved
    lw    r16, F-12(sp)
    lw    r31, F-4(sp)         ; restore link register
    jr    r31                  ; return
    addiu sp, sp, F            ; (delay slot) deallocate frame
```

The deallocation of the frame in the branch delay slot is a small but
useful idiom: the return address has already been loaded into a
register and the stack will not be referenced after the jump, so the
deallocation is safe to overlap with the return.

A leaf function that does not call others, has no spills, and uses no
callee-preserved registers may omit the prologue and epilogue
altogether and proceed directly with its body, returning by `jr r31`.

### 2.4 Object Register Discipline

Object registers are *not spillable to byte-typed memory*. The
architecture deliberately prohibits the bit-pattern reconstruction of
an object reference from arbitrary memory contents — to do so would
defeat the capability invariant Volume III establishes — and provides
no instruction by which an object register's bits may be written to
or loaded from a byte-typed storage region with `SB`/`SH`/`SW` or
their object-relative analogues.

References *are*, however, freely spillable to *OR-typed* storage —
the architectural mechanism Volume III Section 5.4 describes:

1. Allocate a backing object once at task setup through
   `ObjAllocStore` (Volume VI Section 5.1.1) with length `8 × N`
   for an N-slot spill area.
2. At the prologue of any function that needs more than the
   conventional working set of object registers, spill to the
   backing object with `OREFST`.
3. At the epilogue, reload with `OREFLD`.

`OREFLD`/`OREFST` are ordinary object-register-relative loads and
stores; the offsets are 16-bit immediates in the encoding, so the
compiler treats them like `LW`/`SW` for register allocation purposes.
Unlike the firmware spill primitives this discussion previously
recommended, no `CALL` is required, and the spill buffer can be
indexed dynamically (the offset is just an immediate, not a fixed
firmware-internal slot index).

The four callee-preserved object registers `O9`–`O12` are preserved
*by convention*: the callee must not modify them. With OR-typed
spill in hand, this is no longer a hard wall — a function that needs
more than four references live across calls allocates an OBJSTORE
spill object on the stack (or in its frame) and uses it.

The firmware primitives `ORegSpill` and `ORegRestore` (Volume VI
Section 5.3, primitive numbers `0x121` and `0x122`) remain available
for legacy code, but new code should prefer `OREFLD`/`OREFST` against
an `ObjAllocStore`'d spill object. The CALL overhead is gone; the
ordering relative to other object-register operations is well-defined
by `OFENCE` rather than by the implicit semantics of a firmware
trap.

### 2.5 Variadic Arguments

Variadic functions place arguments beyond the first four integer slots
in the caller's outgoing argument spill area, ascending from `0(sp)`.
The first four arguments occupy `R4`–`R7` as for non-variadic
functions; arguments numbered five and beyond occupy `0(sp)`,
`4(sp)`, `8(sp)`, … in order.

Object reference arguments to variadic functions are passed in
`O1`–`O4` only; the variadic mechanism does not extend to references,
since they cannot be spilled to integer memory. A variadic function
that requires more than four reference arguments must accept them
through an explicit array-object indirection.

### 2.6 Returning Aggregates

A struct of total size 8 bytes or less is returned in `R2:R3`,
low-word in `R2`. A struct between 9 and 64 bytes is returned through
a caller-supplied buffer whose address is passed as an *implicit
zeroth argument* in `R4`, with the explicit arguments shifted to
`R5`–`R7` and the spill area; the callee writes the struct to the
buffer and the caller reads it from the same address. A struct larger
than 64 bytes is returned by allocating a fresh object whose reference
is returned in `O1`.

A struct that contains object references is always returned through an
object reference, by the rule of Section 2.4.

## 3. Object System Idioms

### 3.1 Allocate-Initialize-Use

The conventional pattern for allocating an object, initializing its
contents, and proceeding to use it:

```
    addiu r4, r0, LEN          ; length in bytes
    addiu r5, r0, TAG          ; type tag
    addiu r6, r0, CAPS         ; initial maximum capabilities
    call  #0x100               ; ObjAlloc
    bne   r2, r0, alloc_fail
    nop                        ; load delay slot for r2

    ; The new object's reference is in O1 with capabilities equal to
    ; the requested initial set. Store an initial value into the first
    ; word of the object:
    addiu r3, r0, INITIAL
    osw   r3, 0(o1)
```

The `caps` value should be the *least* set of capabilities for which
any holder of a reference to this object will ever have a legitimate
need. Capabilities cannot be strengthened later; over-allocation
weakens the object's protection without recovery. The five most
commonly requested combinations are tabulated below.

| Caps (hex) | Bits        | Typical use                                  |
|------------|-------------|----------------------------------------------|
| `0x03`     | `R W`       | Private mutable data (no sharing)            |
| `0x07`     | `R W X`     | Code object with mutable static data         |
| `0x4F`     | `R W S V C` | Service object with revocation and derivation |
| `0x53`     | `R W S V`   | Single-recipient service (no derivation)     |
| `0x09`     | `S X`       | Send-only capability to a fixed handler      |

### 3.2 Capability Passing

A reference held with broad capabilities is rarely the reference one
should pass to another component. The standard idiom is to derive a
weaker reference at the moment of passing:

```
    ; We hold O9 with caps R|W|S|V|C. We wish to grant a callee the
    ; ability to read and send to the object, but not to modify or
    ; revoke it.
    omov  o1, o9               ; reference to derive from
    addiu r4, r0, 0x09         ; mask = R|S
    call  #0x103               ; ObjDerive
    bne   r2, r0, derive_fail
    nop
    omov  o2, o1               ; pass the weakened reference as arg
    jal   downstream_function
    nop
```

The same technique is used to construct a "reply capability" for
asynchronous messaging: the issuer derives a send-only reference
(`caps = 0x08`, `S` only) to a private reply buffer and passes it in
the `SEND` payload; the recipient may use it only to send back, and
specifically may not read or modify the reply buffer's contents.

### 3.3 Releasing References

There is no garbage collector. References that the program has
finished with are dropped by overwriting the holding register, by
stack unwinding, or by `ObjFree`/`ObjRevoke` when the object itself
is to be released.

A reference whose object outlives the holder need not be released
explicitly; overwriting the register suffices. A reference whose
object is to be reclaimed, on the other hand, requires `ObjFree`
(to release the storage and the table slot) or `ObjRevoke` (to
invalidate every outstanding reference without releasing the
storage). The conventional discipline is:

- *Locally allocated, locally consumed* objects: the allocator calls
  `ObjFree` once it has finished with the object. No other holder
  exists; no coordination is required.
- *Allocated for export*: the allocator passes a reference outward
  with appropriate capabilities and retains a reference with the `V`
  bit. When the time comes to revoke the export, `ObjRevoke` is
  called; recipients see `ESTALE` on subsequent dereferences and may
  recover at their discretion.
- *Held under contract by a service object*: the service is
  responsible for calling `ObjFree` on exported objects when its own
  contract ends.

The architecture's reliance on explicit revocation rather than tracing
collection is deliberate. We considered and rejected a hardware-
assisted reference-counting mechanism in early design, on the grounds
that the cost of maintaining accurate counts across the crossbar
would dominate any plausible savings; we expect higher-level languages
on Object RISC to provide tracing collection in their runtimes when
their semantics demand it.

### 3.4 Loadable Modules

Code is data: the architecture imposes no special distinction between
an object that holds instructions and an object that holds bytes. A
program that wishes to load a fragment of code at runtime — a plug-in
handler, a JIT-compiled function, a shared library brought in on
demand — does so by constructing a code object the same way it would
construct any other.

The canonical sequence is five steps:

```
    ; Step 1: allocate a fresh byte-typed object large enough for the
    ; module image. Initial caps include W so we can write into it.
    addiu r4, r0, MODULE_SIZE   ; length in bytes
    addiu r5, r0, 0x4100        ; type tag = LoadableModule
    addiu r6, r0, 0x07          ; caps = R|W|X (writable while loading)
    call  #0x100                ; ObjAlloc
    bne   r2, r0, fail
    nop
    omov  o9, o1                ; preserve the module object

    ; Step 2: copy the image into the object. The image is read from
    ; wherever it lives — a SEND payload, a remote service, a region
    ; of the boot text — and stored word by word with OSW.
    ; (Loop body omitted.)

    ; Step 3: derive an immutable executable reference. The W bit is
    ; dropped; the resulting reference is sealed and may be shared.
    omov  o1, o9
    addiu r4, r0, 0x05          ; mask = R|X (no W)
    call  #0x103                ; ObjDerive
    bne   r2, r0, fail
    nop
    omov  o10, o1               ; preserve the sealed code reference

    ; Step 4: map the sealed object into the local processor's virtual
    ; address space so that branch targets within the module resolve.
    ; The mapping inherits the reference's caps (R|X), so the mapped
    ; pages are unwritable.
    omov  o1, o10
    addiu r4, r0, MODULE_VA     ; virtual address
    addiu r5, r0, 0x05          ; protection = R|X
    call  #0x110                ; MapObject

    ; Step 5: install the module's entry point as a SEND handler on
    ; some target object. The "code object" argument to InstallHandler
    ; is the sealed reference we just mapped.
    omov  o1, o_target          ; the object whose handler this serves
    omov  o2, o10               ; sealed code object
    addiu r4, r0, ENTRY_OFFSET  ; offset of the handler's first inst
    call  #0x200                ; InstallHandler
```

A few points are worth noting:

- The deliberate dropping of `W` between Steps 2 and 3 is what
  separates a loadable module from a writable buffer. After Step 3,
  the only references to the object's bytes-as-instructions carry
  `R|X` and cannot be modified; this is what allows the descriptor
  cache and the instruction cache (Volume V) to assume the module's
  contents are stable for as long as the sealed reference exists.
- `InstallHandler` checks that the supplied code object is currently
  mapped executable on the receiving processor. This is what permits
  the same primitive that handles the boot text to handle loadable
  modules — there is no special "loadable" handler-installation path.
- A module that is to be unloaded is unloaded by `ObjFree` on the
  *original* writable reference (which, having `V`, can revoke the
  sealed derivative). After revocation, the next dispatch attempting
  to enter the module traps `ESTALE` rather than executing freed
  bytes.
- Modules may carry their own state by allocating `OBJSTORE` objects
  (Volume III Section 5.4) at load time and storing the references in
  a table accessed through the handler's *self* reference (Section
  4.1). This is the architectural answer to "static data segment" for
  dynamically loaded code.

We expect this pattern to be the foundation for plug-in device
drivers, for language runtimes that emit code at runtime, and for any
operating-system facility that benefits from extending its dispatch
surface without rebuilding its boot image.

### 3.5 Bulk-Byte Transfer Between Objects

The integer load and store instructions through an object register
(`OL{B,H,W}` / `OS{B,H,W}`) take an immediate offset and transfer one
to four bytes per instruction. When both objects are local, a copy
loop is the right shape. When one side is remote, the cost changes:
each dereference becomes an `OBJ_READ_REQ` / `OBJ_WRITE_REQ` round-trip
across the crossbar, and a 128-byte copy expressed as `OSB` in a loop
incurs 128 round-trips.

The firmware therefore exposes two primitives that collapse a
contiguous range to one wire round-trip when one side is remote:

- `ObjFetchBytes` (Volume VI, primitive `0x108`) — copy `R6` bytes
  from `O1+R4` to `O2+R5`. The source may be remote; the destination
  must be local.
- `ObjStoreBytes` (Volume VI, primitive `0x109`) — copy `R6` bytes
  from `O1+R4` to `O2+R5`. The source must be local; the destination
  may be remote.

The pair covers transfers in either direction between two objects
when at most one is remote. Neither addresses remote-to-remote
transfers; the calling task arranges a local staging object if it
needs to bridge two remote sides.

```
    ; Pull an 80-byte record from a remote service object (held in
    ; O5 with R) into a local stack-resident scratch buffer (held in
    ; O6 with R|W).
    omov  o1, o5               ; source (remote)
    omov  o2, o6               ; destination (local)
    addiu r4, r0, 0            ; source offset
    addiu r5, r0, 0            ; destination offset
    addiu r6, r0, 80           ; byte count
    call  #0x108               ; ObjFetchBytes
    bne   r2, r0, fetch_fail
    nop                        ; r3 = bytes copied (= 80 on success)
```

The two primitives are non-restartable: the calling task blocks
until the matching `OBJ_READ_RESP` / `OBJ_WRITE_RESP` returns. The
descriptor and capability checks are identical to those imposed by
the per-instruction dereference paths; the only architectural
relaxation is on the destination of `ObjFetchBytes`, which must not
carry the `OBJSTORE` flag (Volume III Section 5.4) — the bulk path
copies bytes-as-bytes and would breach the OR-typed-storage
invariant if applied to a slot intended for object references.

## 4. Inter-Processor Communication Idioms

### 4.1 The Self Convention for Handlers

When firmware on a receiving processor dispatches a `SEND` handler,
it constructs the entry context as follows:

- `O1` is set to a fresh reference to the recipient object itself,
  carrying the object's full `max_caps`. This is the *self* reference,
  by which the handler is expected to access its own state.
- `O2`–`O4` are set to the first three object references from the
  `SEND` payload as transmitted on the wire.
- `R4`–`R7` are set to the four integer payload words.

A SEND payload may carry up to four object references on the wire, as
Volume IV specifies; the convention here delivers only the first three
to the handler in object registers. A handler that requires the fourth
invokes the firmware primitive `MessagePayloadOR4` (Volume VI Section
6, primitive number `0x205`), which retrieves the reference from the
side-channel buffer where dispatch deposited it. We expect this case
to be rare, and we expect a future revision to reorganize the
dispatch convention to remove the asymmetry.

### 4.2 Asynchronous Notification

The simplest use of `SEND` is fire-and-forget: an event has occurred,
the recipient should be told, no reply is expected.

```
    ; We hold a reference to the notification target in O9.
    omov  o1, o9               ; recipient
    onull o2                   ; no payload references
    onull o3
    onull o4
    addiu r4, r0, EVENT_CODE   ; event identifier
    addiu r5, r0, EVENT_DATA   ; event detail
    send  o1                   ; fire
```

The handler at the recipient runs asynchronously, possibly on a
different processor; the issuing task continues immediately. There is
no acknowledgment; the handler may not have run by the time the
issuing task observes any subsequent state.

### 4.3 Synchronous Reply

The pattern that most resembles a remote procedure call layers a
reply object on top of `SEND`. The architecture provides no native
"call-with-reply" primitive; the pattern below is the canonical
construction and is what the reference toolchain emits for
synchronous cross-processor calls.

1. The caller allocates a small object to hold the reply, with
   capabilities `R|W|S|V`.
2. The caller attaches a receive queue to the reply object via
   `ReceiveQueueAttach` (Volume VI Section 6, primitive number
   `0x203`). The queue replaces handler dispatch on the reply object:
   incoming `SEND`s to the reply are buffered for the caller to
   collect rather than spawning a fresh task.
3. The caller derives a send-only capability (`S` alone) on the reply
   object, suitable for passing to the callee. The callee receives
   only the right to send back; it cannot read, modify, or revoke the
   reply object.
4. The caller issues a `SEND` to the service, including the send-only
   reply capability in the payload as one of the reference slots.
5. The caller polls the reply queue via `ReceiveQueuePoll` (primitive
   number `0x204`) with an appropriate timeout. The poll blocks the
   calling task until a message arrives or the timeout expires.
6. On reply, the caller reads the response payload from the registers
   loaded by `ReceiveQueuePoll`, frees the reply object with
   `ObjFree`, and returns.

This pattern is preferable to installing a handler on the reply
object: handler dispatch spawns a fresh task per delivery, which
introduces both a context-switch cost and a synchronization burden on
the caller (the spawned reply handler needs to communicate with the
caller through some shared state). The receive-queue approach keeps
the reply on the caller's task and incurs only the unblock cost.

The full sequence is given in the worked example of Section 5.

## 5. A Worked Example: A Distributed Counter Service

### 5.1 The Service

A *Counter* is an object that holds a single 32-bit unsigned counter
value and exposes three operations through `SEND`:

| Opcode | Operation     | Payload                | Reply payload          |
|--------|---------------|------------------------|------------------------|
| `1`    | `READ`        | (none)                 | current count in `R4`  |
| `2`    | `INCREMENT`   | (none)                 | new count in `R4`      |
| `3`    | `RESET`       | new value in `R5`      | old count in `R4`      |

Counters are allocated with type tag `0x4001` and the capability set
`R|W|S|V|C` (`0x4B`). Clients receive references with `R|S` caps
(`0x09`) — read for verification of locality, send for invoking
operations.

The single shared handler routes on opcode and replies through the
caller-supplied reply capability, which appears as `O2` of the
handler's payload by the `SEND` convention of Section 4.1.

### 5.2 The Server

The server consists of an initialization routine, called once on the
counter's home processor, and a handler, invoked once per incoming
`SEND`.

#### 5.2.1 Initialization

```
; counter_create(handler_code in O1) -> O1 = counter_ref, R2 = status
;
; Allocates a Counter object and installs the shared handler on it.
; The handler code object reference is supplied by the caller in O1.

counter_create:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    omov  o9, o1                ; preserve handler code object

    ; Allocate the counter object: 4 bytes, type 0x4001, caps R|W|S|V|C
    addiu r4, r0, 4             ; length
    addiu r5, r0, 0x4001        ; type tag = Counter
    addiu r6, r0, 0x4B          ; caps = R|W|S|V|C
    call  #0x100                ; ObjAlloc
    bne   r2, r0, fail
    nop

    omov  o10, o1               ; preserve counter ref

    ; Initialize count to zero
    osw   r0, 0(o10)

    ; Install the handler: target = counter, handler code = O9, offset = 0
    omov  o1, o10               ; target
    omov  o2, o9                ; handler code object
    addu  r4, r0, r0            ; offset = 0 (entry of counter_handler)
    call  #0x200                ; InstallHandler
    bne   r2, r0, fail
    nop

    omov  o1, o10               ; return counter ref
    addu  r2, r0, r0            ; status = OK
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16            ; (delay slot) deallocate frame

fail:
    onull o1
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16
```

#### 5.2.2 The Handler

```
; counter_handler:
;   Invoked by firmware when SEND arrives at a Counter object.
;   On entry:
;     O1 = self (counter ref, full caps)
;     O2 = reply object (send-only cap), or null if caller wants no reply
;     R4 = opcode (1 = READ, 2 = INCREMENT, 3 = RESET)
;     R5 = data (RESET only)
;   Runs as a freshly-spawned user-mode task. Returns by TaskExit.

counter_handler:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    sw    r16, 8(sp)

    ; Branch on opcode
    addiu r16, r0, 1
    beq   r4, r16, do_read
    addiu r16, r0, 2            ; (delay slot) prepare next compare
    beq   r4, r16, do_increment
    addiu r16, r0, 3            ; (delay slot)
    beq   r4, r16, do_reset
    nop                         ; (delay slot)

    ; Unrecognized opcode: terminate without replying
    j     terminate
    addiu r4, r0, 1             ; (delay slot) exit code 1 (error)

do_read:
    olw   r4, 0(o1)             ; load current count into reply payload
    j     send_reply
    nop                         ; (load delay slot doubled as branch DS)

do_increment:
    olw   r16, 0(o1)            ; load current count
    nop                         ; load delay slot
    addiu r4, r16, 1            ; new count
    j     send_reply
    osw   r4, 0(o1)             ; (delay slot) store new count

do_reset:
    olw   r4, 0(o1)             ; load old count for the reply
    nop                         ; load delay slot
    j     send_reply
    osw   r5, 0(o1)             ; (delay slot) store new value

send_reply:
    ; If no reply object was supplied, skip the SEND.
    oisn  r16, o2
    bne   r16, r0, no_reply
    nop

    ; Issue reply: recipient = O2, payload R4 carries result.
    omov  o1, o2
    onull o2                    ; clear unused payload references
    onull o3
    onull o4
    send  o1                    ; fire reply

no_reply:
    addu  r4, r0, r0            ; exit code 0 (OK)

terminate:
    lw    r16, 8(sp)
    lw    r31, 12(sp)
    addiu sp, sp, 16
    call  #0x001                ; TaskExit (does not return)
    nop
```

### 5.3 The Client

```
; counter_get(counter in O1) -> R2 = count, R3 = status (0 on success)
;
; Issues a synchronous READ to the counter and returns the current value.

counter_get:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    omov  o9, o1                ; preserve counter ref

    ; Step 1: allocate a 4-byte reply object with R|W|S|V caps
    addiu r4, r0, 4
    addiu r5, r0, 0x4002        ; type tag = Reply
    addiu r6, r0, 0x1B          ; caps = R|W|S|V (no derivation)
    call  #0x100                ; ObjAlloc
    bne   r2, r0, fail
    nop
    omov  o10, o1               ; preserve reply ref

    ; Step 2: attach a receive queue of depth one to the reply object
    omov  o1, o10
    addiu r4, r0, 1             ; depth = 1
    call  #0x203                ; ReceiveQueueAttach
    bne   r2, r0, fail_free
    nop

    ; Step 3: derive a send-only capability on the reply for the server
    omov  o1, o10
    addiu r4, r0, 0x08          ; mask = S only
    call  #0x103                ; ObjDerive
    bne   r2, r0, fail_free
    nop
    omov  o11, o1               ; preserve send-only reply cap

    ; Step 4: SEND the READ request.
    omov  o1, o9                ; recipient = counter
    omov  o2, o11               ; payload OR1 = reply send-cap
    onull o3
    onull o4
    addiu r4, r0, 1             ; opcode = READ
    send  o1

    ; Step 5: poll the reply queue, unbounded wait.
    omov  o1, o10               ; reply object (with V cap)
    addiu r4, r0, -1            ; timeout = 0xFFFFFFFF (unbounded)
    call  #0x204                ; ReceiveQueuePoll
    bne   r2, r0, fail_free
    nop

    ; ReceiveQueuePoll delivers integer payload in R3-R6 (per Volume VI
    ; Section 6 convention). Our reply put the count in R4 of the SEND
    ; payload, which arrives as R3 here.
    addu  r2, r3, r0            ; return value = count

    ; Step 6: free the reply object and return.
    omov  o1, o10
    call  #0x101                ; ObjFree
    addu  r3, r0, r0            ; status = OK
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16

fail_free:
    omov  o1, o10
    call  #0x101                ; best-effort free
    nop
fail:
    addu  r2, r0, r0            ; return value = 0
    addiu r3, r0, 1             ; status = generic error
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16
```

### 5.4 Trace of a Single READ Operation

Suppose the counter object lives on processor 3 and the calling task
runs on processor 7. A trace of `counter_get` proceeds as follows:

1. *On processor 7.* The client allocates the reply object via
   `ObjAlloc`. Firmware on processor 7 selects a free slot in its
   local object table, allocates four bytes of local memory, and
   constructs a reference whose home is `7`.
2. *On processor 7.* `ReceiveQueueAttach` installs a per-object queue
   in firmware-managed memory and configures the local handler-
   dispatch to enqueue rather than spawn a task on receipt.
3. *On processor 7.* `ObjDerive` constructs a new reference to the
   reply object with capabilities reduced to `S` alone. The reference
   has the same generation, home (`7`), and index as the original.
4. *On processor 7.* The `SEND` instruction dispatches its payload to
   the crossbar interface. The payload's recipient is the counter
   reference, whose home field is `3`; the crossbar routes the
   `SEND_DELIVER` packet to port 3.
5. *On processor 3.* The crossbar interface receives the
   `SEND_DELIVER` packet and hands it to firmware. Firmware reads the
   recipient reference, looks up the descriptor in the local object
   table, finds the installed handler, spawns a task at the handler's
   entry point with `O1` set to a full-caps self reference and `O2`
   set to the reply send-cap from the payload, and sets `R4` to `1`
   (the opcode).
6. *On processor 3.* `counter_handler` runs. The `do_read` branch is
   taken; `OL W` fetches the count word, the descriptor cache hit is
   free, and the value is placed in `R4` for the reply.
7. *On processor 3.* The handler executes `SEND O1` to fire the reply.
   The recipient is the reply object on processor 7. The crossbar
   routes the packet back to port 7.
8. *On processor 7.* Firmware delivers the reply to the queue
   previously attached. `counter_get` is sleeping in
   `ReceiveQueuePoll`; firmware unblocks it.
9. *On processor 7.* `counter_get` reads the count from `R3`, frees
   the reply object via `ObjFree` (which increments the reply object's
   generation and returns the slot to the local pool), and returns to
   its caller.

Total architectural latency on the reference 16-port crossbar at
16 MHz is approximately 6 microseconds for the round trip, dominated
by the two crossbar traversals at roughly 1.75 microseconds each plus
the handler invocation at 1.4 microseconds.

## 6. Compiler Notes

### 6.1 Filling Delay Slots

The reference toolchain fills branch delay slots and load delay slots
through a peephole pass that runs after instruction selection. The
compiler considers, in order:

1. An instruction from the basic block following the branch that does
   not depend on the branch's outcome.
2. An instruction from the basic block targeted by the branch, when
   the branch is predicted taken and the duplicated instruction has
   no observable side effect on the not-taken path.
3. A `NOP`, when neither of the above applies.

The observed fill rates on the firmware reference workload are 60% for
branch delay slots and 70% for load delay slots. We expect both
numbers to improve as the toolchain matures.

### 6.2 Object Register Allocation

Object register allocation is performed separately from general
register allocation, with its own live-range analysis. Because object
registers cannot be spilled to memory, the allocator falls back, when
register pressure exceeds the available class, to one of the
strategies of Section 2.4: restructuring is not within its purview, so
in practice it generates either an array-marshalling sequence (the
preferred strategy) or, for compilations that link against a
spill-supporting firmware, a sequence of `ORegSpill`/`ORegRestore`
calls.

In our measurements on representative C code from the firmware
reference workload, the array-marshalling fallback is engaged in
fewer than 1% of compiled functions; the spill-call fallback is
engaged in fewer than 0.1%.

### 6.3 Generating CALL Sequences

A `CALL` to a firmware primitive is emitted by the compiler as a
single instruction; the calling sequence is otherwise standard. The
compiler is permitted to assume that all caller-saved registers are
preserved across `CALL`, by the convention of Volume VI Section 2.2.
This permits register allocation across `CALL` boundaries that would
not be permitted across an ordinary `JAL`, and is the principal
performance advantage of `CALL` relative to a software-implemented
trap.

The compiler emits a `CALL` to a primitive number that the linker
resolves at link time against the firmware's exported primitive
table. The architecture's reservation of the primitive number into
the instruction itself (rather than into a register) is what makes
this resolution possible without runtime indirection.

## 7. Performance Considerations

### 7.1 Local versus Remote Object Access

A local object access through `OL`/`OS` against a descriptor-cache
hit costs the same as an ordinary load or store: one cycle in the
common case. A remote access against a descriptor-cache hit is two
crossbar traversals plus the round-trip through the home processor's
memory hierarchy — approximately 28 cycles in the reference 16-port
configuration at 16 MHz. The ratio of about 14 to one is the principal
performance consideration when deciding whether to migrate an object
or to map a copy of it locally.

A descriptor-cache miss adds approximately a further 30 cycles for
the descriptor fetch round trip; subsequent accesses against the same
descriptor are then at the cached cost. Programs that touch many
distinct remote objects in close succession are accordingly far more
expensive than programs that touch the same handful of remote objects
repeatedly.

### 7.2 Descriptor Cache Locality

The descriptor cache is shared across all tasks resident on a
processor. A task that begins execution on a processor where another
task has recently used a related object inherits the warm cache; a
task that migrates to a cold processor pays a sequence of misses
before its working set is restored. For latency-sensitive task
groups, the firmware primitive `TaskBindProcessor` (Volume VI Section
4) is the standard tool for asserting locality.

### 7.3 SEND Buffering and Back-Pressure

A `SEND` whose destination port is congested stalls until the
crossbar accepts the packet. For applications that can tolerate the
delay, this is harmless; for applications that require throughput
under burst, the firmware primitive `SendBuffered` (Volume VI Section
6) absorbs the stall into a software queue, returning `EAGAIN` only
when the queue itself is full.

The `SendBuffered` queue lives in the issuing task's address space
and is sized at firmware allocation time. The reference firmware
defaults to a queue of sixteen messages per task; tasks that require
larger queues request them at task creation through an extension
parameter not described in this volume.

— *The Object RISC Architecture Group, 1986*
