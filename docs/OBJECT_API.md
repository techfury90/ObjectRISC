# The libc Handle-Based Object API (`obj.{h,c}`)

*An architecture overview for new contributors.*

This document describes the **handle-based object/capability API** that Object
RISC's libc exposes to C programs: [`tools/cc/lib/obj.h`](../tools/cc/lib/obj.h)
and [`tools/cc/lib/obj.c`](../tools/cc/lib/obj.c). It is the layer a C program
uses to allocate objects, derive sub-capabilities, attach receive queues, and
`SEND`/receive messages **without ever holding a capability as a C value** —
because the v1 compiler can't.

It is a *current-implementation* document (the active Phase-4 migration surface),
not part of the formal architecture spec. The **firmware** capability model —
references, descriptors, capability bits, the object table — is
[`docs/OBJECT_SYSTEM.md`](OBJECT_SYSTEM.md) (Volume III); the **firmware
primitives** this API wraps are in
[`docs/SYSTEM_FIRMWARE_INTERFACE.md`](SYSTEM_FIRMWARE_INTERFACE.md) (Volume VI).
This guide covers only the thin libc layer on top, and how to migrate a client
onto it. Every non-obvious claim cites `file:line`.

---

## 1. Why handles exist (the v1-pcc constraint)

Object RISC keeps capabilities in 16 dedicated *object registers* `O0..O15`; a
capability may never be byte-spilled to general memory (that would defeat the
capability invariant — Volume III §5). The v1 `pcc` backend has no way to keep an
`__or` capability **value** live in an object register across a function call:
it would have to spill it, which the architecture forbids
([`obj.h:1-12`](../tools/cc/lib/obj.h#L1),
[`tools/cc/arch/orisc/OREG_MIGRATION_PLAN.md`](../tools/cc/arch/orisc/OREG_MIGRATION_PLAN.md)).

So instead of returning capabilities to C, this API keeps them in a small
per-task table that libc owns, and hands the program an **opaque integer handle**
`obj_t` ([`obj.h:25-28`](../tools/cc/lib/obj.h#L25)) — exactly the
file-descriptor pattern [`host_io.c`](../tools/cc/lib/host_io.c) already uses for
hostfsd fds. Capabilities never appear as C values, so handle-based code compiles
and runs on the current toolchain today.

> This is the **substitute** that shipped while the `__or`-VALUE object API (which
> would let C hold capability values directly) stays blocked behind the OR-spill
> work. Treat `obj_t` as "a file descriptor for a capability."

A handle is just an index `0..OBJ_NHANDLE-1` into the table; `OBJ_NULL` (`-1`) is
the error / "no handle" sentinel. Every handle-returning call yields `OBJ_NULL` on
failure; every status-returning call yields `<0` on error
([`obj.h:14-19`](../tools/cc/lib/obj.h#L14)).

---

## 2. The handle table (8 slots in the O12 OBJSTORE)

The table lives at **byte offset `OBJ_TABLE_OFFSET = 1704`** of the **O12
task-table OBJSTORE** — the OR-typed per-task store libc parks in `O12` at
`task_init` (see [`wm-terminal-overview.md` §3](wm-terminal-overview.md), boot-OR
contract). It holds **`OBJ_NHANDLE = 8`** capability slots, 8 bytes each
([`obj.h:28,35`](../tools/cc/lib/obj.h#L28)):

```
O12 OBJSTORE  (OR-typed storage; OREFLD/OREFST only)
 ┌─ … libc slots (DIR_RESULT@616, WM_SLOT@680, …) …
 ├─ 1696  compiler OR-spill anchor          (OR_SPILL_ANCHOR_OFFSET, task.c:262)
 ├─ 1704  handle 0  ┐
 ├─ 1712  handle 1  │
 ├─ 1720  handle 2  │  8 capability slots, 8 bytes each
 ├─ …               │  obj_inuse bitmask (obj.c:25) tracks which are live
 └─ 1760  handle 7  ┘
```

Properties worth knowing:

- **It is reserved by oversizing, not allocated separately.**
  [`task.c`](../tools/cc/lib/task.c) bumps `ORX_STATE_BYTES` so the O12 allocation
  extends past the compiler's OR-spill anchor (1696) to cover the
  `OBJ_NHANDLE*8 = 64` table bytes ([`task.c:251-262`](../tools/cc/lib/task.c#L251)).
- **The per-slot offsets are hard-coded** because `OREFLD`/`OREFST` take only an
  *immediate* offset — so the slot↔O-register moves are `switch (h)` ladders over
  the compile-time offsets ([`obj.c:33-91`](../tools/cc/lib/obj.c#L33)), one ladder
  per target register (`O1`, `O2`, `O3`). A `typedef`-based static assertion
  (`obj__off_check`, [`obj.c:22`](../tools/cc/lib/obj.c#L22)) catches base drift at
  compile time if `OBJ_TABLE_OFFSET` ever moves.
- **An in-use bitmask gates every access.** `obj_inuse`
  ([`obj.c:25`](../tools/cc/lib/obj.c#L25)) marks live handles; every API call
  rejects a handle whose bit is clear, so a stale ref left in a freed slot is
  unreachable.
- **`obj_init` is idempotent.** It returns `-1` if `O12` is null (i.e.
  `task_init` hasn't run), and otherwise zeroes `obj_inuse` only on the *first*
  call, so a second migrated subsystem calling `obj_init` doesn't clobber live
  handles ([`obj.c:106-120`](../tools/cc/lib/obj.c#L106)).

The **orx.c idiom** recurs throughout: a firmware call leaves the fresh reference
in `O1`, and `obj__store_o1` `OREFST`s it into the handle's slot *immediately*,
before anything can clobber `O1` (e.g. [`obj_alloc`, `obj.c:147`](../tools/cc/lib/obj.c#L147)).

---

## 3. The API surface

Lifecycle: call `task_init()` (sets up `O12`), then `obj_init()` once, then the
rest. Grouped by purpose ([`obj.h`](../tools/cc/lib/obj.h)):

| Function | Wraps | Notes |
|----------|-------|-------|
| `obj_init()` | — | one-time; after `task_init` ([`obj.c:106`](../tools/cc/lib/obj.c#L106)) |
| `obj_alloc(len,tag,caps)` | `#0x100 ObjAlloc` | byte object → handle ([`obj.c:124`](../tools/cc/lib/obj.c#L124)) |
| `obj_alloc_store(len,tag,caps)` | `#0x106 ObjAllocStore` | OR-typed storage; `len` % 8 == 0 ([`obj.c:152`](../tools/cc/lib/obj.c#L152)) |
| `obj_derive(src,caps)` | `#0x103 ObjDerive` | sub-cap → new handle ([`obj.c:178`](../tools/cc/lib/obj.c#L178)) |
| `obj_free(h)` | `#0x101 ObjFree` | frees object (needs `V`) + releases handle ([`obj.c:205`](../tools/cc/lib/obj.c#L205)) |
| `obj_drop(h)` | — | release a derived/borrowed handle **without** freeing its object ([`obj.c:225`](../tools/cc/lib/obj.c#L225)) |
| `obj_adopt_dir_result()` | — | adopt the cap `dir_walk` left in `DIR_RESULT`@616 ([`obj.c:241`](../tools/cc/lib/obj.c#L241)) |
| `obj_adopt_o6()` | — | adopt boot `O6` (the keyboard service) ([`obj.c:263`](../tools/cc/lib/obj.c#L263)) |
| `obj_isnull/eq/len/tag/caps(h)` | `oisn/oeq/olen/otag/ocap` | inspection, no memory access ([`obj.c:287-346`](../tools/cc/lib/obj.c#L287)) |
| `obj_loadw/storew(h[,v])` | `olw/osw` | word at offset 0 (needs `R`/`W`) ([`obj.c:350-365`](../tools/cc/lib/obj.c#L350)) |
| `obj_queue_attach(h,depth)` | `#0x203 ReceiveQueueAttach` | make a mailbox ([`obj.c:369`](../tools/cc/lib/obj.c#L369)) |
| `obj_send(h,a0..a3)` | `SEND` | int payload only, null OR payload ([`obj.c:389`](../tools/cc/lib/obj.c#L389)) |
| `obj_send_or(h,or_h,a0..a3)` | `SEND` | `O2` = `or_h`'s cap (or null) — the subscribe wrapper ([`obj.c:409`](../tools/cc/lib/obj.c#L409)) |
| `obj_send_bytes(svc,src,reply,a0..a3)` | `SEND` | the data-send keystone (below) ([`obj.c:433`](../tools/cc/lib/obj.c#L433)) |
| `obj_recv(h)` | `#0x204 ReceiveQueuePoll` | block; returns the `R3` word only ([`obj.c:465`](../tools/cc/lib/obj.c#L465)) |
| `obj_poll(h,out[4])` | `#0x204` | non-blocking; `out[0..3]` = `R3..R6` ([`obj.c:494`](../tools/cc/lib/obj.c#L494)) |
| `obj_recv_full(h,out[4])` | `#0x204` | blocking sibling of `obj_poll` ([`obj.c:525`](../tools/cc/lib/obj.c#L525)) |

Type tags and capability bits are mirrored as `OBJ_TAG_*` / `OBJ_CAP_*`
([`obj.h:37-49`](../tools/cc/lib/obj.h#L37)); they match the firmware values in
Volume III §5.

### The three SEND wrappers

The wire mechanics (the four int words `R4..R7`, the four OR slots `O1..O4`, and
the **register shift** that delivers a queued SEND's `R4..R7` into the receiver's
`R3..R6`) are explained once in
[`wm-terminal-overview.md` §2](wm-terminal-overview.md); here is just how the
wrappers map onto them.

- **`obj_send`** — recipient in `O1`, `O2..O4` nulled, `R4..R7 = a0..a3`. Pure
  int-payload notification.
- **`obj_send_or`** — like `obj_send` but `O2` carries `or_h`'s capability (or a
  null `O2` when `or_h == OBJ_NULL`, the coarse v1 "unsubscribe" convention). Used
  to hand a service a sub-cap of your own mailbox — i.e. to **subscribe**
  ([`obj.h:113-117`](../tools/cc/lib/obj.h#L113)).
- **`obj_send_bytes`** — the **data-send keystone**. A SEND's int words can't
  carry a string or pixel buffer, so the sender puts a **segment reference** in
  `O2` and a byte offset/length in the int payload, and the receiver
  `ObjFetchBytes`es (`#0x108`) the bytes out. `src` selects the segment:
  `OBJ_SRC_STACK` → boot stack (`O11`), `OBJ_SRC_DATA` → boot data (`O15`),
  `OBJ_SRC_NONE` → null `O2`; `reply` optionally provides an ack mailbox in `O3`
  ([`obj.h:119-134`](../tools/cc/lib/obj.h#L119), [`obj.c:433`](../tools/cc/lib/obj.c#L433)).
  This is what every message-with-data client needs (console, grid, raster, dir,
  host_io).

> `obj_send*` return `0` even on a wire fault — a `SEND` **traps** on error rather
> than reporting status, so there is no status to relay ([`obj.c:406`](../tools/cc/lib/obj.c#L406)).
> They return `-1` only for a bad *handle*.

`obj_poll`/`obj_recv_full` return five values (status + four payload words) but
`pcc` allows only four `asm` outputs, so the status `R2` is `sw`'d to a file-scope
global (`obj__poll_status`, [`obj.c:492`](../tools/cc/lib/obj.c#L492)) from inside
the `asm` body and read back in C.

---

## 4. The migration pattern (the proven recipe)

Migrating a wire-asm client onto handles follows a fixed shape, established by
`raster.c`, `pointer.c`, and `vector.c`. Three steps:

**1. Adopt the service cap into a handle at init.** Bring the capability into the
handle world *inside libc*, so it never crosses a call boundary in an O-register:

```c
static obj_t svc_h = OBJ_NULL;

int foo_init_from_dir_result(void) {
    if (obj_init() != 0) return -1;
    svc_h = obj_adopt_dir_result();          /* cap left by wm_bind_surface */
    return (svc_h < 0) ? -1 : 0;
}
```

`raster_init_from_dir_result` ([`raster.c:33-41`](../tools/cc/lib/raster.c#L33)),
`vec_init_from_dir_result` ([`vector.c:74-78`](../tools/cc/lib/vector.c#L74)), and
`pointer_init_from_dir_result` ([`pointer.c:46-49`](../tools/cc/lib/pointer.c#L46))
adopt from the dir-walk result (`DIR_RESULT`@616); `term_init`
([`term.c:182-183`](../tools/cc/lib/term.c#L182)) uses `obj_adopt_o6()` for the
boot keyboard service.

**2. Each helper guards on the handle, then sends.** Compute any byte offset, then
call the matching wrapper:

- pure ops → `obj_send` (e.g. `vec_*`, [`vector.c:63`](../tools/cc/lib/vector.c#L63));
- subscribe/unsubscribe → `obj_send_or` (e.g. `pointer_subscribe`,
  [`pointer.c:69-72`](../tools/cc/lib/pointer.c#L69); `term` keyboard,
  [`term.c:206`](../tools/cc/lib/term.c#L206));
- byte data → `obj_send_bytes` (e.g. `raster_blit`,
  [`raster.c:42-63`](../tools/cc/lib/raster.c#L42));
- receive → `obj_poll` / `obj_recv_full` (e.g. `pointer_getevent`,
  `term_getkey` at [`term.c:465`](../tools/cc/lib/term.c#L465)).

**3. Restore the boot OPRs the SEND clobbered.** A `SEND`'s reply-overlay clobbers
`O2..O4`. Clients restore `O2 = boot stack (O11)` and `O3 = boot data (O15)` after
every SEND, or a following `print_str` would read its string through a clobbered
`O2` — see `_vec_restore_or` ([`vector.c:38-50`](../tools/cc/lib/vector.c#L38)).
This is the same OPR-hygiene contract the wire-asm clients already followed.

### Migration status

| Client | File | State |
|--------|------|-------|
| handle layer | [`obj.{h,c}`](../tools/cc/lib/obj.c) | the API itself |
| raster | [`raster.c`](../tools/cc/lib/raster.c) | **migrated** — `obj_send_bytes` blits |
| pointer | [`pointer.c`](../tools/cc/lib/pointer.c) | **migrated** — `obj_send_or` / `obj_poll` |
| vector | [`vector.c`](../tools/cc/lib/vector.c) | **migrated** (Phase 4) — `obj_send` draws |
| terminal | [`term.c`](../tools/cc/lib/term.c) | **partly migrated** — keyboard on handles; the **console path (`term_print*`) is still raw asm to `O5`** ([`term.c:33-34`](../tools/cc/lib/term.c#L33)) |
| grid | [`grid.c`](../tools/cc/lib/grid.c) | not migrated (wire-asm) |
| host I/O | [`host_io.c`](../tools/cc/lib/host_io.c) | not migrated (wire-asm; see Open questions) |

---

## 5. The async-buffer-lifetime pitfall

Every `obj_send_bytes` is **fire-and-forget**: the receiver issues its
`ObjFetchBytes` for the payload *after* the sending CPU has moved on, with no ack
in the wire protocol. So the source buffer must stay alive **and unchanged** until
that async fetch — a transient stack buffer overwritten by a later (deeper,
blocking) call is read as garbage, and because the symptom is non-deterministic
and shifts when you add a debug print, it **masquerades as a compiler /
register-allocation Heisenbug**. It is not.

This trap, its recognition signature, and the fixes are written up in full in
[`wm-terminal-overview.md` §7](wm-terminal-overview.md) — read it before sending
byte data from a transient frame; it is not restated here. (Vector/pointer ops are
immune because they carry their whole payload in the int words, with no byte
source — [`vector.c:13-19`](../tools/cc/lib/vector.c#L13).)

---

## Open questions / things to verify

- **`host_io.c` migration is not done, and is non-trivial.** Its hostfsd reply
  mailbox lives in `O8` and is **not** private — `term_print_n_sync` and the
  supervisor read it too. A migration must keep the mailbox cap mirrored in `O8`
  and adopt the hostfsd service ref (`O10`), which would need adopt/park helpers
  this branch's `obj.{h,c}` does **not** yet provide (only `obj_adopt_dir_result`
  and `obj_adopt_o6` exist). Verify the `O8` contract before migrating.
- **`grid.c` is still wire-asm.** It is the obvious next `obj_send_bytes`
  candidate (same positioned-text shape as the console), but had not been migrated
  at the time of writing.
- **The table holds only 8 handles** (`OBJ_NHANDLE`). A client needing more than
  eight simultaneously-live capabilities will get `OBJ_NULL` from `obj_alloc` /
  `obj_derive` / the adopt helpers; there is no dynamic growth. No current client
  comes close, but a future one might.
- **`obj_recv` returns only the `R3` word** ([`obj.h:136-139`](../tools/cc/lib/obj.h#L136));
  callers needing the full four-word payload must use `obj_recv_full` / `obj_poll`.
