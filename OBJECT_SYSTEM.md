# The Object RISC Architecture

## Volume III — Object System

*Architecture Reference, Revision 0.1, 1986*

---

## 1. Scope

This volume specifies the data structures and invariants of the Object
RISC object system: the layout of an object reference, the format of the
object descriptor, the per-processor object table, the descriptor cache
on each processor, the meaning of the capability bits, the lifecycle of
an object from allocation through revocation, and the firmware-mediated
mechanism by which objects migrate between processors.

The instructions that operate on this machinery — `OL*`, `OS*`, `OMOV`,
`OEQ`, `OISN`, `OLEN`, `OTAG`, `OHOME`, `OCAP`, `SEND`, and `CALL` —
are defined in Volume II. The wire-level messages by which descriptor
fetches, invalidations, and forwardings travel between processors are
defined in Volume IV. The firmware primitives invoked through `CALL` to
allocate, revoke, derive, migrate, and map objects are catalogued in
Volume VI; this volume names them but does not specify their detailed
parameter conventions.

The reader is assumed to be familiar with Volumes I and II.

## 2. Object References

### 2.1 Reference Format

An object reference is the 64-bit value held in an object register. It
is structured as four contiguous fields:

```
 63                 48 47        40 39                  16 15  8 7   0
+--------------------+------------+----------------------+------+----+
|     generation     | home proc  |   local table index  | caps | rsv|
+--------------------+------------+----------------------+------+----+
        16                 8                24              8     8
```

- **Generation** (16 bits, unsigned). The value of the generation
  counter at the home processor's object table at the moment this
  reference was minted. It increments each time the underlying table
  slot is freed; on access, the reference's generation must match the
  generation currently recorded in the descriptor, or the reference is
  *stale*.
- **Home processor** (8 bits, unsigned). The identifier of the
  processor whose object table contains the descriptor for this
  object. Up to 256 processors are addressable in a single Object RISC
  system; larger systems are reached through the routing extensions of
  Volume IV.
- **Local table index** (24 bits, unsigned). The position within the
  home processor's object table at which the descriptor resides. Up to
  16,777,216 objects may be resident on any one processor, an
  allocation we judge generous against the physical-memory sizes of
  the late 1980s.
- **Effective capability bits** (8 bits). The capability rights this
  particular reference carries. The bits are checked by hardware on
  every dereference without recourse to the descriptor (Section 5).
  They may be no stronger than the *maximum* capability bits recorded
  in the descriptor, an invariant that firmware maintains by
  construction at every primitive that produces a reference.
- **Reserved** (8 bits). Must be zero in this revision. Future
  revisions may use these bits to encode access hints or, in a wider
  reference format, to extend the local table index.

A reference is *opaque* in the sense that user-mode and supervisor-mode
code cannot construct one whose generation, home processor, or table
index is selected freely; the only operations available to non-firmware
code are to read these fields through `OHOME` and `OTAG` (and other
inspection ops in Volume II), to copy a reference from one object
register to another through `OMOV`, and to weaken its capability bits
through the firmware primitive `ObjDerive`. The capability bits, the
home processor, and the index are visible to any holder of the
reference; nothing about them is secret. The integrity of the system
rests not on the obscurity of references but on the impossibility of
manufacturing one with rights that exceed those of an existing
reference.

### 2.2 The Null Reference

The constant value zero in all fields is the *null reference*,
hardwired into object register `O0`. Its generation, home, and index
fields are by construction invalid, and no live object ever has them.
The null reference dereferences to the synchronous trap
`null-dereference`; it carries no capability bits and cannot be
strengthened.

`OEQ` and `OISN` test the null reference without trapping.

### 2.3 Reference Equality

Two references denote the same object only if they share the same
generation, home processor, and local table index. The capability and
reserved fields are *not* significant for equality; references derived
from a common ancestor through `ObjDerive` with different capability
masks compare equal under `OEQ`.

