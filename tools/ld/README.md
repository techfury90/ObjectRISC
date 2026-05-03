# orld — Object RISC linker

Combines one or more `.oro` object files (produced by `asmorisc -r`)
into a single executable `.orx`. Concatenates text and data sections,
resolves cross-file global symbols, applies the relocation records
each input carries, and writes a final `.orx` with the right entry
offset.

`asmorisc` without `-r` is still a fully working "concatenate +
emit `.orx`" assembler — adding the linker doesn't take that mode
away. The new pipeline is opt-in.

## Usage

    tools/ld/orld -o program.orx crt0.oro lib.oro program.oro
    tools/ld/orld --entry main -o program.orx lib.oro program.oro
    tools/ld/orld --map link.map -o program.orx *.oro

`--entry SYM` chooses the entry point; defaults to `_start` (the
symbol `tools/cc/arch/orisc/crt0.s` defines). `--map FILE` writes a
section/symbol layout map for debugging.

## The pipeline

```
foo.s ──asmorisc -r──▶ foo.oro ─┐
                                ├──orld──▶ program.orx
bar.s ──asmorisc -r──▶ bar.oro ─┘
```

Each `.s` translation unit is assembled independently to a
relocatable `.oro`. Local symbols (pcc-style `L\d+` temporaries by
default; explicitly `.local NAME` otherwise) stay file-private —
the linker's symbol-merge step never sees them. Global symbols
(everything else, or explicitly `.global NAME`) get merged across
inputs; duplicates are an error.

The C demo runner [`examples/cc/run_c.sh`](../../examples/cc/run_c.sh)
now uses this pipeline. The older single-call asmorisc path with the
`sed 's/L\([0-9]\+\)/LP\1/'` per-unit label-mangling hack — needed
because the global symbol table couldn't tell two `L1`s apart — is
gone.

## Symbol scoping

Default rule: a label name matching `L\d+` is **local**; anything
else is **global**. Override with directives:

    .global main, helper           ; export these
    .local  cw_data, cw_stack      ; keep these file-private

`.global` and `.local` accept one symbol or a comma-separated list.
`.globl` is accepted as an alias for `.global`.

## Relocations

Six relocation types, all applied at link time per the bit layouts
in [`CONTRACT.md`](../../CONTRACT.md) §1.1:

| Code   | Name              | Where it appears                                         |
|--------|-------------------|----------------------------------------------------------|
| `0x01` | `R_ORISC_ABS32`   | `.word symbol` — patches the full 32-bit word            |
| `0x02` | `R_ORISC_HI16`    | `lui rd, hi(label)` — first half of `la`                 |
| `0x03` | `R_ORISC_LO16`    | `ori rd, rd, lo(label)` — second half of `la`            |
| `0x04` | `R_ORISC_BRANCH16`| Long-range branches with undefined targets (rare)        |
| `0x05` | `R_ORISC_J26`     | `j label` and `jal label` to global / cross-file targets |

Branches between two locally-defined labels in the same `.oro`
don't generate relocations — their PC-relative displacements are
invariant under section moves, so `asmorisc -r` still resolves them
at pass 2.

## Tests

A small per-feature test suite lives in
[`tools/ld/tests/`](tests). Each test is a self-contained `.sh`
that builds its own `.oro` files in a tempdir, links, and asserts
on either the simulator's exit code or `orld`'s error message.

Run them all:

    bash tools/ld/tests/run-tests.sh

The cases:

- `01_basic_two_files` — cross-file `jal` (J26 reloc).
- `02_la_across_files` — cross-file `la rd, label` (HI16/LO16 pair).
- `03_local_label_scoping` — both files define `L1`; per-`.oro` local
  scoping keeps them apart.
- `04_undefined_symbol` — linker errors on unresolved external.
- `05_duplicate_global` — linker errors on `helper` defined twice.
- `06_explicit_local` — `.local helper` makes the same name file-private
  in both files; both link cleanly.

## Files

| File                       | Role                                           |
|----------------------------|------------------------------------------------|
| `orld`                     | The linker                                     |
| `tests/run-tests.sh`       | Runs every `*.sh` test in this directory       |
| `tests/0*.sh`              | Per-feature test cases                         |
