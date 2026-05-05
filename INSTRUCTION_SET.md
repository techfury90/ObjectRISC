# The Object RISC Architecture

## Volume II — Instruction Set

*Architecture Reference, Revision 0.1, 1986*

---

## 1. Scope

This volume specifies the instructions executed by an Object RISC
processor in user, supervisor, and firmware modes. It covers the integer
subset, the operations on object registers, the inter-processor
communication primitive `SEND`, the firmware-call primitive `CALL`, the
small number of privileged instructions, and the trap and exception
behaviour common to all of them.

It does not cover the wire-level format of crossbar messages (Volume IV),
the cycle-by-cycle behaviour of any particular implementation (Volume V),
or the firmware primitive set reached through `CALL` (Volume VI). Where
those volumes describe details visible to the programmer, this volume
states the architectural contract; conforming implementations may differ
in timing and microarchitecture but must agree on the contract.

The reader is assumed to be familiar with Volume I.

## 2. Notation

In the descriptions that follow:

- `Rs`, `Rt`, `Rd` denote general-purpose registers, with `Rs` and `Rt`
  reading their values and `Rd` receiving the result.
- `Os`, `Ot`, `Od` denote object registers under the same convention.
- `imm` denotes a 16-bit immediate field, sign-extended to 32 bits unless
  noted otherwise; `uimm` is the same field zero-extended.
- `target` denotes the 26-bit target field of a J-type instruction.
- `offset` denotes a 16-bit signed displacement in bytes.
- `←` denotes assignment; `||` denotes bitwise concatenation; `M[a]` and
  `M[a:b]` denote memory contents at byte address `a`, the latter for a
  range.
- All addresses are byte addresses. Multi-byte accesses must be
  naturally aligned; misalignment raises a synchronous trap.
- Instruction encodings are written most-significant bit first, in the
  big-endian byte order used throughout the architecture.

## 3. Register Model and Calling Conventions

### 3.1 General-Purpose Registers

The processor provides thirty-two 32-bit general-purpose registers,
`R0` through `R31`. `R0` is hardwired to zero: writes are silently
discarded and reads return the constant zero. `R31` is, by convention,
the link register written by `JAL` and `JALR`; it has no special
hardware treatment outside of those instructions.

The standard calling convention partitions the register file as
follows:

| Register      | Use                                                 |
|---------------|-----------------------------------------------------|
| `R0`          | constant zero                                       |
| `R1`          | reserved as assembler temporary                     |
| `R2`–`R3`     | integer return values                               |
| `R4`–`R7`     | integer arguments                                   |
| `R8`–`R15`    | caller-saved temporaries                            |
| `R16`–`R23`   | callee-saved                                        |
| `R24`–`R28`   | additional caller-saved temporaries                 |
| `R29`         | stack pointer (`SP`)                                |
| `R30`         | frame pointer (`FP`)                                |
| `R31`         | link register (`RA`); set by `JAL`, `JALR`          |

The convention is recommended, not enforced by hardware. Code that
violates it executes correctly but does not interoperate with the
standard runtime.

### 3.2 Object Registers

The processor provides sixteen object registers, `O0` through `O15`,
each holding a 64-bit object reference whose internal layout is
specified in Volume III. `O0` is hardwired to the *null reference*:
writes are silently discarded and reads return the null constant. The
null reference compares equal only to itself and traps on any attempt
to dereference it.

The standard calling convention partitions the object register file as
follows:

| Register      | Use                                                 |
|---------------|-----------------------------------------------------|
| `O0`          | null reference                                      |
| `O1`–`O4`     | object arguments and first object return value      |
| `O5`–`O8`     | caller-saved temporaries                            |
| `O9`–`O12`    | callee-saved                                        |
| `O13`–`O15`   | additional caller-saved temporaries                 |

The same convention governs the payload of `SEND`: object arguments to
the remote handler are placed in `O1`–`O4` and integer arguments in
`R4`–`R7`, exactly as for a local call. A remote invocation therefore
differs from a local one only in the instruction used to issue it.

### 3.3 Program Counter and Status