A pair of references whose triples agree but whose generation differs
do *not* denote the same object. They denote different generations of
the same table slot, which by construction are different objects; this
is the case `OEQ` returns zero, even though it may appear at first
glance that two "almost equal" references should compare equal. In
practice this case is rare; the generation counter exists precisely to
make it always discoverable.

## 3. The Object Table

### 3.1 Per-Processor Layout

Each processor maintains an object table holding the descriptors of
every object whose home it is. The table is a contiguous array of
fixed-size descriptors, allocated by firmware in the local physical
memory at boot, and pinned for the duration of the system's
operation.

The table's base physical address is held in the firmware-only control
register `OBJTAB_BASE`; its size, in descriptor entries, is held in
`OBJTAB_LIMIT`. A reference whose local index exceeds `OBJTAB_LIMIT`
on its home processor is trivially invalid and raises
`stale-reference` on dereference.

Allocation of fresh entries within the table is a firmware concern;
the architecture neither specifies nor restricts the allocation
discipline. The reference implementation maintains a free list
threaded through the unused descriptors of the local table.

### 3.2 Descriptor Format

Each descriptor occupies thirty-two bytes, naturally aligned, and is
structured as follows:

```
Offset  Size  Field
+0      4     base — physical base address (on home processor)
+4      4     length — size of the object in bytes
+8      2     generation — current generation counter for this slot
+10     2     type_tag — opaque software-defined type identifier
+12     1     max_caps — maximum capability bits ever issuable
+13     1     flags — descriptor state (see Section 3.3)
+14     2     reserved — must be zero
+16     8     send_handler_ref — object reference of the handler code
+24     4     send_handler_off — byte offset within the handler object
+28     4     reserved — must be zero
```

The fields are interpreted as follows.

- **base** is the physical byte address, on the home processor, at
  which the object's storage begins. The storage region is contiguous
  and lies entirely within the home processor's local memory.
- **length** is the size of the storage region in bytes. The legal
  byte offsets into the object are `[0, length)`.
- **generation** is incremented each time the slot is freed. A
  reference is live if and only if its generation field equals this
  value.
- **type_tag** is an opaque 16-bit value supplied by the allocator at
  `ObjAlloc` time. The hardware does not interpret it. Software
  conventions assign meaning to ranges of values.
- **max_caps** is the upper bound on the capability bits any reference
  to this object may carry. `ObjDerive` may produce references with
  any subset of `max_caps`; no primitive produces a reference with a
  bit set in its caps field that is not also set in `max_caps`.
- **flags** carries descriptor state, encoded as Section 3.3 details.
- **send_handler_ref** and **send_handler_off** together name the code
  invoked when a `SEND` message arrives addressed to this object.

The two reserved fields are written as zero by firmware on
allocation; future revisions may consume them for additional state
without changing the descriptor's size.

### 3.3 Descriptor Flags

The `flags` byte encodes the descriptor's lifecycle state:

| Bit | Name              | Meaning                                       |
|-----|-------------------|-----------------------------------------------|
| 0   | `LIVE`            | Slot holds a valid object                     |
| 1   | `MIGRATING`       | Migration is in progress (Section 6.4)        |
| 2   | `FORWARDED`       | This slot is a stub forwarding to elsewhere   |
| 3   | `PINNED`          | Storage may not be moved or paged             |
| 4   | `DEVICE`          | Object overlays a device register window      |
| 5   | `EXECUTABLE`      | Storage holds executable code                 |
| 6–7 | reserved          | Must be zero                                  |

A `FORWARDED` descriptor reinterprets its `base` and `length` fields
as the new home processor and new local index, respectively, of the
object's true location; see Section 6.4.

## 4. The Object Descriptor Cache

Every dereference of an object register requires the descriptor of
the named object: at minimum, the generation (for liveness), the
length (for bounds), and the base address (to locate the storage).
Fetching the descriptor from the home processor's object table on
every access would impose a long-latency dependency on every object-
register load and store, defeating the design goal of zero-cycle
overhead in the common case.

