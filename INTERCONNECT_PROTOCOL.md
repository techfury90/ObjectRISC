# The Object RISC Architecture

## Volume IV — Interconnect Protocol

*Architecture Reference, Revision 0.1, 1986*

---

## 1. Scope

This volume specifies the wire-level protocol of the Object RISC
crossbar interconnect: the physical packet format, the set of message
types, the arbitration discipline at each output port, the routing of
packets across single and hierarchical crossbar configurations, the
flow-control regime by which sources and sinks coordinate, the
timing and clock-distribution requirements for a conforming
implementation, and the small set of error conditions the protocol
recognizes.

It does not specify the layout of the silicon that implements the
protocol; the OR-XBAR-1 reference crossbar is described in Volume V.
It also does not specify the firmware that interprets the higher-level
semantics carried by these messages; that is the subject of Volume VI.

The reader is assumed to be familiar with Volumes I, II, and III.

## 2. Topology

An Object RISC system consists of one or more *processors*, each
identified by an 8-bit *processor identifier* assigned at boot. Every
processor is a port of the crossbar; the crossbar has no other class
of port. Memory modules are local to each processor and are reached
across the crossbar only by way of that processor's local memory
controller.

The reference configuration is a single 16-port crossbar interconnecting
sixteen processors. The protocol admits configurations of one to
sixteen ports per physical crossbar chip and, by hierarchical
arrangement, up to 256 processors total. The detailed packaging of
larger systems is left to the integrator; the protocol is concerned only
with the contract on the wire.

Each port presents to its processor a *transmit channel* and a
*receive channel*, each carrying one packet word per cycle. The width
of a packet word is 32 bits. The two channels operate independently
and may be driven in the same cycle.

## 3. Packet Format

### 3.1 Header

Every packet begins with a single 64-bit header word (transmitted as
two 32-bit cycles, most significant first) of the following layout:

```
 63        56 55       48 47       40 39       32 31              16 15        0
+-----------+-----------+-----------+-----------+------------------+------------+
|  src_pid  |  dst_pid  |   type    |   flags   |     trans_id     |   length   |
+-----------+-----------+-----------+-----------+------------------+------------+
     8           8           8           8              16              16
```

- **src_pid** (8 bits). The processor identifier of the originating
  port. Receivers use this field to direct responses and to credit
  flow control.
- **dst_pid** (8 bits). The processor identifier of the destination
  port. In a single-crossbar configuration, this is the port number;
  in a hierarchical configuration, it is interpreted by the routing
  table at each intermediate hop (Section 6).
- **type** (8 bits). Selects the message format from the table of
  Section 4.
- **flags** (8 bits). Per-message flags. The bits assigned in this
  revision are described per type; bits 7:6 are reserved at the
  protocol level for `ACK_REQ` and `LAST_FRAGMENT`, which apply
  uniformly to every message type.
- **trans_id** (16 bits). A per-source transaction identifier,
  selected by the source. The destination uses this field to
  correlate request and response messages; the value is opaque to
  intermediate hops.
- **length** (16 bits). The length of the payload, in 32-bit words.
  Zero is permitted and indicates a header-only message. The maximum
  payload of 65,535 words (256 KB) is more generous than any single
  message in this revision uses; longer transfers are decomposed into
  multiple packets in software.

### 3.2 Payload Encoding

The payload immediately follows the header and consists of `length`
contiguous 32-bit words. Each message type specifies the layout of its
payload words; some types have variable payloads (notably the data-
carrying responses), in which case the layout is also defined per type.

A single trailing word, the *checksum*, follows the payload. It carries
a 32-bit word formed by the bitwise exclusive-OR of every header and
payload word transmitted in the packet. A receiver that observes a
checksum mismatch raises `LINK_ERROR` to firmware and discards the
packet.

The checksum is not included in the `length` field; a packet's total
on-wire length is `2 + length + 1` words.

## 4. Message Types

The 256 type codes are partitioned by purpose. Codes `0x10`–`0x1F` are
the object memory-access messages, `0x20`–`0x2F` the inter-task
communication messages, `0x30`–`0x3F` the descriptor maintenance
messages, and `0xF0`–`0xFF` the link-control messages. The remaining
code ranges are reserved for future use.