The program counter is not directly readable as a general-purpose
register; its value is captured by `JAL`, `JALR`, and the trap
mechanism. A small number of architecturally-visible status registers
(current mode, interrupt enable mask, exception program counter,
exception cause, and the like) are accessed through the privileged
`LCTRL` and `SCTRL` instructions described in Section 12. Their
detailed contents are specified in Volume V.

## 4. Instruction Formats

All instructions are 32 bits and aligned on 4-byte boundaries.
Misaligned instruction fetch raises a bus-error trap.

Four formats are defined.

### 4.1 R-Type (Register-Register)

```
 31      26 25  21 20  16 15  11 10   6 5      0
+----------+------+------+------+------+--------+
|  opcode  |  rs  |  rt  |  rd  | shamt|  funct |
+----------+------+------+------+------+--------+
     6        5      5      5      5       6
```

The major opcode for all R-type instructions is `0x00` (`SPECIAL`); the
6-bit `funct` field selects the operation.

### 4.2 I-Type (Register-Immediate)

```
 31      26 25  21 20  16 15                   0
+----------+------+------+----------------------+
|  opcode  |  rs  |  rt  |      immediate       |
+----------+------+------+----------------------+
     6        5      5             16
```

Used for arithmetic with immediates, conditional branches, and loads
and stores through general-purpose registers.

### 4.3 J-Type (Jump)

```
 31      26 25                                  0
+----------+-------------------------------------+
|  opcode  |              target                 |
+----------+-------------------------------------+
     6                       26
```

The 26-bit `target` is shifted left by two and combined with the upper
four bits of the address of the delay-slot instruction to form the
absolute branch target.

### 4.4 O-Type (Object Operations)

Two related formats serve the object register file.

The *object register-register* form is used for operations whose only
operands are object registers and (optionally) general registers:

```
 31      26 25 22 21 18 17  13 12   8 7   4 3   0
+----------+----+----+------+------+-----+-----+
|  opcode  | os | ot |  rd  |  rs  |funct| rsv |
+----------+----+----+------+------+-----+-----+
     6       4    4     5      5     4     4
```

The *object load-store* form replaces the trailing fields with a
16-bit signed offset for object-relative memory access:

```
 31      26 25 22 21    16 15                  0
+----------+----+--------+----------------------+
|  opcode  | os |  rt'   |        offset        |
+----------+----+--------+----------------------+
     6       4      6              16
```

The `rt'` field is six bits at positions 21:16, of which the high bit
(bit 21) is reserved and must be zero in this revision; the low five
bits hold the GPR number, as in every other I-type. Future widening of
the register file may consume the reserved bit.

## 5. Integer Computation

The integer subset is conventional for its era. Three-operand register
forms are encoded as R-type with major opcode `SPECIAL` and a per-
operation `funct`; immediate forms are encoded as I-type.

| Mnemonic                    | Effect                                          |
|-----------------------------|-------------------------------------------------|
| `ADD Rd, Rs, Rt`            | `Rd ← Rs + Rt`; trap on signed overflow         |
| `ADDU Rd, Rs, Rt`           | `Rd ← Rs + Rt`; no overflow trap                |
| `SUB`, `SUBU`               | as `ADD`/`ADDU`, with subtraction               |
| `AND`, `OR`, `XOR`          | bitwise                                         |
| `NOR Rd, Rs, Rt`            | `Rd ← ¬(Rs ∨ Rt)`                               |
| `SLL Rd, Rt, sa`            | logical shift left by 5-bit `shamt`             |
| `SRL`, `SRA`                | logical and arithmetic shift right              |
| `SLLV`, `SRLV`, `SRAV`      | variable shift; count is low five bits of `Rs`  |
| `SLT Rd, Rs, Rt`            | `Rd ← 1` if `Rs < Rt` (signed) else `0`         |
| `SLTU`                      | as `SLT`, unsigned                              |
| `MULT`, `MULTU`             | 64-bit product, deposited in `HI:LO`            |
| `DIV`, `DIVU`               | 32-bit quotient in `LO`, remainder in `HI`      |
| `MFHI Rd`, `MFLO Rd`        | move multiplier result registers to `Rd`        |