The architecture therefore requires every implementation to maintain
an *object descriptor cache* (ODC), a small associative structure
local to the processor that holds recently-used descriptors. The
contents of the ODC are coherent with the object table by the
mechanism of Section 4.2; programs that observe the ODC's behaviour
only through `OL*`, `OS*`, and the inspection operations of Section 8
of Volume II cannot distinguish it from a hypothetical implementation
that performed a fresh descriptor fetch on every access.

### 4.1 Tag, Lookup, and Hit Path

An ODC entry is keyed by the tuple `(home, index)` taken from the
reference's home and table-index fields. On the issuing of an object-
register access, the ODC is consulted in the same cycle as the
effective-address computation. A hit returns the entry's cached
descriptor; the bounds check and the dispatch of the access (local
versus remote) proceed without further dependency. A miss stalls the
issuing instruction and dispatches a `DESC_REQ` message to the home
processor, which responds with a `DESC_RESP` message carrying the
descriptor; the entry is installed and the access retried.

The reference implementation provides a thirty-two-entry, four-way
set-associative ODC; conforming implementations may vary the size and
associativity, but are required to provide an ODC of at least eight
entries.

### 4.2 Generation as Coherence Mechanism

The ODC is kept consistent with the object table by the generation
counter rather than by explicit invalidation traffic. On every hit,
the hardware compares the generation field of the *reference being
dereferenced* against the generation field of the *cached
descriptor*. If they disagree, the entry is treated as a miss, evicted,
and refetched. Code that holds a stale reference therefore sees its
access fail at the descriptor cache itself; code that has obtained a
fresh reference after the slot was reused sees the new descriptor on
its first access.

This mechanism removes the need for cross-processor invalidation
broadcasts on every `ObjFree`, the cost of which would dominate any
busy system. It does, however, mean that the descriptor's
*non-generation* fields — most importantly the maximum capability
bits, the type tag, and the send handler — must not be modified
without also incrementing the generation. Consequently, those fields
are immutable in this revision: changing them requires freeing the
object and allocating a new one. The send handler is the sole
exception; it is described in Section 7.

The generation field of the descriptor wraps after 65,536 increments.
A reference whose generation has been overrun is technically
indistinguishable from a reference to the current incarnation; the
firmware specifications of Volume VI are required to recycle table
slots in a manner that makes wrap-around in practice unobservable to
correctly written software (typically by keeping a slot retired for
many generations after `ObjFree`, or by allocating from the slot
least-recently freed). The architecture imposes no specific policy.

## 5. Capability Bits

### 5.1 The Capability Set

The eight capability bits are assigned as follows:

| Bit | Symbol  | Right                                                  |
|-----|---------|--------------------------------------------------------|
| 0   | `R`     | Read storage through `OL*`                             |
| 1   | `W`     | Write storage through `OS*`                            |
| 2   | `X`     | Treat storage as executable for an indirect jump       |
| 3   | `S`     | Issue `SEND` to this object                            |
| 4   | `V`     | Revoke this object through `ObjRevoke` or `ObjFree`    |
| 5   | `M`     | Request migration of this object                       |
| 6   | `C`     | Derive further references from this one                |
| 7   | reserved | Must be zero in this revision                         |

The hardware enforces `R`, `W`, and `S` at access; `X` is enforced by
the page-permission machinery of Volume V at the point an executable
mapping is established; `V`, `M`, and `C` are enforced by firmware at
the relevant primitive call.

A reference with no capability bits set is *inert*: it can be moved,
compared, and inspected, but no useful operation can be performed
through it. Inert references are the canonical handle to an object
held purely for naming purposes — for example, an entry in a
software-managed table of known objects.

### 5.2 Derivation

The firmware primitive `ObjDerive(ref, mask) → ref'` produces a new
reference to the same object with effective capabilities equal to
`ref.caps ∧ mask`. Derivation requires that the calling reference's
`C` bit is set; firmware refuses the primitive otherwise. The derived
reference inherits the same generation, home, and index, and is
indistinguishable from the original under `OEQ`.

There is no operation that strengthens capabilities. The architecture
treats `ObjDerive` as the only producer of reference values from
existing references, and no other primitive returns a reference whose
capabilities are not bounded above by those of the inputs from which
it was derived.

