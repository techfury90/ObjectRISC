# The Ouroboros Supervisor (`supervisor.c`)

*An architecture overview for new contributors.*

This document describes [`ouroboros/supervisor.c`](../ouroboros/supervisor.c), the
**init / session-manager process** that runs on every CPU in an Ouroboros boot. It
covers the leader/worker model, how a session is set up, the spawn-service RPC, the
cross-CPU spawn relay, and hot-attach.

It is a *current-architecture* document. The supervisor's **WM session
establishment** (binding console/keyboard/grid surfaces and handing them down the
`sysinit → login → shell` chain) is covered in
[`docs/wm-terminal-overview.md` §9](wm-terminal-overview.md) and is **not restated
here** — this guide cross-references it and focuses on the parts §9 doesn't cover:
the dispatch model and the multi-CPU spawn machinery. Every non-obvious claim cites
`file:line`.

> Like the WM, the supervisor carries a thick sediment of pre-Phase-60 comments
> referencing `oriscterm` and `/sys/term/<n>/*`. Where one contradicts the running
> WM-mediated boot, this document states the current reality.

---

## 1. One supervisor per CPU; leader vs worker

`main()` ([`supervisor.c:1788`](../ouroboros/supervisor.c#L1788)) runs on **every**
CPU. The first thing it computes is its role:

```c
int is_leader = (procid == 0);     /* supervisor.c:1829 */
```

- **The leader (PROCID 0)** drives all visible activity: it spawns the
  `sysinit → login → shell` chain, runs hot-attach, and reaps exited tasks.
- **A worker (PROCID ≠ 0)** sits in a dispatch loop servicing **relayed spawn
  requests** from the leader (and from its own local shell, in a multi-terminal
  boot). It does no hot-attach scan.

In a `make boot` system there are two supervisor CPUs — **pid 0 (leader)** and
**pid 1 (worker)** — alongside the two WM CPUs (pids 2–3); see
[`wm-terminal-overview.md` §3](wm-terminal-overview.md).

---

## 2. Boot sequence

`main()` runs, in order:

1. **Allocate the spawn-service mailbox.** `allocate_service_mailbox`
   ([`supervisor.c:215`](../ouroboros/supervisor.c#L215)) `ObjAlloc`s a 16-byte
   `TAG_SERVICE` object, attaches a receive queue of **depth 8** (`#0x203
   ReceiveQueueAttach`, [`supervisor.c:233-239`](../ouroboros/supervisor.c#L233)),
   and parks the full ref in `O9`. This is the object every spawn/shutdown/ps RPC
   targets.
2. **Install the child-`O8` override.** `install_child_o8_override`
   ([`supervisor.c:254`](../ouroboros/supervisor.c#L254)) derives an `R|S` sub-cap
   of the spawn mailbox into `ORX_SLOT_CHILD_O8`@560, so spawned children boot with
   `O8` = a cap to SEND spawn requests **back** to this supervisor.
3. **Adopt the directory.** The boot directory mailbox (boot `O8`, parked in
   `BOOT_PARENT_SLOT`@544 by `task_init`) is copied into `DIR_SLOT`@584, the slot
   [`dir.c`](../tools/cc/lib/dir.c) reads for every walk/register.
4. **Discover boot surfaces (fails under Phase 60).** It walks
   `/sys/term/<procid>/{console,keyboard,grid}` and `/sys/hostfsd/0`, OREFLDing each
   resolved ref into `O5/O6/O7/O10` **only on success**
   ([`supervisor.c:1880-1901`](../ouroboros/supervisor.c#L1880)). In a WM-mediated
   boot **nothing publishes `/sys/term/<n>/*`** (the WM owns input/output locally and
   `boot.sh` launches no terminal device), so these walks return `NOT_FOUND` and the
   OREFLDs are skipped — `O5/O6/O7` keep their (null) boot values. The `O10`
   hostfsd walk *does* resolve (hostfsd self-registers). The Phase-47 comment above
   the block ([`supervisor.c:1857-1879`](../ouroboros/supervisor.c#L1857)) describes
   the *direct-terminal* boot where these succeed; it predates the WM.
5. **Establish the WM session.** It then calls `wm_init` (retrying up to 5× on
   `NOENT`) and, on success, `maybe_lazy_wm_bind`
   ([`supervisor.c:1939-1950`](../ouroboros/supervisor.c#L1939),
   [`:791`](../ouroboros/supervisor.c#L791)) — which binds the WM console/keyboard/
   grid surfaces into `O5/O6/O7`, caches them, and registers
   `/sys/wm/<term>/leader-console` and `/sys/wm/<term>/leader-grid`. **Full detail in
   [`wm-terminal-overview.md` §9](wm-terminal-overview.md).**
6. **`hf_init()`; `orx_init()`** ([`supervisor.c:1952-1953`](../ouroboros/supervisor.c#L1952))
   set up the hostfsd client and the `.orx` spawn machinery.
7. **Self-register for peer discovery.** The supervisor `dir_register`s an `R|S`
   sub-cap of its `O9` mailbox at **`/sys/cpu/<procid>/supervisor`**
   (`render_peer_path`, [`supervisor.c:416`](../ouroboros/supervisor.c#L416)) — the
   single wire every other supervisor needs to relay a spawn to this CPU.

The leader then spawns the `sysinit → login → shell` chain (the children inherit the
WM surfaces via `TaskCreate`'s OPR copy — [`§9`](wm-terminal-overview.md)).

---

## 3. The dispatch loop and the RPC protocol

After boot, every supervisor enters the same loop
([`supervisor.c:2172-2277`](../ouroboros/supervisor.c#L2172)):

```c
for (;;) {
    int status = poll_one_request(&op, &len, &target_pid, &term_hint);  // O9 mailbox
    if (status != 0) {                 // timeout (leader's hot-attach pulse) or error
        if (is_leader) { reap_exited_tasks(); hot_attach_scan(); }
        continue;
    }
    switch (op) { … }
}
```

`poll_one_request` ([`supervisor.c:1756`](../ouroboros/supervisor.c#L1756))
`ReceiveQueuePoll`s `O9` and unpacks the message. A request is a `SEND` to the
supervisor's mailbox carrying: the **op** code, a payload byte **length**, a
**target pid**, a **terminal hint**, plus `O2` = a `TAG_DATA` bytes ref
(`path\0args\0cwd\0`) and `O3` = the caller's **reply cap**. (Recall the queue
register shift — [`wm-terminal-overview.md` §2](wm-terminal-overview.md) — so the
sender's `R4` arrives as the receiver's `R3`, etc.)

The leader sets its poll timeout to `HOT_ATTACH_POLL_TICKS = 5000`
([`supervisor.c:1752`](../ouroboros/supervisor.c#L1752),
[`:2168-2170`](../ouroboros/supervisor.c#L2168)) so an idle poll periodically wakes
it to reap and scan; workers leave the timeout at `-1` (block forever).

### Op codes

| `op` | Name | Handler | Action |
|------|------|---------|--------|
| 1 | spawn | `handle_spawn_request` ([`:1562`](../ouroboros/supervisor.c#L1562)) | spawn a program — local, relayed, or round-robin (§4) |
| 2 | shutdown | inline ([`:2191-2224`](../ouroboros/supervisor.c#L2191)) | cascade-kill every owned task (`task_kill`), then a **worker** relays op=2 to the leader |
| 4 | `SUP_OP_GET_DIR` | inline ([`:2225-2259`](../ouroboros/supervisor.c#L2225)) | reply with `DIR_SLOT` in `O2` so a child's `dir_init` can bootstrap its own directory mailbox (or status `-6` if this supervisor has no directory) |
| 5 | `SUP_OP_LIST_TASKS` | `handle_list_tasks_request` ([`:1504`](../ouroboros/supervisor.c#L1504)) | cross-CPU `ps`: format one line per live task, SEND a `TAG_DATA` bytes ref back |
| 6 | `SUP_OP_DIR_NOTIFY` | inline ([`:2265-2273`](../ouroboros/supervisor.c#L2265)) | oriscdir signalled `/sys/term` changed → re-run hot-attach scan (leader only) |

> **Shutdown teardown.** On `exit`/`quit` the shell SENDs op=2. The handler
> cascade-kills the supervisor's tasks so its table is clean, then a **worker**
> calls `relay_shutdown_to_leader` ([`supervisor.c:592`](../ouroboros/supervisor.c#L592))
> to forward op=2 to `/sys/cpu/0/supervisor`. The **leader** instead just returns
> from `main` — its CPU-0 exit is what `oriscrun`'s `--leader 0` watchdog watches to
> SIGTERM the rest of the process group ([`supervisor.c:2160-2224`](../ouroboros/supervisor.c#L2160)).

---

## 4. Spawning: local, relayed, and round-robin

`handle_spawn_request` ([`supervisor.c:1562`](../ouroboros/supervisor.c#L1562))
routes by `target_pid`:

- **`TARGET_PID_LOCAL` (`0xFF`)** or this CPU's own pid → spawn **here**.
- **A specific peer pid** → **relay** to that peer's supervisor.
- **`TARGET_PID_ANY` (`0xFE`)** → `pick_next_cpu`
  ([`supervisor.c:696`](../ouroboros/supervisor.c#L696)) chooses a live peer
  round-robin (walking `/sys/cpu/<N>/supervisor`), then relays. Constants at
  [`supervisor.c:156-157`](../ouroboros/supervisor.c#L156).

### Local spawn

`sup_spawn_named` ([`supervisor.c:688`](../ouroboros/supervisor.c#L688)) calls
`orx_spawn` → `orx_task_create` (the `.orx` loader, in
[`orx.c`](../tools/cc/lib/orx.c)), which ends in `#0x000 TaskCreate`. The child
**inherits the parent's object registers** (`O5/O6/O7` surfaces, `O8` spawn cap)
via `TaskCreate`'s OPR copy — the zero-per-spawn path that gives every child the WM
session (see [`§9`](wm-terminal-overview.md)).

For a spawn that must run with a *different* terminal's surfaces (a relayed `run @N`
or a hot-attached login), `populate_child_term_slots`
([`supervisor.c:891`](../ouroboros/supervisor.c#L891)) fills the per-child override
slots `ORX_SLOT_CHILD_O5/O6/O7`@632/648/664 — preferring the WM-mediated
`/sys/wm/<idx>/leader-*` caps (`try_walk_wm_leader_path`,
[`supervisor.c:879`](../ouroboros/supervisor.c#L879)), falling back to
`/sys/term/<idx>/*` — and `orx_task_create` swaps them into the child's OPRs just
before `TaskCreate`. `clear_child_term_slots` nulls them afterward so the override
doesn't leak into the next local spawn.

### Cross-CPU relay

`relay_spawn_request` ([`supervisor.c:518`](../ouroboros/supervisor.c#L518)):

1. Stash the request's `O2` (bytes ref) and `O3` (reply cap) into scratch slots
   `RELAY_BYTES_SLOT`@592 / `RELAY_REPLY_SLOT`@600 (a `dir_walk` would clobber the
   O-registers).
2. `dir_walk("/sys/cpu/<target>/supervisor")` to resolve the peer's mailbox cap.
3. Restore `O2`/`O3` and SEND op=1 to the peer with `R6 = TARGET_PID_LOCAL` (so the
   peer spawns locally rather than relaying again) and the **original reply cap
   unchanged** — so the peer's spawn reply goes **straight back to the original
   requester**, not back through the relayer.

---

## 5. Hot-attach (leader only)

When a terminal appears or disappears, its `/sys/term` directory entries change.
`oriscdir` notifies subscribers (op=6), and the leader's idle poll also pulses every
`HOT_ATTACH_POLL_TICKS`. Either way the leader runs `hot_attach_scan`
([`supervisor.c:1146`](../ouroboros/supervisor.c#L1146)): it diffs the current
`/sys/term` listing against what it has seen and **spawns a login** for each new
terminal (and is wired to kill the login for a departed one). `reap_exited_tasks`
([`supervisor.c:1211`](../ouroboros/supervisor.c#L1211)) unloads `EXITED` task slots
so the table and `ps` stay accurate.

> Under Phase 60, `/sys/term` is populated by *terminal devices*, which a normal
> `make boot` does not run — so hot-attach is exercised mainly in direct-terminal /
> multi-terminal configurations, not the default WM boot.

---

## 6. Key O12 OBJSTORE slots

All are byte offsets in the supervisor's `O12` task-table OBJSTORE (defines at
[`supervisor.c:104-175`](../ouroboros/supervisor.c#L104)):

| Offset | Slot | Holds |
|--------|------|-------|
| 544 | `BOOT_PARENT_SLOT` | boot directory mailbox (boot `O8`) |
| 560 | `ORX_SLOT_CHILD_O8` | spawn-service sub-cap injected into each child's `O8` |
| 576 | `SUP_SCRATCH_SLOT` | reply cap stashed across `orx_spawn`/`ObjAlloc` |
| 584 | `DIR_SLOT` | directory mailbox `dir.c` uses |
| 592 / 600 | `RELAY_BYTES_SLOT` / `RELAY_REPLY_SLOT` | request `O2`/`O3` stashed across a relay `dir_walk` |
| 616 | `DIR_RESULT_SLOT` | where `dir_walk` publishes the resolved ref |
| 632 / 648 / 664 | `ORX_SLOT_CHILD_O5/O6/O7` | per-spawn console/keyboard/grid overrides |
| 696 / 704 | `WM_LEADER_CONSOLE_SLOT` / `WM_LEADER_GRID_SLOT` | cached WM-mediated leader caps |

---

## Open questions / things to verify

- **Worker WM mediation.** The eager-bind block
  ([`supervisor.c:1939-1950`](../ouroboros/supervisor.c#L1939)) is not obviously
  gated on `is_leader`, yet the Phase-56 comment
  ([`supervisor.c:1903-1912`](../ouroboros/supervisor.c#L1903)) says "workers stay on
  direct per-CPU `/sys/term/<procid>/*`." In a WM boot a worker's `/sys/term` walk
  fails, so what surfaces a worker supervisor (pid 1) ends up with — and whether it
  binds its own WM window — is worth tracing on a live two-terminal boot.
- **Hot-attach keyboard asymmetry.** Same item flagged in
  [`wm-terminal-overview.md`](wm-terminal-overview.md) Open questions: relayed /
  hot-attach children resolve keyboard only from `/sys/term/<idx>/keyboard`
  ([`supervisor.c:891+`](../ouroboros/supervisor.c#L891)), which doesn't resolve
  under Phase 60, so they rely on inherited `O6`.
- **Stale `oriscterm` / `/sys/term` comments** are pervasive in `supervisor.c`
  (e.g. [`:1857-1879`](../ouroboros/supervisor.c#L1857),
  [`:1903-1912`](../ouroboros/supervisor.c#L1903)); they describe the direct-terminal
  boot, not the WM-mediated default. No `oriscterm.orx` is ever spawned.
