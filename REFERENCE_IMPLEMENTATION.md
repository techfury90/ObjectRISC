# The Object RISC Architecture

## Volume V — Reference Implementation

*Architecture Reference, Revision 0.1, 1986*

---

## 1. Scope

This volume describes a representative implementation of the Object RISC
architecture: the OR-1000 processor and the OR-XBAR-1 crossbar. The
implementation is offered as an existence proof that a useful Object RISC
system can be constructed in 1.5-micron CMOS at 1986 transistor budgets,
and as a yardstick against which alternative implementations may be
measured. Conforming implementations are not required to match it in
microarchitecture, only in the architectural contracts of Volumes I
through IV and the firmware interface of Volume VI.

The volume covers, in order: the OR-1000 pipeline; its caches, TLB, and
object descriptor cache; the multiplier and divider; the external bus
and crossbar interface; the architecturally visible control register
set; the OR-XBAR-1 crossbar's port organization, arbitration, and
buffering; representative system configurations from a single-processor
workstation to a 16-processor server; the reset and boot sequence; the
physical characteristics of both chips; and a small body of performance
estimates.

The reader is assumed to be familiar with Volumes I through IV.

## 2. The OR-1000 Processor

### 2.1 Specifications

| Parameter                       | Value                                  |
|---------------------------------|----------------------------------------|
| Process                         | 1.5-micron, two-layer-metal CMOS       |
| Transistors                     | approximately 110,000                  |
| Die size                        | 9.8 mm × 9.8 mm                        |
| Pin count                       | 168 (PGA)                              |
| Clock                           | 16 MHz (commercial), 20 MHz (selected) |
| Power dissipation               | 2.4 W typical at 16 MHz, 5 V supply    |
| Pipeline                        | 5-stage, single-issue, in-order        |
| Instruction cache               | 4 KB, direct-mapped, 16-byte lines     |
| Data cache                      | 4 KB, direct-mapped, 16-byte lines     |
| Object descriptor cache         | 32 entries, 4-way set associative      |
| TLB                             | 32 entries, fully associative          |
| Page size                       | 4 KB                                   |
| External data bus               | 32 bits                                |
| External address bus            | 32 bits                                |
| Crossbar port                   | 32-bit data, dedicated channel         |

### 2.2 Pipeline Organization

The processor implements a five-stage pipeline whose stages we name
*IF*, *ID*, *EX*, *MEM*, and *WB*.

**IF — Instruction Fetch.** The contents of the program counter are
applied to the instruction cache and to the instruction-side TLB
simultaneously. On a hit, a 32-bit instruction is returned at the end
of the cycle and latched. The next-PC predictor unconditionally
selects PC+4; conditional branches are resolved in EX, and a misprediction
is paid for by squashing the instruction in ID. Reset, exception, and
unconditional branch redirect the next-PC at the end of the cycle in
which they are recognized.

**ID — Decode and Register Read.** The latched instruction is decoded;
the source registers (general or object, as required) are read from
their respective register files; the immediate is sign- or zero-
extended; hazards against EX and MEM are detected and the appropriate
forwarding paths are armed. Reading an object register against a
descriptor-cache lookup begins in this stage and completes in EX.

**EX — Execute.** The arithmetic-logic unit produces its result. For
loads and stores through general registers, the effective address is
computed. For loads and stores through object registers, the
effective offset is computed in parallel with the descriptor cache
lookup; the validity, bounds, and capability checks complete by the
end of EX, and a failure raises a precise trap that suppresses the
remaining stages of the instruction. For branches, the comparison is
performed and the next-PC is redirected on the EX-stage clock edge.

**MEM — Memory Access.** For loads and stores through general
registers, the data cache is consulted; on a hit, the data are
returned at the end of the cycle. For loads and stores through object
registers, either the data cache (for local objects) or the crossbar
interface (for remote objects) is engaged. A cache miss stalls the
pipeline; a remote access stalls until the response packet returns.
Stores complete in this stage; their data are committed to the cache
on the cycle's clock edge and to the write buffer for propagation to
main memory.

**WB — Writeback.** The result of the instruction, if any, is written
to the destination register on the rising edge of the WB-stage clock.
The instruction is retired.