### 5.3 Revocation

Revocation invalidates every outstanding reference to an object in a
single firmware-mediated step. The firmware primitive `ObjRevoke(ref)`
requires the `V` bit on the calling reference and increments the
generation counter of the descriptor without freeing the underlying
storage; subsequent dereferences of any outstanding reference to the
revoked object see a generation mismatch and fail at `stale-reference`.

Revocation is the architecture's primitive answer to the question of
how rights granted to one program may be withdrawn by another. The
holder of a reference with `V` may revoke; the revocation is global,
because no separate state per holder is maintained. This coarseness is
intentional: tracking individual outstanding references for fine-grained
revocation would impose either a system-wide reverse-index of references
(prohibitively expensive) or a software discipline that the architecture
chooses not to mandate.

`ObjFree(ref)` is `ObjRevoke` followed by the release of the storage
back to the home processor's free pool. It also requires the `V` bit.

## 6. Object Lifecycle

### 6.1 Allocation

`ObjAlloc(length, type_tag, init_caps) → ref` creates a new object on
the calling processor (the home is fixed by the call site; objects
may not be allocated remotely). Firmware:

1. Selects an unused slot from the local object table's free list.
2. Allocates a contiguous physical region of `length` bytes from the
   local memory pool.
3. Zeroes the storage.
4. Constructs a descriptor with the chosen base, the requested length
   and type tag, `max_caps` set to `init_caps`, the generation taken
   from the slot's current value, and `flags` set to `LIVE`.
5. Constructs a reference whose generation, home, and index match the
   descriptor, and whose effective capabilities equal `init_caps`.

The returned reference has `max_caps` worth of rights; subsequent
holders may receive references derived to weaker rights but never
stronger.

If the local memory pool cannot satisfy the request, firmware returns
a null reference and an error code per Volume VI's call convention.
The architecture provides no hardware-level mechanism for cross-
processor allocation: a task that wishes to create an object on a
different processor must `SEND` a request to a service object on
that processor, whose handler issues the local `ObjAlloc` and
returns the new reference.

### 6.2 Deallocation

`ObjFree(ref)` increments the descriptor's generation, returns the
storage to the local free pool, and threads the table slot back into
the free list. Outstanding references become stale immediately; their
next dereference fails at the descriptor cache, regardless of which
processor holds them.

Storage returned to the free pool may be reused for any subsequent
allocation, including allocations of objects with different lengths
and capabilities. The generation increment is the sole guarantor that
old references do not accidentally name the new object.

### 6.3 Derivation

See Section 5.2.

### 6.4 Migration

Migration moves the storage of an object from one home processor to
another without invalidating any outstanding reference. The firmware
primitive `ObjMigrate(ref, new_home)` requires the `M` bit on the
calling reference. The protocol is:

1. The source firmware sets the `MIGRATING` flag on the descriptor at
   the original home.
2. The source firmware copies the storage to the destination
   processor, and the destination firmware allocates a fresh slot in
   its local table holding a descriptor with the same generation, the
   same `max_caps`, the same `type_tag`, the new physical base, the
   same length, and `flags` set to `LIVE`.
3. The source firmware replaces the original descriptor in place with
   a *forwarded* descriptor: `flags` is set to `FORWARDED`, and the
   `base` and `length` fields are reinterpreted as the new home and
   new local index, respectively.
4. The source firmware retains the slot in its forwarded state. It is
   not returned to the free list.

Subsequent dereferences of references that still name the original
home arrive at the forwarded descriptor. The home processor's
firmware responds with a `DESC_FORWARD` crossbar message that carries
both the redirection and the new descriptor; the issuing processor
installs the new descriptor in its ODC, *under the original
`(home, index)` tag*, and the original reference therefore continues
to function but with all subsequent accesses dispatched directly to
the new home. The forwarded slot at the source remains until firmware
chooses to garbage-collect it, by which time the architecture is
silent.

Migration does not increment the generation. A reference that was
live before migration remains live after.

### 6.5 Mapping

