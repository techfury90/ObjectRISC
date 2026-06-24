# The Directory & Host-Filesystem Subsystem

*An architecture overview for new contributors.*

This document describes how Ouroboros programs find each other by **name** rather
than by hard-wired capability: the directory daemon
[`tools/devices/oriscdir`](../tools/devices/oriscdir), its libc client
[`tools/cc/lib/dir.c`](../tools/cc/lib/dir.c), and how the host-filesystem server
[`tools/devices/hostfsd`](../tools/devices/hostfsd) plugs into the namespace.

The crossbar (Volume IV) only routes by `dst_pid`; it has no notion of names. The
directory is the **name → object-reference** layer built on top: a single daemon
holds a tree of names, and any port can register a ref under a name or walk a name
to a ref. `hostfsd`'s own wire protocol, `--root` jail, error codes, and the
`host_io.c` client are already documented in
[`tools/devices/README.md`](../tools/devices/README.md#hostfsd); this guide does not
restate them — it covers the **directory** and how host FS attaches to it. Every
non-obvious claim cites `file:line`.

```
        program (CPU)
           │  dir_walk("/programs/edit.orx")          dir.c
           ▼
   ┌───────────────────────────── oriscdir (pid 18) ─────────────────────────┐
   │  name tree (Node: DIR / LEAF / MOUNT)                                    │
   │                                                                         │
   │   /                                                                     │
   │   ├── sys/                                                              │
   │   │   ├── cpu/<n>/supervisor   LEAF → supervisor mailbox sub-cap        │
   │   │   ├── wm/<n>/{0,leader-console,leader-grid}  LEAF → WM caps         │
   │   │   ├── term/<n>/{console,keyboard,grid}  LEAF → terminal caps        │
   │   │   │                                     (direct-terminal boots only)│
   │   │   └── hostfsd/<n>          LEAF → hostfsd service sub-cap           │
   │   └── programs/                MOUNT → hostfsd service + prefix         │
   └─────────────────────────────────────────────────────────────────────────┘
                                       │ MOUNT delegates the walk remainder
                                       ▼
                              hostfsd (pid 17) — host FS jail
```

---

## 1. The namespace model (`oriscdir`)

`oriscdir` is a content-blind crossbar port like any device
([`tools/devices/README.md` §wire-layer](../tools/devices/README.md)). It exposes
**one service object** at index `1`, generation `1`, max-caps `0x5B` (`R|W|S|V|C`)
([`oriscdir:213-215`](../tools/devices/oriscdir#L213)). In a `make boot` it runs at
**pid 18**, started *first* among devices so later self-registrations have
somewhere to land ([`wm-terminal-overview.md` §3](wm-terminal-overview.md)).

The name tree is built from one `Node` type
([`oriscdir:347`](../tools/devices/oriscdir#L347)); a node's kind is implied by
which field is set ([`oriscdir:359-361`](../tools/devices/oriscdir#L359)):

| Kind (R4) | Node state | Meaning |
|-----------|-----------|---------|
| `KIND_DIR` (1) | `children` non-empty | an interior directory |
| `KIND_LEAF` (2) | `leaf_ref` set | a name bound to **one object reference** |
| `KIND_MOUNT` (3) | `mount_ref` + `mount_prefix` | a subtree **delegated** to a backend service |
| `KIND_NOT_FOUND` (0) | — | the walk fell off the tree |

A **MOUNT** is how a whole subtree (e.g. `/programs`) is handed to a backend like
`hostfsd`: a walk that descends into the mount returns the backend's service ref
plus the **remainder** path the caller should forward to it.

### The wire protocol

All requests are a `SEND` to oriscdir's service object; op code in the first int
word ([`oriscdir:219-225`](../tools/devices/oriscdir#L219), dispatch at
[`_handle_send`, `oriscdir:541`](../tools/devices/oriscdir#L541)). Recall the
queue register shift ([`wm-terminal-overview.md` §2](wm-terminal-overview.md)): a
sender's `R4` is read as the receiver's first payload word.

| Op | Name | Payload | Reply |
|----|------|---------|-------|
| 1 | `OP_REGISTER` | `O2`=path bytes, `O4`=ref to bind, `O3`=reply cap, `R5`=path len | `R3`=status ([`_do_register`, `:717`](../tools/devices/oriscdir#L717)) |
| 2 | `OP_MOUNT` | `O2`=`path\0prefix\0`, `O4`=backend service ref, `R5`/`R6`=lengths | `R3`=status |
| 3 | `OP_WALK` | `O2`=path bytes, `O4`=remainder dest buf, `O3`=reply cap, `R5`=path len, `R6`=remainder cap | `R3`=status, `R4`=kind, `R5`=remainder len, `O2`=resolved ref ([`_do_walk`, `:805`](../tools/devices/oriscdir#L805)) |
| 4 | `OP_LIST` | `O2`=path, `O4`=names dest buf | `R3`=status, `R4`=count, `R5`=bytes written |
| 5 | `OP_REG_INLINE` | path **packed into spare register words** (32-byte budget), `or_payload[0]`=ref | none (fire-and-forget) |
| 6 | `OP_SUBSCRIBE` | `O2`=notify cap | notified on later mutations under the path |

> **Inline register (`OP_REG_INLINE`).** A device registering itself at boot has no
> object yet to hold its path string, and ObjAlloc on a freshly-booted device is
> awkward. So Phase 47 added a path-inline variant: up to 32 path bytes are packed
> into the spare int/OR payload words ([`oriscdir:106-130`](../tools/devices/oriscdir#L106),
> [`_do_register_inline`, `:659`](../tools/devices/oriscdir#L659)) and the request is
> fire-and-forget. `hostfsd` uses exactly this to self-register at
> `/sys/hostfsd/<instance>` before it prints `READY`
> ([`hostfsd:385-440`](../tools/devices/hostfsd#L385)).

A **walk** that lands in a MOUNT writes the leftover path tail into the caller's
`O4` buffer via `OBJ_WRITE_REQ`, and returns `KIND_MOUNT` + the backend ref; the
caller then SENDs the remainder to that backend. A walk to a LEAF returns
`KIND_LEAF` + the bound ref in `O2`.

---

## 2. The libc client (`dir.c`)

[`dir.c`](../tools/cc/lib/dir.c) is **raw wire-asm** (not migrated onto the handle
API — see [`OBJECT_API.md`](OBJECT_API.md)). It uses these O12 OBJSTORE slots
([`dir.c:70-101`](../tools/cc/lib/dir.c#L70)):

| Offset | Slot | Holds |
|--------|------|-------|
| 544 | `BOOT_PARENT_SLOT` | the supervisor's mailbox (boot `O8`) — used to bootstrap the directory ref |
| 552 | `REPLY_MB_SLOT` | the program's per-call reply mailbox |
| 584 | `DIR_SLOT` | the directory service mailbox (the SEND target) |
| 608 | `DIR_REPLY_SCRATCH` | the derived reply sub-cap, stashed across the SEND |
| 616 | `DIR_RESULT_SLOT` | **where a successful walk publishes the resolved ref** |
| 624 | `DIR_INPUT_REF_SLOT` | the ref-to-register, saved on entry (register/mount clobber `O1`) |

### Bootstrapping the directory ref — `dir_init`

A program doesn't boot with the directory mailbox; only the *supervisor* does (it
copies boot `O8` into `DIR_SLOT`). `dir_init`
([`dir.c:279`](../tools/cc/lib/dir.c#L279)) lazily fills `DIR_SLOT` on first use:
if it's null, it SENDs **`op = SUP_OP_GET_DIR` (4)** to the supervisor mailbox in
`BOOT_PARENT_SLOT`, polls `REPLY_MB_SLOT`, and `OREFST`s the supervisor's reply
(`O2` = the directory ref) into `DIR_SLOT`
([`dir.c:300-330`](../tools/cc/lib/dir.c#L300)). The supervisor side of this RPC is
op=4 in [`supervisor.c:2225-2259`](../ouroboros/supervisor.c#L2225) (see
[`supervisor-overview.md` §3](supervisor-overview.md)).

### The public calls

| Function | Op | Notes |
|----------|----|-------|
| `dir_register(path)` ([`:340`](../tools/cc/lib/dir.c#L340)) | 1 | caller puts the ref to bind in `O1`; `dir_register` saves it to `DIR_INPUT_REF_SLOT`@624 *immediately* (because `dir_init` clobbers `O1`), then SENDs |
| `dir_mount(path, prefix)` ([`:425`](../tools/cc/lib/dir.c#L425)) | 2 | bind a backend service + path prefix as a MOUNT |
| `dir_walk(path, &kind, rem_buf, rem_cap)` ([`:551`](../tools/cc/lib/dir.c#L551)) | 3 | resolves a name; **publishes the resolved ref into `DIR_RESULT_SLOT`@616** for the caller to `OREFLD`. Returns the remainder length (MOUNT only); the remainder is `ObjFetchBytes`ed from a scratch buffer into the caller's stack |
| `dir_list(path, buf, cap)` ([`:777`](../tools/cc/lib/dir.c#L777)) | 4 | NUL-separated child names |
| `dir_subscribe(path)` ([`:933`](../tools/cc/lib/dir.c#L933)) | 6 | register a notify cap for changes under `path` |

The **`DIR_RESULT_SLOT`@616 hand-off is the key idiom**: `dir_walk` leaves the
resolved capability in a known O12 slot rather than relying on an O-register
surviving the return, so the caller `OREFLD`s it wherever it wants — into `O5/O6/O7`
(the supervisor's surface walks), or adopted into an `obj_t` handle via
`obj_adopt_dir_result` ([`OBJECT_API.md` §4](OBJECT_API.md)). This is the same slot
the WM bind path uses ([`wm-terminal-overview.md` §5](wm-terminal-overview.md)).

---

## 3. Host FS on the namespace (`hostfsd`)

`hostfsd` is a host-filesystem server that lets CPU-side programs read and write the
host's files. Its single service object (index 1), op dispatch
(`SUBSCRIBE/OPEN/CLOSE/READ/WRITE`), the per-CPU reply mailbox, the `--root` jail,
the error codes, and the `host_io.c` client (`hf_open`/`hf_read`/…) are **fully
documented in [`tools/devices/README.md` §hostfsd](../tools/devices/README.md#hostfsd)**
and are not repeated here.

What this guide adds is how it joins the namespace:

- **Self-registration.** At boot, `hostfsd` inline-registers its `R|S` mailbox
  sub-cap at `/sys/hostfsd/<instance>` via `OP_REG_INLINE`
  ([`hostfsd:385-440`](../tools/devices/hostfsd#L385)). The supervisor walks
  `/sys/hostfsd/0` at boot and parks the ref in `O10`
  ([`supervisor.c:1898-1900`](../ouroboros/supervisor.c#L1898)), which `hf_init`
  reads.
- **`/programs` as a MOUNT.** A program directory is exposed by `dir_mount`-ing a
  backend under `/programs`, so `dir_walk("/programs/edit.orx")` returns
  `KIND_MOUNT` + the backend ref + remainder `edit.orx`, which the loader forwards.
- **`host_io.c` is raw wire-asm**, not migrated onto the handle API, and its `O8`
  reply mailbox is *not* private (the supervisor and `term_print_n_sync` read it
  too) — see [`OBJECT_API.md`](OBJECT_API.md) Open questions.

---

## Open questions / things to verify

- **Where `/programs` is mounted, and to what backend.** This guide infers the
  MOUNT-delegation shape from `oriscdir`'s `KIND_MOUNT` handling and the inline
  `dir_mount("/programs", …)` comment ([`oriscdir:49`](../tools/devices/oriscdir#L49));
  the exact mounting site (which process issues the `dir_mount`, and whether the
  backend is `hostfsd` or a dedicated program store) should be traced on a live
  boot before relying on it.
- **`OP_SUBSCRIBE` notify delivery.** `dir_subscribe` registers a notify cap and the
  supervisor consumes `SUP_OP_DIR_NOTIFY` (op=6) hot-attach pulses
  ([`supervisor-overview.md` §5](supervisor-overview.md)); the precise mutation set
  that triggers a notification (register / register-inline / mount, per
  [`oriscdir:135`](../tools/devices/oriscdir#L135)) and whether removals notify is
  worth confirming.
- **No name removal.** `oriscdir` grows entries but, at the time of writing, has no
  documented entry-*removal* op — relevant to hot-attach detach, which the
  supervisor is wired for but which depends on `/sys/term` entries disappearing.
