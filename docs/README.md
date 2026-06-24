# Object RISC documentation

The architecture spec — seven volumes, plus the integration
contract, plus the project history.

## The seven volumes

| Volume | File                                                | Subject                              |
|--------|-----------------------------------------------------|--------------------------------------|
| I      | [`OVERVIEW.md`](OVERVIEW.md)                        | Architectural pitch and shape        |
| II     | [`INSTRUCTION_SET.md`](INSTRUCTION_SET.md)          | The ISA                              |
| III    | [`OBJECT_SYSTEM.md`](OBJECT_SYSTEM.md)              | References, descriptors, capabilities |
| IV     | [`INTERCONNECT_PROTOCOL.md`](INTERCONNECT_PROTOCOL.md) | Crossbar wire format                 |
| V      | [`REFERENCE_IMPLEMENTATION.md`](REFERENCE_IMPLEMENTATION.md) | OR-1000 / OR-XBAR-1 designs   |
| VI     | [`SYSTEM_FIRMWARE_INTERFACE.md`](SYSTEM_FIRMWARE_INTERFACE.md) | Firmware primitive ABI         |
| VII    | [`PROGRAMMING_PRACTICE.md`](PROGRAMMING_PRACTICE.md) | ABI, idioms, worked example          |

[`CONTRACT.md`](CONTRACT.md) is the integrator's contract: things
the architecture spec leaves to the implementation but that the
toolchain commits to — `.orx` binary format, loader's initial task
state, host-side semantics of each firmware primitive.

[`HISTORY.md`](HISTORY.md) is the project's development log,
phase by phase.

## Subsystem guides

Focused, current-architecture walkthroughs of individual subsystems
(as opposed to the architecture spec above):

- [`wm-terminal-overview.md`](wm-terminal-overview.md) — the Ouroboros
  window-manager + terminal + graphics stack: how a keystroke reaches a
  program, how a program draws to the screen, and how it all boots
  (Phase 60: the WM owns the framebuffer + input and *is* the terminal).

## Combined PDFs

[`build_pdf.py`](build_pdf.py) merges all seven volumes into a
single markdown (`OBJECT_RISC.md`) ready for pandoc:

```sh
cd docs
python3 build_pdf.py             # produces docs/OBJECT_RISC.md
pandoc OBJECT_RISC.md -o ObjectRISC.pdf \
    --include-in-header=preamble.tex \
    --pdf-engine=xelatex
```

Two pre-built PDFs ship for convenience:

- [`ObjectRISC.pdf`](ObjectRISC.pdf) — Computer Modern, 72 pp
- [`ObjectRISC-Palatino.pdf`](ObjectRISC-Palatino.pdf) — TeX Gyre Pagella, 79 pp