### 4.1 Object Memory Access

These messages implement the dispatch of `OL*` and `OS*` instructions
issued through references whose home is a different processor.

| Code   | Name              | Direction        | Payload                     |
|--------|-------------------|------------------|-----------------------------|
| `0x10` | `OBJ_READ_REQ`    | issuer → home    | 4 words: ref low, ref high, offset, width |
| `0x11` | `OBJ_READ_RESP`   | home → issuer    | `ceil(width/4)` words of data, byte-padded |
| `0x12` | `OBJ_WRITE_REQ`   | issuer → home    | 4 + `ceil(width/4)` words: ref, offset, width, data |
| `0x13` | `OBJ_WRITE_RESP`  | home → issuer    | 0 words; flags carry success or fault code |

The reference is transmitted little-end first to align with the layout
of the issuing processor's pair-register pair. The home processor
performs the validity, bounds, and capability checks itself: the
issuing processor has already confirmed the cached descriptor's caps
match, but the home is the authoritative source and may discover that
the object has been freed (generation mismatch) or that the descriptor
in the issuing processor's cache was stale. In any such case the
response carries a fault code in `flags` rather than data.

The fault codes carried in `flags` for the response messages are:

| Bits 5:0 | Name                | Meaning                              |
|----------|---------------------|--------------------------------------|
| `0x00`   | `OK`                | Access succeeded                     |
| `0x01`   | `STALE`             | Generation mismatch at home          |
| `0x02`   | `BOUNDS`            | Offset out of range                  |
| `0x03`   | `CAP`               | Capability denied                    |
| `0x04`   | `BUS_ERROR`         | Underlying memory error              |
| `0x3F`   | `FORWARDED`         | Object has migrated; see DESC_FORWARD |

`OBJ_READ_RESP` carries no data words when its flags indicate any
fault other than `OK`; the issuing processor raises the corresponding
synchronous trap on the original instruction.

### 4.2 Inter-Task Communication

| Code   | Name             | Direction      | Payload                       |
|--------|------------------|----------------|-------------------------------|
| `0x20` | `SEND_DELIVER`   | issuer → home  | 14 words: 2 ref, 4 GPR, 8 OR  |

`SEND_DELIVER` is the wire form of the `SEND` instruction. Its
payload comprises:

- two words for the recipient object reference;
- four words for the integer payload (`R4`–`R7`);
- eight words for the four-reference object payload (`O1`–`O4`).

The recipient verifies the reference at delivery, dispatches to the
installed handler, and produces no response packet on success. If the
recipient is invalid (generation mismatch, no installed handler, or
home not this processor), the firmware diagnostic log is appended and
no further action is taken; the architecture does not require a NAK.

The mapping of the eight payload words for object references onto the
handler's object register file is asymmetric: the wire transports four
references, but the handler receives only three of them in `O2`–`O4`,
with `O1` overridden by a fresh self-reference and the fourth wire
slot held in the side-channel buffer. The full convention is given in
Volume III Section 7.2 and the corresponding firmware primitive in
Volume VI Section 6.

`SEND_DELIVER` carries the `ACK_REQ` flag at the discretion of higher-
level software. When set, the recipient sends a `SEND_ACK` (`0x21`)
header-only message back to the source on completion of dispatch. The
ACK does not indicate that the handler has run to completion, only
that it has been entered.

### 4.3 Descriptor Maintenance

These messages keep each processor's object descriptor cache coherent
with the object table at the home processor.

| Code   | Name              | Direction          | Payload             |
|--------|-------------------|--------------------|---------------------|
| `0x30` | `DESC_REQ`        | issuer → home      | 1 word: index       |
| `0x31` | `DESC_RESP`       | home → issuer      | 8 words: descriptor |
| `0x32` | `DESC_FORWARD`    | home → issuer      | 8 words: new descriptor (with new home and index)|
| `0x33` | `DESC_INVALIDATE` | home → all (broadcast) | 1 word: index   |