Immediate forms `ADDI`, `ADDIU`, `ANDI`, `ORI`, `XORI`, `SLTI`, and
`SLTIU` follow the I-type format; the immediate is sign-extended for
the arithmetic and comparison operations and zero-extended for the
bitwise ones. `LUI Rt, imm` loads the immediate into the upper sixteen
bits of `Rt`, clearing the lower sixteen, and is the standard means of
constructing a 32-bit constant in two instructions.

There are no condition codes. All branches read general registers
directly. We consider the avoidance of a global condition-code register
to be one of the more important simplifications of the RISC approach;
instructions remain free of an implicit dependency that would otherwise
serialize the pipeline at every comparison.

The multiplier and divider produce results into the implicit `HI` and
`LO` registers and proceed concurrently with the rest of the pipeline.
Issuing a `MULT` or `DIV` while the previous one has not completed is
permitted but stalls the issuing instruction until the prior result is
read out of `HI` or `LO`. The reference implementation completes a
32×32 multiply in twelve cycles and a divide in thirty-five.

`ADD`, `SUB`, and `ADDI` raise an arithmetic-overflow trap on signed
overflow; the unsigned-suffixed variants do not. Generated code that
does not require overflow detection should use the unsigned forms.

## 6. Loads and Stores Through General Registers

| Mnemonic                   | Effect                                           |
|----------------------------|--------------------------------------------------|
| `LB Rt, offset(Rs)`        | byte load, sign-extended                         |
| `LBU`                      | byte load, zero-extended                         |
| `LH`                       | halfword load, sign-extended                     |
| `LHU`                      | halfword load, zero-extended                     |
| `LW`                       | word load                                        |
| `SB`, `SH`, `SW`           | byte, halfword, and word stores                  |

The effective address is `Rs + sign_extend(offset)`. Halfword and word
addresses must be naturally aligned; misalignment raises a synchronous
trap before the access proceeds. The address is translated through the
current task's page table; a TLB miss or a permission failure raises a
synchronous trap routed to firmware.

Loads exhibit a one-cycle delay between the load and the use of its
result. The reference implementation does not interlock on this hazard;
the value of `Rt` in the cycle following the load is undefined if read
by the immediately succeeding instruction. The compiler is responsible
for filling the load delay slot with an unrelated instruction or, in
the absence of one, a `NOP` (canonically encoded as `SLL R0, R0, 0`).

## 7. Branches and Jumps

| Mnemonic                   | Effect                                           |
|----------------------------|--------------------------------------------------|
| `BEQ Rs, Rt, label`        | branch if `Rs == Rt`                             |
| `BNE Rs, Rt, label`        | branch if `Rs ≠ Rt`                              |
| `BLEZ Rs, label`           | branch if `Rs ≤ 0` (signed)                      |
| `BGTZ Rs, label`           | branch if `Rs > 0`                               |
| `BLTZ Rs, label`           | branch if `Rs < 0`                               |
| `BGEZ Rs, label`           | branch if `Rs ≥ 0`                               |
| `BLTZAL Rs, label`         | `R31 ← PC+8`; branch if `Rs < 0`                 |
| `BGEZAL Rs, label`         | `R31 ← PC+8`; branch if `Rs ≥ 0`                 |
| `J target`                 | unconditional jump                               |
| `JAL target`               | `R31 ← PC+8`; jump to `target`                   |
| `JR Rs`                    | jump to `Rs`                                     |
| `JALR Rd, Rs`              | `Rd ← PC+8`; jump to `Rs`                        |

`BLTZAL` and `BGEZAL` write the link register unconditionally and
branch only if the condition holds; they are the architectural basis
for PC-relative subroutine calls when `JAL`'s 256-megabyte reach is
inadequate or undesirable.

All branches and jumps have a one-instruction delay slot: the
instruction immediately following the branch executes regardless of
whether the branch is taken. Code that cannot find a useful instruction
to fill the slot inserts a `NOP`.

The branch displacement is the 16-bit `offset` field shifted left by
two and added to the address of the delay-slot instruction. The reach
of a conditional branch is therefore ±128 KB; `J` and `JAL` reach a
256-megabyte region within the current address space.