### 2.3 Hazards and Forwarding

Two forwarding paths short-circuit the register file: from the EX-
stage ALU output to the EX-stage ALU input, and from the MEM-stage
load result to the EX-stage ALU input. The first eliminates the
read-after-write delay on chains of arithmetic instructions; the
second reduces, but does not eliminate, the cost of a load-use
sequence.

Load-use hazards are *not* hardware-interlocked, in keeping with the
architectural specification of Volume II. The compiler is expected to
schedule an unrelated instruction into the load-delay slot. If it
cannot, a `NOP` is inserted; the result of the load is undefined if
read in the immediately succeeding instruction. The compiler-fill
rate observed in the reference toolchain on representative C
workloads is approximately 70%.

Branch delay slots are likewise compiler-filled; the observed fill
rate is approximately 60%.

The multiplier and divider, described in Section 2.8, run
independently of the main pipeline. A consumer of `HI` or `LO` that
issues before the producer has completed stalls in MEM until the
producer retires its result.

### 2.4 Instruction Cache

The instruction cache is 4 KB, direct-mapped, with 16-byte lines and
20-bit tags. Eight bits of the address index the 256 lines; four bits
select the byte within the line. On a miss, the cache requests a
4-word burst from main memory; the missed instruction is forwarded
from the bus to the IF stage as it arrives, and the remaining three
words are written into the cache concurrently. The fill takes seven
cycles in the reference memory configuration.

The cache is virtually indexed and physically tagged. The
instruction-side TLB is consulted in parallel with the cache lookup;
the physical tag from the TLB is compared with the cache tag at the
end of the IF cycle. Misalignment of the program counter raises
`address-misaligned-i` before the lookup completes.

Cache lines are not snooped from the data cache; self-modifying code
must invalidate the affected line through a privileged firmware
primitive before the modified instruction is fetched.

### 2.5 Data Cache

The data cache is also 4 KB, direct-mapped, with 16-byte lines. It is
write-through and no-write-allocate: stores update the cache on a
hit but do not allocate on a miss; in either case the store is
forwarded to a four-entry write buffer that drains to main memory
asynchronously. The buffer permits up to four pending writes to be
absorbed before stores stall the pipeline.

The data cache is virtually indexed and physically tagged in the same
manner as the instruction cache. A cache miss services a 4-word
burst, taking seven cycles in the reference memory configuration; the
requested word is forwarded to MEM as it arrives.

The cache is not coherent with the crossbar: remote accesses bypass
the local data cache entirely, dispatching directly to the crossbar
interface. This is the practical consequence of the architectural
decision recorded in Volume I, Section 7, and avoids the substantial
complexity that any cross-crossbar coherence protocol would impose.

### 2.6 Object Descriptor Cache

The object descriptor cache (ODC) is 32 entries, organized as eight
sets of four ways. The tag is the 32-bit concatenation of the home
processor identifier and the local table index of the cached object
(8 + 24 bits, with the upper 8 bits zero in single-crossbar
configurations). The data portion of each entry is a 32-byte
descriptor; the cache therefore occupies 1 KB of descriptor storage
plus tag and replacement state.

Lookup is performed in parallel with effective-address computation
during EX. A hit returns the descriptor in time for the bounds and
capability checks to complete in the same cycle; a miss stalls the
pipeline and dispatches a `DESC_REQ` packet to the home processor
through the crossbar interface. The reference implementation observes
ODC miss rates below one percent on representative workloads of the
firmware reference test suite.

Replacement within a set is least-recently-used. Firmware may flush
the ODC explicitly by writing the appropriate firmware-only control
register (Section 2.10).

### 2.7 Translation Lookaside Buffer

The TLB is 32 entries, fully associative. Each entry holds a 20-bit
virtual page number, an 8-bit address-space identifier, a 20-bit
physical page number, and a 4-bit permission field encoding read,
write, execute, and global bits. On every memory access, the TLB is
queried; a miss raises `tlb-miss-i` or `tlb-miss-d` to firmware,
which is responsible for loading the appropriate entry through the
`TLBWI` and `TLBWR` instructions.