`MapObject(ref, va_hint, offset, length, prot) → va` installs page-
table entries in the calling task's address space that resolve `va`
through `va + length` to the storage of the object referenced by
`ref`, beginning at the given byte offset. The calling task may then
access the storage through ordinary loads and stores in addition to,
or in place of, object-register dereferences. The `prot` parameter is
required to be a subset of the reference's effective capabilities
restricted to `R`, `W`, and `X`.

Mapping is restricted to objects whose home is the local processor, as
stated in Volume I, Section 4. Attempting to map a remote object
returns an error to the caller; the firmware primitive `ObjMigrate`
must be invoked first if local mapping is desired.

The detailed parameter conventions of `MapObject`, `Unmap`, and
`Protect` are given in Volume VI, Section 5.

## 7. Send Handlers

### 7.1 Installation

The `send_handler_ref` and `send_handler_off` fields of the
descriptor name the code that runs when a `SEND` message arrives
addressed to this object. The two fields together specify an entry
point: the code begins at byte `send_handler_off` of the object
referenced by `send_handler_ref`, which must carry the `X` capability.

These fields are populated by the firmware primitive
`InstallHandler(target_ref, handler_ref, handler_off)`. The primitive
does not increment the descriptor's generation; the send-handler
fields are the only mutable parts of the descriptor and are excluded
from the immutability invariant of Section 4.2 because they are read
only by the home processor's firmware on receipt of a `SEND`, and the
read is performed against the live descriptor without any caching.

### 7.2 Dispatch

When a `SEND_DELIVER` message arrives at a processor, firmware on the
receiving side:

1. Validates the recipient reference: the home in the delivered ref
   must be this processor, the index must be in range, the generation
   must match.
2. Reads `send_handler_ref` and `send_handler_off` from the
   descriptor.
3. Constructs a fresh task (or reuses a worker from a pool) and
   transfers control to the handler entry point. The integer payload
   of the SEND is installed verbatim in `R4`–`R7`. The object payload,
   which on the wire carries four references, is delivered to the
   handler asymmetrically:
   - `O1` receives a freshly-constructed, full-capability reference to
     the recipient object itself — the handler's *self* reference.
     This overrides what the first wire-format object payload slot
     would otherwise have carried.
   - `O2`, `O3`, `O4` receive the *first*, *second*, and *third*
     object references from the SEND payload as transmitted on the
     wire, respectively.
   - The *fourth* reference of the SEND payload is delivered to the
     handler's task control side-channel buffer (Volume VI Section 11)
     and is accessible through the firmware primitive
     `MessagePayloadOR4` for handlers that require it.

This dispatch convention applies only to handler invocation. Receive
queues established by `ReceiveQueueAttach` (Volume VI Section 6)
deliver the SEND payload verbatim to the polling task: all four
wire-format object references appear unmodified in `O1`–`O4` of the
polling task on return from `ReceiveQueuePoll`, and the self-reference
convention does not apply.

If no handler is installed (the `send_handler_ref` is null), the
message is dropped and an event is posted to the firmware diagnostic
log. The architecture does not require the message be retried, queued,
or acknowledged in this case; recovery is a software responsibility
above the level of this volume.

## 8. Reserved Fields and Future Extensions

The following fields are reserved in this revision and shall be
written as zero by all conforming firmware:

- Bits 7:0 of an object reference (the `rsv` field).
- Bit 7 of the capability byte (the spare capability bit).
- Bits 6 and 7 of the descriptor's `flags` byte.
- Bytes 14:15 and 28:31 of the descriptor.

Future revisions are expected to consume some of this space for the
following purposes, listed without commitment:

- An additional capability bit naming the right to install send
  handlers, separating it from the present `V` bit.
- A descriptor flag distinguishing read-mostly from write-mostly
  objects, as a hint to the cache-replacement policy.
- A short generation extension widening the counter from sixteen to
  twenty-four or thirty-two bits, addressing the wrap-around concern
  noted in Section 4.2 at the cost of a wider reference format.

— *The Object RISC Architecture Group, 1986*