## 8. Operations on Object Registers

The following operations move and inspect the contents of object
registers without dereferencing them. They do not access memory through
the object's storage; they read only the fields of the reference itself
or the cached descriptor on the issuing processor. None of them
requires the object to actually exist.

| Mnemonic               | Effect                                              |
|------------------------|-----------------------------------------------------|
| `OMOV Od, Os`          | `Od ← Os`                                           |
| `ONULL Od`             | `Od ← null`                                         |
| `OEQ Rd, Os, Ot`       | `Rd ← 1` if `Os` and `Ot` denote the same object and generation, else `0` |
| `OISN Rd, Os`          | `Rd ← 1` if `Os` is the null reference, else `0`    |
| `OLEN Rd, Os`          | `Rd ← length(Os)` in bytes                          |
| `OTAG Rd, Os`          | `Rd ← type_tag(Os)`, zero-extended                  |
| `OHOME Rd, Os`         | `Rd ← home_processor_id(Os)`                        |
| `OCAP Rd, Os`          | `Rd ← capability_bits(Os)`                          |

`OEQ` and `OISN` do not trap on null operands and may be used to test
references unconditionally. The four inspection operations `OLEN`,
`OTAG`, `OHOME`, `OCAP` raise a `null-dereference` trap if `Os` is the
null reference; the descriptor lookup is otherwise free, completing in
the same cycle as the comparison or arithmetic that consumes the
result. A reference whose generation does not match the table entry
raises `stale-reference` instead.

The capability bits, the home processor identifier, and the type tag
are *readable* by user code but not modifiable. There is no instruction
in this volume by which a user- or supervisor-mode program may
construct a new object reference, alter the capability bits of an
existing one, change the home processor of an object, or mutate any
other field of the descriptor. Such operations are reached only through
`CALL` to the appropriate firmware primitive.

A ninth operation in this same encoding family takes no operands and
introduces no new register dependency:

| Mnemonic | Effect                                                           |
|----------|------------------------------------------------------------------|
| `OFENCE` | Order all preceding object-system memory accesses (`OL*`/`OS*`/`OREFLD`/`OREFST`) before all subsequent ones, and likewise order them with respect to mapped-page accesses (`LB`/`SW`/etc.) targeting the same underlying storage. |

`OFENCE` is the architecture's only memory-ordering primitive. The
issue arises because object-register access and ordinary mapped-page
access reach storage by different paths — the descriptor cache versus
the TLB — and an implementation may, in principle, reorder operations
between the two paths when no architectural dependency exists. Code
that mixes the two and depends on a specific ordering must interpose
an `OFENCE`. A single-issue in-order implementation may retire
`OFENCE` as a no-op; a more aggressive implementation must drain its
out-of-order queues across the boundary.

## 9. Loads and Stores Through Object Registers

| Mnemonic                       | Effect                                       |
|--------------------------------|----------------------------------------------|
| `OLB Rt, offset(Os)`           | byte load through object register            |
| `OLBU`, `OLH`, `OLHU`, `OLW`   | as for general-register loads                |
| `OSB Rt, offset(Os)`           | byte store through object register           |
| `OSH`, `OSW`                   | halfword and word stores                     |

The access covers `width` bytes starting at byte `offset` of the
object referenced by `Os`. Three checks are performed in parallel with
effective-address computation:

1. **Validity.** The reference must denote a live object: the
   generation counter recorded in the reference must match the
   generation in the object table. A mismatch raises
   `stale-reference`.
2. **Bounds.** Both `offset` and `offset + width` must lie within
   `[0, length(Os))`. A violation raises `bounds-violation`.
3. **Capability.** The capability bits of the reference must include
   `read` for a load and `write` for a store. A failure raises
   `capability-violation`.

Any failed check raises a synchronous trap before the memory access is
issued. The faulting instruction is precise and the architectural state
is exactly that of the cycle preceding it.