`DESC_REQ` is issued by an issuing processor on a miss in its object
descriptor cache; the home responds with the live descriptor in a
`DESC_RESP`. If the home's descriptor is `FORWARDED`, the response
type is instead `DESC_FORWARD`, and the issuing processor installs
the descriptor under the *original* `(home, index)` tag of the
requested object, redirecting subsequent accesses to the migrated
location while preserving the validity of the original reference.

`DESC_INVALIDATE` is issued by a home processor when it has reason to
believe one or more issuing processors hold a stale entry for an
object whose state has changed in a manner not captured by the
generation counter — for example, when the send handler is updated.
In the present revision the only field that may be modified without a
generation increment is the send handler, which is read only at the
home and therefore never cached at issuing processors;
`DESC_INVALIDATE` is consequently reserved but unused. It is specified
here against future extensions that might cache further descriptor
fields at the issuing side.

### 4.4 Link Control

| Code   | Name                  | Direction        | Payload          |
|--------|-----------------------|------------------|------------------|
| `0xF0` | `LINK_FLOW_CREDIT`    | port → port      | 1 word: credits  |
| `0xF1` | `LINK_ERROR`          | port → firmware  | 1 word: cause    |
| `0xFE` | `LINK_HEARTBEAT`      | port → port      | 0 words          |
| `0xFF` | `LINK_RESET`          | port → all       | 0 words          |

Link-control messages are not delivered to any processor's user-
visible interface; they are consumed by the crossbar interface
hardware on the receiving port. The single exception is `LINK_ERROR`,
which is forwarded to firmware on the receiving processor for
diagnostic logging.

`LINK_HEARTBEAT` is exchanged at intervals defined by the
implementation (typically every 4,096 cycles) to confirm liveness;
the absence of expected heartbeats is reported through `LINK_ERROR`.

`LINK_RESET` is broadcast on power-up and on firmware request; it
returns every input FIFO and every flow-control counter to its initial
state, and is the only message permitted to bypass arbitration.

## 5. Arbitration

Each output port arbitrates among the input ports requesting it in any
given cycle. The arbitration discipline is *rotating priority*: the
output port maintains a single 8-bit register, the priority pointer,
which selects the input port that, among those requesting in the
current cycle, is granted access. The pointer advances by one position
modulo the port count after each grant.

This discipline guarantees that no requesting input is starved for more
than `N − 1` cycles, where `N` is the port count, regardless of the
demand pattern. It is also synthesizable in a single cycle for the
reference 16-port configuration in 1.5-micron CMOS, which is the
underlying constraint motivating its choice.

Arbitration is performed independently at each output port. There is no
global serialization of grants; up to `N` packets may begin
transmission in the same cycle, provided each has a distinct output
port.

## 6. Routing

### 6.1 Single-Crossbar Configuration

In a single-crossbar configuration the destination processor identifier
in the packet header is the physical output port number. The crossbar
inspects only this field and the source identifier; no routing table
is consulted, and no per-hop state is maintained.

The single-crossbar configuration is the configuration of any system of
sixteen processors or fewer. We expect it to remain the dominant
configuration for the lifetime of this revision.

### 6.2 Hierarchical Configuration

Configurations of more than sixteen processors are constructed by
arranging multiple crossbar chips in a hierarchy: a *root* crossbar
whose ports each connect to a *leaf* crossbar, with processors
attached to the leaves. The reference scaling, which permits up to 256
processors, is a single root with sixteen leaves of sixteen ports
each, with one port on each leaf consumed by the connection to the
root.

In a hierarchical configuration, every port of every crossbar chip
carries a *routing table* of 256 entries, indexed by the destination
processor identifier and producing a 4-bit *next-hop port number*.
The table is loaded by firmware at boot and is invariant during
operation. A packet arriving at a port whose routing table indicates
its own number is delivered to the local processor (for a leaf) or
discarded with a `LINK_ERROR` (for any other case, indicating a
configuration error).

Larger configurations than the 256-processor reference scaling are
permitted, but require widening the destination identifier field
beyond 8 bits and consequently the routing table; such extensions are
deferred to a future revision.

## 7. Flow Control

