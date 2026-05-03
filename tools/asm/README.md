# asmorisc — Object RISC Assembler

`asmorisc` is the assembler half of the Object RISC toolchain. It
consumes one or more assembly source files and produces a single
`.orx` binary as specified in
[`/CONTRACT.md`](../../CONTRACT.md), which is loadable directly by the
companion simulator (`simorisc`).

For the full architecture, see Volumes I–VII at the repo root, in
particular Volume II (`INSTRUCTION_SET.md`) for instruction semantics
and Volume VII (`PROGRAMMING_PRACTICE.md`) for example programs.

## Building / Installing

Pure Python 3.10+, standard library only. No build step.

```sh
chmod +x tools/asm/asmorisc        # if not already executable
```

If your installation does not have an executable bit, run via
`python3 tools/asm/asmorisc ...`.

## Usage

```sh
# Assemble.  Default output is INPUT with .s replaced by .orx.
asmorisc examples/hello.s
asmorisc examples/hello.s -o build/hello.orx

# Multiple inputs are concatenated in order.
asmorisc lib.s main.s -o app.orx

# Decode a binary back to assembly (useful for round-trip checks).
asmorisc --disasm hello.orx

# Hex dump with one instruction per line annotated with mnemonic.
asmorisc --hex hello.orx
```

## Syntax overview

Full syntax is pinned in [`CONTRACT.md`](../../CONTRACT.md) Section 4.
At a glance:

* Comments begin with `;` and run to end of line.
* Identifiers are case-sensitive and match `[A-Za-z_.][A-Za-z0-9_.]*`.
* Numbers: `42`, `0xCAFE`, `0b1010`, `'A'`. Negatives prefix with `-`.
* Strings are double-quoted with C escapes `\n \t \r \\ \" \0 \xHH`.
* `label:` defines a symbol equal to the current section's byte offset.
* Register names: canonical `r0`–`r31`, `o0`–`o15`, plus all the
  ABI aliases (`zero`, `sp`, `fp`, `ra`, `a0`–`a3`, `t0`–`t12`,
  `s0`–`s7`, `v0`, `v1`, `at`, `null`).

### Directives

| Directive          | Effect                                    |
|--------------------|-------------------------------------------|
| `.text`            | switch to text section                    |
| `.data`            | switch to data section                    |
| `.entry <label>`   | set program entry point to `<label>`      |
| `.byte`, `.half`, `.word` | emit 1/2/4-byte values             |
| `.string "..."`    | emit raw bytes (no NUL)                   |
| `.asciz "..."`     | emit bytes plus a trailing `0x00`         |
| `.align N`         | align to a 2^N boundary with zero bytes   |
| `.skip N`          | emit N zero bytes                         |

### Pseudo-instructions

| Pseudo                | Expansion                                                |
|-----------------------|----------------------------------------------------------|
| `nop`                 | `sll r0, r0, 0`                                          |
| `move rd, rs`         | `addu rd, rs, r0`                                        |
| `b label`             | `beq r0, r0, label`                                      |
| `li rd, imm`          | one `addiu` if it fits in s16, else `lui` + `ori`        |
| `la rd, label`        | always `lui rd, hi(addr); ori rd, rd, lo(addr)`          |

### Encoding reference

The full encoding tables are in CONTRACT.md Section 5 and in Volume II
of the architecture spec. The dispatch tables in `asmorisc` map
directly onto those tables — search for `SPECIAL_FUNCT`, `MAJOR`,
`REGIMM_RT`, and `OBJECT_FUNCT`.

## Example

```sh
$ asmorisc examples/hello.s -o hello.orx
$ asmorisc --hex hello.orx
$ asmorisc --disasm hello.orx
```

## Tests

`tests/run-tests.sh` assembles each `tests/*.s` file and `diff`s the
resulting `.orx` against the matching `.expected` file. See
`tests/README` for what each test exercises.