If the home processor of the object is the issuing processor, the
access proceeds against local memory through the cache and memory
hierarchy in the usual way. If the home is another processor, the
access is packaged as a crossbar message, routed to the home, executed
there against local memory, and the response routed back; the issuing
instruction stalls until the response returns. The architectural
behaviour is identical in both cases — only the latency differs.

There is no architectural ordering guarantee between an object-register
access and an ordinary load or store directed at the same byte through
a mapping installed by firmware. Code that requires such ordering must
either interpose an explicit firmware call or use the `OFENCE`
instruction (`OBJECT` function code `0x8`), which orders all
preceding object-register accesses before all subsequent ones —
including those reaching the same physical storage through a mapping
installed by firmware.

Object-register loads, like general-register loads, expose a one-cycle
load-use delay to the compiler.

## 10. Loads and Stores of References

| Mnemonic                       | Effect                                       |
|--------------------------------|----------------------------------------------|
| `OREFLD Od, offset(Os)`        | load a 64-bit object reference from object storage |
| `OREFST Od, offset(Os)`        | store a 64-bit object reference into object storage |

These two instructions are the architecture's solution to a problem
the rest of the spec deliberately created: the prohibition on storing
object references through general-register `SB`/`SH`/`SW` (which would
let user code freely manufacture references by writing arbitrary bit
patterns and reloading them as references). `OREFLD`/`OREFST` provide
a path for references to enter and leave object memory, but only
through *OR-typed storage* — storage whose descriptor carries the
`OBJSTORE` flag (Volume III Section 3.3). On such storage:

1. Integer `OL*`/`OS*` instructions trap with `capability-violation`.
2. `OREFLD`/`OREFST` succeed if the reference, generation, bounds, and
   capability checks of Section 9 all pass, and additionally the
   offset is 8-byte aligned.

Conversely, `OREFLD`/`OREFST` on byte-typed storage (the common case)
trap with `capability-violation`.

The two paths together preserve the capability invariant: bits that
sit in OR-typed storage were placed there by some prior `OREFST` of
a real reference held in an object register. They cannot be observed
or reconstructed as integers (the integer access path traps), and
they cannot be written from arbitrary bit patterns (the byte-store
path also traps). The set of derivable references therefore remains
exactly the set the firmware has minted, precisely as Volume III
Section 5 requires.

`OREFLD`/`OREFST` are the natural target instructions for compiler
spill/reload of object registers, for reference fields embedded in
heap structures, and for handler state passed between dispatches
through a service object's storage. Their encoding is given in
Volume III's CONTRACT addendum and in Volume V Section 2.6.

The 8-byte alignment requirement is enforced as
`address-misaligned-d`. The remaining fault conditions match Section 9
exactly.

## 11. The SEND Instruction

```
SEND Os
```

`SEND` transmits a message to the home processor of the object
referenced by `Os`. The message comprises:

- the recipient object reference (`Os`);
- the contents of `R4`–`R7` as a four-word integer payload;
- the contents of `O1`–`O4` as a four-reference object payload.

The capability bits of `Os` must include `send`; otherwise the
instruction raises `capability-violation`. The reference must be live;
a stale reference raises `stale-reference`. A null reference in `Os`
raises `null-dereference`.

`SEND` is asynchronous. The instruction completes as soon as the
crossbar accepts the message; it does not wait for the message to be
delivered to a handler, and it produces no architectural reply. If the
crossbar's outbound buffer for the destination port is full, the
processor stalls until space is available, or — if the implementation
elects to do so — raises `send-buffer-overflow` to firmware, which may
then queue the message in software.

On the receiving processor, the firmware-installed handler for the
target object is dispatched as if by an ordinary procedure call, with
the integer and object payloads delivered in the same registers used to
construct the message. The handler returns to firmware by `ERET`. If no
handler is installed for the target object, the message is dropped and
firmware is notified at its discretion.

`SEND` is the architectural primitive on which the system's higher-
level communication patterns are built. Synchronous remote procedure
call, actor-style message dispatch, and the rendezvous and broadcast
patterns of the firmware itself all reduce to a sequence of `SEND`s
combined with software-managed continuations and replies. A reply
capability is itself an object reference passed in `O1`–`O4`, on which
the recipient may in turn `SEND`.

