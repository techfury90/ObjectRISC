# Object RISC simulator validation suite

Runs Object RISC assembly programs through the assembler and the
simulator and checks each program's outcome against in-source
expectations. Each `.s` file is one test; the runner discovers them
under `<category>/*.s`.

## Running

From the repo root:

```sh
python3 tools/sim/tests/validation/runner.py            # all categories
python3 tools/sim/tests/validation/runner.py integer    # substring filter
python3 tools/sim/tests/validation/runner.py -v         # verbose: show description on PASS
python3 tools/sim/tests/validation/runner.py --list     # list discovered tests
```

The runner exits non-zero if any test fails.

## Categories

| Dir              | What it covers                                          |
|------------------|---------------------------------------------------------|
| `01_integer/`    | ADD/SUB overflow vs ADDU/SUBU, ADDI sign-extend, SLT/SLTU, MULT/DIV/HI/LO, LUI+ORI, ANDI zero-extend |
| `02_logical/`    | AND/OR/XOR/NOR, SLL/SRL/SRA, variable shifts, R0 hardwiring |
| `03_memory/`     | LW/SW round-trip, LH/LHU/LB/LBU sign vs zero extension, big-endian byte order, misalignment traps |
| `04_control/`    | BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL, J/JAL/JR/JALR, the branch delay slot |
| `05_oreg/`       | OMOV/ONULL/OEQ/OISN/OLEN/OTAG/OHOME/OCAP, null-trap behavior |
| `06_omem/`       | OL*/OS* widths, bounds-violation, capability-violation, null-dereference |
| `07_firmware/`   | ConsoleWrite happy path + EFAULT/EINVAL, TaskExit code, ENOSYS for unknown CALL |
| `08_traps/`      | Precise EPC capture, reserved-instruction (raw `.word`), reserved OBJECT funct, unmapped-VA tlb-miss |
| `09_call/`       | No-delay-slot semantics, PC+4 return, max imm26, chained CALLs |
| `10_golden/`     | Iterative factorial, count loop, conditional, partial-string print, print-in-a-loop |

## Test format

Each `.s` file starts with a header of `; @key: value` directives.
Recognized keys:

| Directive                  | Meaning                                              |
|----------------------------|------------------------------------------------------|
| `@description: <text>`     | One-line description; shown in verbose output       |
| `@expect-exit: <int>`      | Required process exit code (0–255)                   |
| `@expect-stdout: "<text>"` | Required exact stdout (C escapes recognized)         |
| `@expect-stdout-hex: <hh>` | Required stdout as hex bytes (whitespace stripped)   |
| `@expect-trap: <name>`     | Required trap by architectural name (e.g. `arithmetic-overflow`) |
| `@expect-trap-pc: <addr>`  | Required architectural faulting PC                   |
| `@expect-stderr-contains: <substring>` | Required substring in stderr             |
| `@max-cycles: <int>`       | Cap on simulator cycles (default 100000)            |

A test must declare at least one of `@expect-exit`, `@expect-stdout`,
or `@expect-trap`. Multiple expectations all apply.

## Adding a test

1. Pick the right category dir (or add a new one as `NN_name/`).
2. Drop a new `NN_short_name.s` whose header documents the
   expectation, then write the program below.
3. Run `python3 tools/sim/tests/validation/runner.py <substring>` to
   verify.

The exit code is the most ergonomic way to report a value the test
"computes": `TaskExit` truncates `R4` to its low byte, so any 0–255
result lands in the process exit code.

## Test inventory

87 tests across 10 categories as of the latest run.