The address-space identifier permits multiple per-task address spaces
to coexist in the TLB without flushing on context switch; firmware
loads the current task's ASID into the appropriate field of the
context register before resuming the task.

Replacement under `TLBWR` is governed by the value of the `RANDOM`
control register, which decrements on every cycle and wraps modulo
the TLB size; firmware that wishes to pin a small set of entries may
do so by maintaining `RANDOM` above the pinned region's index.

### 2.8 Multiplier and Divider

The multiplier is a 16×16 booth array iterated twice per 32×32
operation; a `MULT` or `MULTU` completes in twelve cycles and
deposits the 64-bit product in `HI:LO`. The divider is a non-
restoring serial unit producing one bit per cycle; a `DIV` or `DIVU`
completes in thirty-five cycles. Both units run concurrently with the
main pipeline; only a consumer of `HI` or `LO` stalls.

The multiplier and divider are unprotected from sign issues at the
boundary cases (`INT_MIN / -1`); firmware traps `arithmetic-overflow`
on the offending result.

### 2.9 External Bus and Crossbar Interface

The OR-1000 presents two distinct external interfaces: a conventional
synchronous memory bus carrying 32 bits of data and 32 bits of
address with associated control, and a dedicated crossbar port
carrying 32 bits of data and the small handful of strobes required
by the protocol of Volume IV.

The memory bus is multiplexed in the conventional manner, with bus
transactions taking five cycles in the reference configuration: one
to drive the address, three to transfer data (one per word of a
four-word burst is the typical case), and one for turnaround.

The crossbar port operates synchronously to the same clock as the
processor and carries one packet word per cycle in each direction.
The interface buffers up to four packets in each direction; flow-
control credits are managed in hardware according to Volume IV,
Section 7.

### 2.10 Control Registers

The architecturally visible control registers, accessed through
`LCTRL` and `SCTRL`, are numbered as follows. The accessibility
column indicates whether supervisor mode (sv) or only firmware (fw)
may read or write each register.

| Number | Name           | Access | Contents                              |
|--------|----------------|--------|---------------------------------------|
| 0      | `STATUS`       | sv/fw  | Current mode, interrupt mask          |
| 1      | `CAUSE`        | sv/fw  | Last exception cause and BD bit       |
| 2      | `EPC`          | sv/fw  | Exception program counter             |
| 3      | `BADVADDR`     | sv/fw  | Faulting virtual address              |
| 4      | `CONTEXT`      | sv/fw  | Page-table base and current ASID      |
| 5      | `COUNT`        | sv/fw  | Free-running cycle counter            |
| 6      | `COMPARE`      | sv/fw  | Timer interrupt compare value         |
| 7      | `PROCID`       | sv/fw  | This processor's identifier (read-only) |
| 8      | `VECBASE`      | fw     | Base of the firmware vector table     |
| 9      | `TLBHI`        | fw     | TLB entry: virtual page number, ASID  |
| 10     | `TLBLO`        | fw     | TLB entry: physical page, permissions |
| 11     | `INDEX`        | fw     | Index for `TLBWI`, `TLBR`             |
| 12     | `RANDOM`       | fw     | Index for `TLBWR` (decrements freely) |
| 13     | `OBJTAB_BASE`  | fw     | Physical base of object table         |
| 14     | `OBJTAB_LIMIT` | fw     | Number of slots in object table       |
| 15     | `ROUTE_BASE`   | fw     | Physical base of routing table        |
| 16     | `ODC_TAG`      | fw     | Tag of ODC entry being read or written |
| 17     | `ODC_DATA`     | fw     | Data of ODC entry being read or written |
| 18–31  | reserved       |        | Read as zero; writes ignored          |

The supervisor-accessible subset (registers 0–7) suffices for the
trap-handling and cycle-accounting needs of an operating system; the
firmware-only subset (registers 8 and above) controls the TLB, the
object table, the crossbar routing, and the descriptor cache.

## 3. The OR-XBAR-1 Crossbar

### 3.1 Specifications