## 12. The CALL Instruction

```
CALL #imm26
```

`CALL` traps the current task into firmware mode at the firmware entry
vector for primitive number `imm26`. The architectural effect is:

1. `EPC` receives the address of the instruction following `CALL`.
2. The current privilege mode is recorded in the status register and
   the mode is changed to firmware.
3. Control transfers to the firmware entry vector indexed by `imm26`.

`CALL` does not have a branch delay slot. The instruction immediately
following a `CALL` is the next user-visible instruction to execute on
return from firmware; firmware is responsible for any pipeline cleanup
required. This exception to the delay-slot convention is justified by
the infrequency of `CALL` relative to ordinary control flow and by the
substantial simplification it offers to firmware entry sequencing.

Arguments to a primitive are passed in `R4`–`R7` and `O1`–`O4`,
following the standard calling convention; return values are returned
in `R2`–`R3` and `O1`. Firmware preserves all caller-saved registers
across the call. From the caller's perspective, `CALL` therefore
behaves as an opaque procedure call with implementation-defined cost.

The set of valid primitive numbers, the meaning of each, and the trap
conditions raised on invalid arguments are specified in Volume VI.
A primitive number outside the range defined by the firmware
implementation raises `reserved-call`, which firmware in turn handles
by terminating the offending task or returning an error code at its
discretion.

## 13. Privileged Instructions

The following instructions are valid only in supervisor or firmware
mode. Execution in user mode raises `privileged-instruction`.

| Mnemonic           | Mode    | Effect                                      |
|--------------------|---------|---------------------------------------------|
| `LCTRL Rd, ctrl`   | sv / fw | read control register `ctrl` into `Rd`      |
| `SCTRL ctrl, Rs`   | sv / fw | write `Rs` into control register `ctrl`     |
| `ERET`             | sv / fw | return from exception or `CALL`             |
| `WAIT`             | sv / fw | halt processor until next interrupt         |
| `TLBP`             | fw only | probe TLB for entry matching `BadVAddr`     |
| `TLBR`             | fw only | read TLB entry indexed by `Index`           |
| `TLBWI`            | fw only | write the indexed TLB entry                 |
| `TLBWR`            | fw only | write a TLB entry chosen at random          |

`LCTRL` and `SCTRL` access a numbered set of control registers
including the status register, the exception program counter, the
exception cause, the faulting address, and the page-table base. The
detailed register set is enumerated in Volume V; the firmware-only
registers controlling the object table, the crossbar routing, and the
processor identification are not visible to supervisor-mode `LCTRL`.
A supervisor-mode `LCTRL` or `SCTRL` of a firmware-only control
register raises `privileged-instruction` rather than reading as zero.

`ERET` returns from the most recent exception or `CALL`: it restores
the saved privilege mode and resumes execution at `EPC`. It takes no
operand.

The TLB-management instructions are not normally invoked by hand-
written code; firmware uses them in the trap handlers responsible for
maintaining per-task page tables.

### 13.1 Encoding

The privileged instructions share major opcode `SYSTEM = 0x10` and are
sub-decoded by the `rs` field, in the COP0-style layout familiar to
implementations of MIPS-derived ISAs. The control-register selector for
`LCTRL` / `SCTRL` is carried in the `rd` field; the `funct` field
selects among the operand-less `CO` operations.

| `op`    | `rs`    | `funct` | Mnemonic           |
|---------|---------|---------|--------------------|
| `0x10`  | `0x00`  | —       | `LCTRL Rt, ctrl(Rd)` |
| `0x10`  | `0x04`  | —       | `SCTRL ctrl(Rd), Rt` |
| `0x10`  | `0x10`  | `0x01`  | `TLBR`             |
| `0x10`  | `0x10`  | `0x02`  | `TLBWI`            |
| `0x10`  | `0x10`  | `0x06`  | `TLBWR`            |
| `0x10`  | `0x10`  | `0x08`  | `TLBP`             |
| `0x10`  | `0x10`  | `0x18`  | `ERET`             |
| `0x10`  | `0x10`  | `0x20`  | `WAIT`             |

