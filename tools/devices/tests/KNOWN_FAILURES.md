# Known test failures & flakes — `tools/devices/tests/`

If a suite sweep shows one of the tests below red, **that is expected** —
do NOT treat it as a regression from your change, and do NOT try to "fix" it
unless you are *deliberately* reviving it. When in doubt, re-run the
suspected red on plain `main` first; if `main` is also red, it's this list,
not your diff.

There is no aggregate/gating runner — tests are individual `test_*.sh`. When
sweeping, wrap each with a per-test timeout (macOS has no `timeout(1)`):

```sh
perl -e 'alarm 120; exec @ARGV' bash tools/devices/tests/<t>.sh   # exit 142 = timed out
pkill -f simorisc                                                 # reap orphaned sims
```

## KNOWN-RED (deferred on purpose)

### `test_shell_edit`
The shell's `edit` builtin launches a **standalone editor** (`edit.orx`) that
subscribes to the keyboard itself, then an F1 focus-switch routes keystrokes
to it. That premise is **stale**: it boots neither a supervisor nor a WM, but
in the current architecture the editor is spawned via `sup_spawn_at` (needs a
supervisor) and input is owned by the WM — so the editor never comes up and
the inserted text never lands.

Reviving it = rebuild the topology the way `test_supervisor_run_at` and
`test_shell_logout` were modernized (#197 / #198): boot a supervisor with
**walk-don't-wire** service wiring (null `O5/O6/O7` so it walks `/sys/term/0`
for its console), let it spawn the shell, and drive the editor through the
fake terminal. Moderate effort; **not a quick win — don't get pulled into it
by accident.**

## REAL-TIME-LOAD FLAKY (green on a quiet box, red under load)

### `test_wm_boot`
The fake terminal injects keystrokes on a **fixed wall-clock schedule** while
`simorisc` runs unbounded; under host load the emulated CPU falls behind and
input lands outside the shell's input window → spurious FAIL. Proven
environmental (fails on `main` too under load). Real fix (deferred): poll for
a prompt/marker instead of a fixed `sleep`/`--delay`. Before blaming a diff,
re-run on `main` under the same load.

> `test_multiterminal` was formerly listed here as load-flaky, but its 240s
> "timeout" was not load — it was the same **wired-O5 co-resident trap** #197
> fixed for `run_at`: both CPUs wired `O5=terminal`, the supervisor read that
> as co-resident and tried to launch a WM that isn't in the jail, and the boot
> hung. Fixed by walk-don't-wire on both CPUs (each walks `/sys/term/<procid>`
> for its own terminal). Now green + stable (3/3 on a quiet box).

---
_Last reviewed 2026-07-01. Every other `test_*.sh` is green + fast on a quiet
box. The cross-CPU-spawn tests (`run_at`, `dynamic_cpu`, `logout`) were revived
in #197/#198; `test_supervisor`, `test_round_robin`, and `test_multiterminal`
were revived alongside a fake-terminal **SIGTERM fix** — a #197 regression that
had broken the 5 WM/graphics smoke tests (`wm/ptr/raster/vec/font_smoke`) by
treating the teardown-SIGTERM'd terminal (exit 143) as a failure. A fail-fast
guard makes a misconfigured terminal test abort in ~11s instead of hanging. See
the `[[device-test-suite-gotchas]]` memory for the deeper history._