Each input port maintains a fixed-size FIFO of 32 words. Flow control
is credit-based: the source maintains a counter of available credits
for each destination it transmits to, decrementing the counter by the
length of each transmitted packet (including header and checksum).
The destination periodically returns a `LINK_FLOW_CREDIT` message
indicating credits liberated by the FIFO's drainage.

A source whose credit counter is too low to admit a packet stalls the
packet's transmission until a `LINK_FLOW_CREDIT` arrives. The
processor's crossbar interface presents the stall to the issuing
instruction as a memory stall indistinguishable from a long-latency
cache miss; the architecture observably reflects flow-control stalls
only in the cycle counts, not in the program-visible state.

The 32-word input FIFO is sized to absorb at least one full
`SEND_DELIVER` packet (17 words including header and checksum) plus
one full `OBJ_WRITE_REQ` packet of moderate width; conforming
implementations may size FIFOs more generously to reduce flow-control
round trips, at the obvious cost in silicon area.

## 8. Timing and Synchronization

The reference crossbar is fully synchronous: every port samples its
input on the rising edge of a single distributed clock, and every
arbitration decision is taken in the same cycle as the corresponding
port-grant. Implementations may choose between a directly distributed
clock and a phase-locked loop per port, but the architecture requires
that the maximum cycle-to-cycle skew between any two ports of the same
crossbar chip not exceed one nanosecond at the reference 20 MHz clock
rate.

Inter-port latency at a single crossbar chip is two cycles plus the
packet length: one cycle for arbitration, one cycle to traverse the
switching matrix, and one cycle per word transmitted thereafter. In
the reference 16-port configuration at 20 MHz, a four-word
`OBJ_READ_REQ` therefore consumes 6 cycles or 300 nanoseconds at the
crossbar itself; the round-trip latency from issue to data return,
including dispatch at the home processor, is approximately 30 cycles.

In a hierarchical configuration each additional hop adds one cycle of
arbitration and one cycle of traverse, plus any flow-control stall.

## 9. Error Detection and Recovery

The crossbar protocol recognizes a small set of error conditions, all
of which are reported via `LINK_ERROR` to firmware on the receiving
port for diagnostic logging. The architecture does not require
hardware-level retry; recovery from any error is a firmware policy.

| Cause word | Name                       | Source                          |
|------------|----------------------------|---------------------------------|
| `0x01`     | `CHECKSUM_MISMATCH`        | Receiver, on bad packet         |
| `0x02`     | `RESERVED_TYPE`            | Receiver, on undefined message type |
| `0x03`     | `MISDIRECTED`              | Crossbar, on routing-table miss |
| `0x04`     | `FIFO_OVERFLOW`            | Receiver, on credit violation   |
| `0x05`     | `HEARTBEAT_LOST`           | Receiver, on missed heartbeat   |
| `0x06`     | `RESET_OBSERVED`           | Receiver, after `LINK_RESET`    |

A receiver that detects `CHECKSUM_MISMATCH` discards the packet and
does not reply; the source learns of the loss only by way of an
absent response (for request/response message pairs) or not at all
(for fire-and-forget messages such as `SEND_DELIVER`). Higher-level
recovery, including timeouts and retransmission, is a software
responsibility.

## 10. Reserved Fields and Future Extensions

The following encodings are reserved in this revision and shall not be
used by conforming implementations:

- Type codes `0x00`–`0x0F`, `0x14`–`0x1F`, `0x22`–`0x2F`, `0x34`–`0xEF`,
  `0xF2`–`0xFD` (every code not enumerated in Section 4).
- Bits 5:0 of the header `flags` byte for non-response messages.
- Bits 4 and 3 of the response fault-code field.

Anticipated extensions that have already been provisionally allocated:

- An optional broadcast message type, `0x40`, for system-wide
  notifications such as global time advancement.
- A descriptor-cache-fill prefetch hint, `0x35`, by which firmware on
  one processor may speculatively warm the descriptor cache of another.
- A widened destination identifier for systems of more than 256
  processors, requiring a different header layout and consequently a
  different protocol revision number.

— *The Object RISC Architecture Group, 1986*