All other `(rs, funct)` combinations under `op = 0x10` are reserved and
raise `reserved-instruction`. The privilege-mode check is applied
before reserved-encoding decoding: an `op = 0x10` instruction issued in
user mode raises `privileged-instruction` regardless of its sub-fields.

## 14. Traps, Exceptions, and the Restart Model

All exceptions on Object RISC are *precise*: when a trap handler is
entered, every instruction earlier in program order than the faulting
instruction has completed, and no instruction later in program order
has any architectural effect. The address of the faulting instruction
is captured in `EPC`; the cause is captured in `Cause`. After
servicing the exception, firmware (or supervisor code, for the small
number of supervisor-handled exceptions) returns by `ERET`.

The architecturally defined exceptions are:

| Cause   | Name                       | Description                           |
|---------|----------------------------|---------------------------------------|
| `0x00`  | `reset`                    | hardware reset                        |
| `0x01`  | `external-interrupt`       | masked by the status register         |
| `0x02`  | `bus-error-i`              | instruction fetch failure             |
| `0x03`  | `bus-error-d`              | data access failure                   |
| `0x04`  | `address-misaligned-i`     | misaligned program counter            |
| `0x05`  | `address-misaligned-d`     | misaligned data access                |
| `0x06`  | `tlb-miss-i`               | TLB miss on instruction fetch         |
| `0x07`  | `tlb-miss-d`               | TLB miss on data access               |
| `0x08`  | `page-permission`          | TLB hit but permission denied         |
| `0x09`  | `arithmetic-overflow`      | trapping `ADD`, `SUB`, `ADDI`         |
| `0x0a`  | `reserved-instruction`     | undecoded major opcode or `funct`     |
| `0x0b`  | `privileged-instruction`   | privileged op in user mode            |
| `0x0c`  | `breakpoint`               | reserved opcode for debuggers         |
| `0x10`  | `null-dereference`         | use of `O0` where forbidden           |
| `0x11`  | `stale-reference`          | generation mismatch                   |
| `0x12`  | `bounds-violation`         | object-relative offset out of range   |
| `0x13`  | `capability-violation`     | required capability bit absent        |
| `0x14`  | `send-buffer-overflow`     | crossbar outbound buffer full         |
| `0x20`  | `firmware-call`            | execution of `CALL`                   |
| `0x21`  | `reserved-call`            | `CALL` with primitive number out of range |

The object-system traps `0x10`–`0x14` are routed to firmware regardless
of the privilege mode at which the offending instruction was issued.
Supervisor code that wishes to handle them on behalf of its tasks may
do so by registering with firmware through the appropriate primitive.

External interrupts are masked individually and as a class through the
status register. Their priority, numbering, and routing across the
crossbar are specified in Volume IV.

A trap taken on an instruction in a branch delay slot records the
address of the *branch* in `EPC` and sets the `BD` bit in the cause
register. `ERET` resuming from such a trap re-executes the branch,
which is the only way to ensure that the branch's effect on control
flow is correctly preserved.

## 15. Reserved Encodings

This revision allocates thirty-six of the sixty-four major opcodes
(adding `SYSTEM` at `0x10` for the privileged-instruction group, which
narrows the floating-point reservation to `0x11`–`0x1F`), twenty-two of
the sixty-four `SPECIAL` function codes, and nine of the sixteen
`OBJECT` function codes (eight inspection/movement operations plus
`OFENCE` at funct `0x8`). Every unallocated encoding raises
`reserved-instruction` when executed.

The following ranges are reserved for anticipated extensions; conforming
implementations shall not allocate them to local additions:

- Major opcodes `0x11`–`0x1F` — floating-point and other coprocessor
  extensions, to be defined by a future revision.
- Major opcode `0x3E` — reserved for a future vector or wide-arithmetic
  extension.
- Major opcode `0x3F` — implementation-specific. Code using this
  opcode is not portable.
- `SPECIAL` function codes `0x30`–`0x3F` — reserved.
- `OBJECT` function codes `0x9`–`0xF` — reserved for further object-
  system primitives.

## Appendix A — Major Opcode Map

