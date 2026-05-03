# simorisc — Object RISC ISA Simulator

A pure-Python (3.10+, stdlib only) instruction-set simulator for the
Object RISC architecture. Loads `.orx` binaries per `CONTRACT.md`, sets
up the initial task state, and executes Object RISC instructions with
the architectural semantics of Volume II.

## Usage

    tools/sim/simorisc <program.orx>
    tools/sim/simorisc --trace <program.orx>
    tools/sim/simorisc --max-cycles 50000 <program.orx>

`--trace` writes one line per retired instruction to stderr (so stdout
remains the program's output):

    0x00010000  omov o1, o3                       ; o1 = 0x00010003000100ff_...
    0x00010004  addiu r4, r0, 0
    0x00010008  addiu r5, r0, 14                  ; r5 = 0x0000000e
    0x0001000c  call #0x320                       ; r2 = 0x00000000, r3 = 0x0000000e

## Build & test

    sh tools/sim/tests/run-tests.sh

(or `chmod +x tools/sim/simorisc tools/sim/tests/run-tests.sh` once and
run them directly).

The runner regenerates `tests/hello-test.orx` from `build-hello-test.py`
(hand-encoded against the contract — independent of the assembler
agent's output) and runs `simorisc` on it.

## What's implemented

- All Volume II Sections 5-11 instructions: integer arithmetic
  (`ADD/ADDU/SUB/SUBU/AND/OR/XOR/NOR/SLL/SRL/SRA/SLLV/SRLV/SRAV/SLT/SLTU`,
  immediate forms, `LUI`, `MULT/MULTU/DIV/DIVU/MFHI/MFLO`); GPR loads
  and stores (`LB/LBU/LH/LHU/LW/SB/SH/SW`); branches and jumps
  (`BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL/BGEZAL/J/JAL/JR/JALR`) with
  one-instruction architectural delay slots; object register ops
  (`OMOV/ONULL/OEQ/OISN/OLEN/OTAG/OHOME/OCAP`); object loads/stores
  (`OL{B,BU,H,HU,W}/OS{B,H,W}`) with null/stale/bounds/capability
  checks; `SEND` (single-CPU: traces handler dispatch, no real delivery
  needed for hello world); `CALL`.
- The two firmware primitives required by the contract: `0x001
  TaskExit` and `0x320 ConsoleWrite`. **Every other primitive number
  returns `R2 = 4` (`ENOSYS`)** — including all the task management,
  object lifecycle, mapping, and time/clock primitives in Volume VI.
- Trap model: precise; on any architectural trap, the simulator prints
  the cause name and code, faulting PC, and a register-state slice,
  then exits non-zero. There is no firmware vector table — `CALL` goes
  directly to a Python primitive dispatcher.

## What's stubbed

- Privileged instructions (`LCTRL/SCTRL/ERET/WAIT/TLBP/TLBR/TLBWI/TLBWR`)
  decode to `reserved-instruction`. Hello world doesn't exercise them.
- The descriptor cache and TLB are not modeled as caches: every access
  walks the (very small) VA→object map and looks up the descriptor
  directly. The generation-counter coherence model (Volume III §4)
  means this is observably equivalent.
- No multiprocessor, crossbar, devices beyond stdout, demand paging,
  or hypervisor primitives.

## Design notes

- **Object table.** A flat Python `list[Optional[Descriptor]]` indexed by
  reference's `local table index` field. Generation is checked on every
  OL/OS, OLEN/OTAG/OHOME/OCAP, and SEND.
- **References.** Stored as raw 64-bit Python ints with the
  generation/home/index/caps fields packed per Volume III §2.1. Helpers
  `make_ref()` and `ref_*()` keep the bit layout in one place.
- **Memory translation.** A list of `(va_lo, va_hi, descriptor_idx,
  base_offset, prot)` tuples populated at load time by the contract's
  three `MapObject` calls. Linear scan on every access — fine at the
  scale of one or two mappings per program.
- **Branch delay slots.** Modeled with a `next_pc` register: `pc` is the
  current instruction, `next_pc` is what `pc` becomes after retirement.
  A branch updates `next_pc` to the branch target *after* swapping in
  the delay slot, so the slot always retires regardless of taken/not.
  `CALL` is special-cased to skip the delay slot per Volume II §11.
