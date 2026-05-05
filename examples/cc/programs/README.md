# examples/cc/programs

Sample guests for the shell's `run` command. Each `.c` here gets
built to a same-named `.orx` automatically by `run_shell.sh` (or
manually via `bash build-one.sh foo.c foo.orx`).

Inside the shell:

```
/> ls /programs
build-one.sh
count.c
count.orx
exit42.c
exit42.orx
hello.c
hello.orx
README.md
/> run /programs/hello.orx
hello from a child task
[exited 0]
/> run /programs/count.orx
1
2
3
...
10
[exited 0]
/> run /programs/exit42.orx
about to exit 42
[exited 42]
```

`hello`, `count`, and `exit42` use `print_str` / `print_int`, which
go through firmware `ConsoleWrite` to host stdout (i.e., the
terminal where you launched `run_shell.sh` — not the Tk
oriscterm window). That's the right output channel for now: the
guest can't safely use `term_print` while the shell is running on
the same CPU, since `term_init` would compete with the shell for
keyboard subscriptions. Phase 31 will fix that.

To add a new program: drop `foo.c` here, re-run `run_shell.sh`,
then `run /programs/foo.orx` from inside.
