# Ouroboros — the OS layer

The OS that grew on top of Object RISC. Named for the
snake-eating-its-tail symbolism: the OS runs on the architecture
whose toolchain self-hosts; traps return where they came from via
`ERET`; and the VM/CMS analogy that motivates the design (CP =
firmware, CMS = Ouroboros) is itself a recursive shape.

## What's in here

- [`shell.c`](shell.c) — the shell. Not just an interactive
  front-end: cmd_run loads a `.orx` from hostfsd and `TaskCreate`s
  it as a child task on the same CPU. The shell IS the supervisor.
  Commands: `help`, `cat`, `more`, `ls`, `cd`, `pwd`, `echo`,
  `run` (sync or `&` for background), `wait`, `kill`, `view`,
  `cycles`, `time`, `exit`. Up/down arrow history, backspace with
  visual undo, `--More--` paginator, paths resolved against cwd,
  build-date banner shifted back 40 years.

- [`programs/`](programs/) — guest programs the shell can `run`.
  Each is a small standalone C program linking against
  `liborisc.ora`. Ships with: `hello.c`, `hello_term.c`, `count.c`,
  `exit42.c`, `dhry.c` (Dhrystone v2.1), `edit.c` (full-screen
  text editor with focus-switch).

## Boot

```sh
make boot      # builds the shell, builds every program, starts Ouroboros
```

`make boot` runs [`scripts/boot.sh`](../scripts/boot.sh) which
spawns oriscbar (crossbar), oriscterm (Tk window, pid 16),
hostfsd (host filesystem, pid 17, jailed to repo root), and the
shell CPU (pid 0, the leader).

The boot script computes today-minus-40 and rebuilds the shell
with that banner each time, so the year in the prompt is current
even after a make. The hostfsd jail has `/programs/` symlinked to
`build/programs/` — you can `run /programs/hello.orx` from the
shell immediately.

## Adding a program

Drop a `.c` file in [`programs/`](programs/) and re-run `make`.
The Makefile has a uniform `%.c → %.orx` pattern rule, no
per-program plumbing. From inside the shell after the next boot,
your program is at `/programs/<name>.orx`.

Linkage: every program implicitly gets `crt0.s` + `console_io.s`
+ `liborisc.ora`. `crt0.s` provides `_start`, calls `main`, and
`TaskExit`s with `main`'s R2 as the exit code.

## Architecture phases

[`docs/HISTORY.md`](../docs/HISTORY.md) records Ouroboros's
growth, currently through phase 41d:

- **24** — privilege modes, trap delivery
- **25** — supervisor handler installation
- **26** — tasks, scheduler, per-task address spaces
- **27** — timer interrupts
- **28** — multi-child task API in libc
- **29** — `.orx` loader (`orx_run` / `orx_spawn`)
- **30** — shell-as-supervisor (cmd_run direct-spawns guests)
- **36** — preemption (timer-driven yield)
- **38–40** — interactive guests on shared keyboard, focus
  switching via F1
- **41** — argv plumbing (a → top half, b → buffer wiring + a
  TaskExit scheduler fix, c → unlimit cycles in --connect mode,
  d → cwd passthrough + targeted unsubscribe)