| Parameter                       | Value                                  |
|---------------------------------|----------------------------------------|
| Process                         | 1.5-micron, two-layer-metal CMOS       |
| Transistors                     | approximately 80,000                   |
| Die size                        | 8.5 mm × 8.5 mm                        |
| Pin count                       | 264 (PGA)                              |
| Clock                           | 16 MHz nominal                         |
| Ports                           | 16, each 32-bit data plus control      |
| Input FIFO depth                | 32 words per port                      |
| Arbitration latency             | 1 cycle per output port                |
| Switch traversal                | 1 cycle                                |

### 3.2 Port Organization

Each of the sixteen ports presents an identical electrical interface:
32 bits of data in each direction, four bits of control encoding
packet-word framing and link-control, and the shared synchronous
clock. The transmit and receive channels are independent and may be
driven in the same cycle.

A port's input FIFO is 32 words deep, sized as Volume IV Section 7
requires. The output side has no FIFO of its own; arbitration grants
the use of the switching matrix for one packet at a time, with the
source's output stream latched directly into the matrix on grant.

### 3.3 Arbitration

Arbitration at each output port is rotating-priority, as Volume IV
Section 5 specifies. The implementation is a 16-bit one-hot state
register holding the current priority pointer, combined with a
15-input request mask gated by the pointer; the granted input is
selected by the low-order set bit of the masked request vector. The
implementation completes in approximately 8 nanoseconds in 1.5-micron
CMOS, comfortably within a single 50-nanosecond cycle at 20 MHz.

### 3.4 Switching Matrix

The matrix is a 16×16 fully-connected crosspoint of 32-bit lanes,
implemented as 32 independent 16-input multiplexers, one per data
bit. The control inputs of all 32 multiplexers in a row are tied
together and driven by the granted-input signal from the
corresponding output port's arbiter.

### 3.5 Diagnostic and Reset

A single diagnostic port, electrically distinct from the sixteen data
ports, presents a small register file accessible by an attached
service processor. The registers permit read-out of per-port packet
counts, error counts, and the current priority pointer of each
output port, and allow firmware-controlled assertion of `LINK_RESET`
on any subset of ports.

## 4. System Configurations

### 4.1 Single-Processor Workstation

The minimum conforming Object RISC configuration consists of one
OR-1000, four to sixteen megabytes of dynamic memory, a small ROM
holding the firmware image, and an I/O subsystem of the integrator's
choosing. The crossbar is omitted; the processor's crossbar port is
either left unconnected or, in some implementations, looped back
through a small on-board controller that responds to messages
addressed to processor identifiers other than zero with a
`MISDIRECTED` link error.

The single-processor configuration exercises the entire architecture
except for inter-processor communication. The firmware reduces to a
minimal kernel of task management, memory management, and I/O; the
hypervisor primitives of Volume VI degrade gracefully into a
single-tenant model.

### 4.2 Four-Way and Eight-Way Server

The intermediate configurations comprise four or eight OR-1000
processors connected to a single OR-XBAR-1 with the unused ports
unconnected. Each processor has its own local memory, of typically
sixteen megabytes; total system memory is therefore 64 to 128
megabytes, partitioned among the processors and reachable globally
through the crossbar.

The unused ports of the crossbar are a sunk cost in silicon; the
chip is not made smaller for smaller configurations. Integrators
seeking a lower-cost intermediate scaling may instead use a smaller
crossbar, of which two- and four-port variants have been designed
internally and are described in supplementary documents not part of
the present revision.

### 4.3 Sixteen-Way Server

The maximum single-crossbar configuration uses all sixteen ports of
one OR-XBAR-1, connecting sixteen OR-1000 processors with their
local memory. A typical 1986 cabinet houses two such configurations
(a "two-cluster" system of thirty-two processors) with the clusters
joined by a hierarchical second-level crossbar; processor identifiers
0 through 15 are assigned to one cluster and 16 through 31 to the
other.

The hierarchical configuration is not a tested reference in this
revision but is supported by the protocol of Volume IV. Volume V
concerns itself only with the contents of a single cluster.

### 4.4 Reset and Boot Sequence

On power-up or reset, every OR-1000 enters firmware mode with all
interrupts masked and the program counter set to a fixed reset
vector address. The reset vector is at offset zero of the firmware
ROM, which is mapped at a physical address determined by the
configuration of two strap pins.

The reset sequence proceeds as follows:

