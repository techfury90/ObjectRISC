# ouroboros/programs

Sample guests for the shell's `run` command. Each `.c` here gets
built to a same-named `.orx` under `build/programs/` by `make`.
`make boot` symlinks `build/programs/` into the hostfsd jail at
`/programs/`, so they're immediately runnable from inside the
shell.

```
/> ls /programs
build-one.sh
count.c       count.orx
dhry.c        dhry.orx
exit42.c      exit42.orx
hello.c       hello.orx
hello_term.c  hello_term.orx
README.md
/> run /programs/hello.orx
[exited 0]
/> run /programs/count.orx
[exited 0]
/> run /programs/exit42.orx
[exited 42]
/> run /programs/hello_term.orx
hello from inside the Tk window
[exited 0]
/> run /programs/hello_term.orx &
[bg task 0]
hello from inside the Tk window
/> wait 0
[task 0 exited 0]
/> run /programs/dhry.orx
Dhrystone Benchmark, Version 2.1 (Object RISC port)
Iterations: 5000
...
Cycles per iteration: 4067
  16 MHz: ~3934 dhry/s   = ~2.2 DMIPS
  20 MHz: ~4918 dhry/s   = ~2.7 DMIPS
[exited 0]
```

## Two output paths

Object RISC has two ways for a guest to produce output:

- **`print_str` / `print_int` / `print_hex`** (in `io.c`) lower
  to firmware `ConsoleWrite`, which writes to **host stdout** —
  the terminal you launched `run_shell.sh` from, not the Tk
  oriscterm window. `hello.c`, `count.c`, and `exit42.c` use this
  path; their output appears alongside the shell's startup
  messages, not interleaved with prompts.
- **`term_print*`** (in `term.c`) SENDs to the oriscterm console
  service (O5) and lands in the **Tk window** alongside the
  shell's prompts and exit markers. Requires the boot OPRs to be
  parked — call `term_print_only_init` first (Phase 31). Don't
  call the full `term_init` from a guest: it subscribes to the
  keyboard, which would compete with the shell on the same CPU
  for keystrokes. `hello_term.c` is the worked example.

## Backgrounding (`&`)

Append `&` to a `run` command to spawn the child without waiting:

```
/> run /programs/hello_term.orx &
[bg task 3]                              ← libc task table slot 3
hello from inside the Tk window          ← child's term_print
/> wait 3                                ← harvest
[task 3 exited 0]
```

The shell's `cmd_run` does one `task_yield` after `orx_spawn` so
short children get a quantum to run before the shell blocks on
the next keystroke. Long-running CPU-bound children don't starve
the shell — Phase 36's timer preemption fires every 5000 cycles
and yields back, so you can keep typing (and `dhry.orx` running
inside the shell is the canonical worked example).

If you forget to `wait`, the task descriptor sits in the libc
task table EXITED until the shell exits. No exit-code harvest,
but no harm.

## Adding a new program

Drop `foo.c` here, re-run `make` (the Makefile picks it up
automatically — there's a uniform `%.c → %.orx` pattern rule),
then from inside the shell: `run /programs/foo.orx`.

```sh
echo '#include "liborisc.h"
int main(void) { print_str("hi\n"); return 7; }
' > ouroboros/programs/hi.c
make programs
# `make boot` if not already running, then inside the shell:
#   /> run /programs/hi.orx
```