The 64 major opcode slots are presently allocated as follows. `—`
denotes a reserved encoding that raises `reserved-instruction`.

| Opcode   | Mnemonic   | Opcode   | Mnemonic   |
|----------|------------|----------|------------|
| `0x00`   | `SPECIAL`  | `0x20`   | `LB`       |
| `0x01`   | `REGIMM`   | `0x21`   | `LH`       |
| `0x02`   | `J`        | `0x22`   | —          |
| `0x03`   | `JAL`      | `0x23`   | `LW`       |
| `0x04`   | `BEQ`      | `0x24`   | `LBU`      |
| `0x05`   | `BNE`      | `0x25`   | `LHU`      |
| `0x06`   | `BLEZ`     | `0x26`   | —          |
| `0x07`   | `BGTZ`     | `0x27`   | —          |
| `0x08`   | `ADDI`     | `0x28`   | `SB`       |
| `0x09`   | `ADDIU`    | `0x29`   | `SH`       |
| `0x0A`   | `SLTI`     | `0x2A`   | —          |
| `0x0B`   | `SLTIU`    | `0x2B`   | `SW`       |
| `0x0C`   | `ANDI`     | `0x2C`–`0x2F` | —     |
| `0x0D`   | `ORI`      | `0x30`   | `OBJECT`   |
| `0x0E`   | `XORI`     | `0x31`   | `OLB`      |
| `0x0F`   | `LUI`      | `0x32`   | `OLH`      |
| `0x10`   | `SYSTEM`   | `0x33`   | `OLW`      |
| `0x11`–`0x1F` | reserved (FP) | `0x34` | `OLBU` |
|          |            | `0x35`   | `OLHU`     |
|          |            | `0x36`   | `OREFLD`   |
|          |            | `0x37`   | `OREFST`   |
|          |            | `0x38`   | `OSB`      |
|          |            | `0x39`   | `OSH`      |
|          |            | `0x3A`   | —          |
|          |            | `0x3B`   | `OSW`      |
|          |            | `0x3C`   | `SEND`     |
|          |            | `0x3D`   | `CALL`     |
|          |            | `0x3E`   | reserved (vec) |
|          |            | `0x3F`   | implementation |

The `REGIMM` major opcode at `0x01` is decoded by the contents of the
`rt` field, encoding the conditional branches `BLTZ`, `BGEZ`, `BLTZAL`,
and `BGEZAL`. The `SPECIAL` and `OBJECT` major opcodes are decoded by
their `funct` fields as described in Sections 5 and 8 respectively.

## Appendix B — Trap Vector Layout

Each architectural exception has a fixed entry point in firmware,
expressed as an offset from the firmware vector base register
(`VECBASE`, a firmware-only control register). The offsets are
sixty-four bytes apart, sufficient for a short trampoline at each
entry; firmware that requires more space at a particular vector branches
out to a longer handler from within the trampoline.

| Cause   | Offset      | Cause   | Offset      |
|---------|-------------|---------|-------------|
| `0x00`  | `0x000`     | `0x10`  | `0x400`     |
| `0x01`  | `0x040`     | `0x11`  | `0x440`     |
| `0x02`  | `0x080`     | `0x12`  | `0x480`     |
| `0x03`  | `0x0C0`     | `0x13`  | `0x4C0`     |
| `0x04`  | `0x100`     | `0x14`  | `0x500`     |
| `0x05`  | `0x140`     | `0x20`  | `0x800`     |
| `0x06`  | `0x180`     | `0x21`  | `0x840`     |
| `0x07`  | `0x1C0`     |         |             |
| `0x08`  | `0x200`     |         |             |
| `0x09`  | `0x240`     |         |             |
| `0x0a`  | `0x280`     |         |             |
| `0x0b`  | `0x2C0`     |         |             |
| `0x0c`  | `0x300`     |         |             |

The reset vector at offset `0x000` is entered with the processor in
firmware mode and all interrupts masked. Firmware is responsible for
initializing the remaining control registers, the object descriptor
cache, and the connection to the crossbar before transferring control
to the supervisor.

— *The Object RISC Architecture Group, 1986*
