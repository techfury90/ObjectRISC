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
- [`OBJECT_API.md`](OBJECT_API.md) — the libc handle-based object API
  (`obj.{h,c}`): why C programs hold opaque `obj_t` handles instead of
  capabilities, the 8-slot handle table in the O12 OBJSTORE, the
  `obj_send_bytes` data keystone, and the client-migration pattern.
- [`supervisor-overview.md`](supervisor-overview.md) — the per-CPU
  init / session-manager process: the leader/worker model, the
  spawn-service RPC, cross-CPU spawn relay + round-robin, and hot-attach.
- [`directory-hostfs-overview.md`](directory-hostfs-overview.md) — the
  name → reference namespace (`oriscdir` + `dir.c`): the dir-walk /
  register / inline-register protocol, the `DIR_RESULT_SLOT` hand-off,
  and how `hostfsd` mounts onto the tree.

The boot path itself (`scripts/boot.sh` → `tools/oriscrun` → `simorisc`:
the crossbar, process/CPU launch order, the boot-OR register contract) is
covered in [`wm-terminal-overview.md` §3](wm-terminal-overview.md); a
dedicated boot guide is a possible follow-up.

## Combined PDFs

[`build_pdf.py`](build_pdf.py) merges all seven volumes into a
single markdown (`OBJECT_RISC.md`); pandoc + xelatex then typeset it
into a book with a title page, a hyperlinked TOC, and one chapter per
volume:

```sh
cd docs
python3 build_pdf.py             # produces docs/OBJECT_RISC.md
pandoc OBJECT_RISC.md -o ObjectRISC.pdf \
    --include-in-header=preamble.tex --pdf-engine=xelatex \
    --top-level-division=chapter -V documentclass=report \
    --toc --toc-depth=2 -V geometry:margin=1in \
    -V title="The Object RISC Architecture" \
    -V subtitle="Architecture Reference, Revision 0.1" \
    -V author="The Object RISC Architecture Group" \
    -V date="1986"
```

The `--top-level-division=chapter`, `documentclass=report`, title
metadata, and `geometry` flags are all required: `preamble.tex`
redefines the chapter heading (which only exists in a chapter-bearing
class), and the title/TOC/margins are what reproduce the shipped layout.

Two pre-built PDFs ship for convenience:

- [`ObjectRISC.pdf`](ObjectRISC.pdf) — Computer Modern, 72 pp (the
  command above).
- [`ObjectRISC-Palatino.pdf`](ObjectRISC-Palatino.pdf) — the academic
  variant: the same command plus `-V mainfont="TeX Gyre Pagella"
  -V mainfontoptions="Numbers=OldStyle"`. Regenerated less often, so it
  may trail the Computer Modern build.