1. Each processor reads its `PROCID` and selects the appropriate
   branch of the boot code: processor zero is the *boot processor*
   and proceeds with system initialization; processors of higher
   identifier execute a small wait loop, polling a memory location
   that the boot processor will later write.
2. The boot processor sizes its local memory by a destructive write-
   read sweep, initializes the object table at a firmware-chosen
   physical base, programs `OBJTAB_BASE` and `OBJTAB_LIMIT`, clears
   the TLB and ODC, and configures `VECBASE`.
3. The boot processor configures the crossbar by writing routing
   tables (in hierarchical configurations) through the diagnostic
   port, and exchanges `LINK_HEARTBEAT` with each peer to confirm
   connectivity.
4. The boot processor releases the other processors by writing the
   well-known location they are polling; each then performs an
   abbreviated form of steps 2 and 3 against its local resources,
   omitting the global crossbar configuration.
5. Each processor proceeds to load the operating system image from
   the boot device, transferring control out of firmware by `ERET`.

The reset sequence is timed at approximately 50 milliseconds in the
reference implementation, dominated by the memory-sizing sweep.

## 5. Physical Characteristics

### 5.1 Pin Assignment Summary

The OR-1000's 168 pins partition as follows:

| Function                             | Pin count |
|--------------------------------------|-----------|
| Memory address bus                   | 32        |
| Memory data bus                      | 32        |
| Memory control                       | 12        |
| Crossbar transmit data               | 32        |
| Crossbar receive data                | 32        |
| Crossbar control                     | 8         |
| Interrupts                           | 8         |
| Clock and reset                      | 4         |
| Test, diagnostic, and strap          | 8         |
| Power and ground                     | 32 (16 pairs) |

The OR-XBAR-1's 264 pins are dominated by the sixteen ports of 36
pins each (32 data + 4 control bidirectional), with the remainder
dedicated to power, clock, and the diagnostic interface.

### 5.2 Power and Thermal

The OR-1000 dissipates 2.4 W typical at 16 MHz and 5 V; selected
parts at 20 MHz dissipate 3.0 W. The OR-XBAR-1 dissipates 1.8 W
typical at 16 MHz. Both chips are specified for operation at junction
temperatures up to 85°C and require no forced air in
representative cabinets.

## 6. Performance

### 6.1 Single-Processor Workloads

Measured cycles per instruction on the firmware reference test suite:

| Workload                           | CPI   | Notes                       |
|------------------------------------|-------|-----------------------------|
| Integer arithmetic kernel          | 1.05  | Compiler-scheduled          |
| Pointer-traversal kernel           | 1.40  | Cache fits working set      |
| Object-register-heavy kernel       | 1.18  | All ODC hits                |
| Operating-system mix               | 1.55  | Includes TLB-miss handling  |

At 20 MHz the corresponding native performance falls in a range
broadly comparable to first-generation MIPS R2000 and SPARC parts of
the same era; the architecture's distinguishing features impose no
measurable penalty on integer code that does not exercise them.

### 6.2 Inter-Processor Communication

Measured latencies on a 16-processor reference configuration at 16 MHz:

| Operation                                      | Cycles | Time     |
|------------------------------------------------|--------|----------|
| Local `OL*` (descriptor cache hit)             | 2      | 125 ns   |
| Remote `OL*` (descriptor cache hit, no contention) | 28 | 1.75 µs  |
| Remote `OL*` (descriptor cache miss)           | 58     | 3.6 µs   |
| `SEND` issue to handler entry                  | 22     | 1.4 µs   |
| `CALL` to a leaf firmware primitive            | 18     | 1.1 µs   |

These numbers compare favourably with the message-passing latencies
of contemporary tightly-coupled multiprocessors, and we believe they
validate the decision to make the crossbar a primitive of the
architecture rather than a peripheral.

### 6.3 Comparative Position

Among contemporary architectures of similar scale and process, the
OR-1000 is most directly comparable to the MIPS R2000 in single-
processor performance and to the INMOS T800 transputer in
communication latency. We are unaware of any commercially available
architecture in 1986 that combines the two within a single instruction
set, and we offer this combination as the principal contribution of
the work.

— *The Object RISC Architecture Group, 1986*
