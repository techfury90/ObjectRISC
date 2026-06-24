# The Ouroboros WM / Terminal / Graphics Subsystem

*An architecture overview for new contributors.*

This document describes the window-manager + terminal + graphics stack of
**Ouroboros**, the OS that runs on the Object RISC capability CPU (simulated by
[`tools/sim/simorisc`](../tools/sim/simorisc)). It traces how a keystroke gets
from the host into a program, how a program draws to the screen, and how the
whole thing is wired and booted.

It is deliberately a *current-architecture* document. The code carries a thick
sediment of comments from earlier designs — most importantly, comments that
describe a separate `oriscterm` terminal process and surfaces published under
`/sys/term/<n>/*`. **That world no longer exists in a normal boot** (see the
[Phase 60 caveat](#0-the-phase-60-caveat-the-wm-is-the-terminal) immediately
below). Where a comment contradicts the running code, this document states the
current reality and flags the stale comment. Every non-obvious claim cites
`file:line` so you can verify it.

---

## 0. The Phase 60 caveat: the WM *is* the terminal

Read this first; it reframes everything else.

Historically Ouroboros had a separate Tk terminal device,
[`tools/devices/oriscterm`](../tools/devices/oriscterm), that owned the screen,
keyboard, and mouse. CPUs reached it over the crossbar, and it self-registered
its console/keyboard/grid/… services under `/sys/term/<n>/*` in the directory
daemon. The window manager was a client of that terminal.

**Phase 60 collapsed the terminal into the window manager.** As of *Phase 60
step 2/3*, a WM CPU allocates its **own framebuffer** (firmware primitive
`#0x10A ObjAllocFramebuffer`) and its **own keyboard and pointer input sinks**
(`#0x10B ObjAllocInputSink`) inside its own `simorisc` process, and the host
display worker (`--display tk`) mirrors that framebuffer to a Tk window and feeds
host keystrokes/mouse straight into those sinks. The WM CPU *is* the terminal
firmware ([`oriscwm.c:872-900`](../ouroboros/oriscwm.c#L872),
[`oriscwm.c:825-870`](../ouroboros/oriscwm.c#L825)).

Concretely, in a `make boot` system:

- **There is no `oriscterm` process.** `scripts/boot.sh`'s actual `exec` launches
  no terminal device ([`boot.sh:125-131`](../scripts/boot.sh#L125)). `oriscterm`
  is **legacy/vestigial** — it survives as the reference Tk device for a handful
  of direct-terminal demos (all launched via `oriscrun --terminal`), not the
  WM-mediated path. (`test_framebuffer.sh` exercises the framebuffer
  `OBJ_READ/WRITE` protocol against `fake_terminal.py`, which implements those
  handlers itself ([`fake_terminal.py:208-286`](../tools/devices/tests/fake_terminal.py#L208)),
  **not** the real `oriscterm`; the stale in-script comment claiming otherwise is
  flagged in [`tools/devices/README.md`](../tools/devices/README.md).)
- **Nothing is published under `/sys/term/<n>/*`.** The supervisor's attempt to
  walk `/sys/term/<procid>/{console,keyboard,grid}` is *expected to fail* and is
  a no-op; the supervisor separately establishes a WM session via `wm_init`
  (details in [§9](#9-the-supervisors-role)).

Stale comments to distrust (non-exhaustive; each is flagged again in context):

- `term.c`, `grid.c`, `vector.c`, `raster.c` headers all say "oriscterm" — they
  mean "the WM" now.
- `term.c:195-202` claims the O9 keyboard mailbox is "depth 16 ... shared:
  keyboard events AND hostfsd responses both land here." Both halves are wrong:
  the queue is **depth 64** ([`term.c:205`](../tools/cc/lib/term.c#L205)), and
  hostfsd uses its **own O8 mailbox** ([`host_io.c:120-167`](../tools/cc/lib/host_io.c#L120)).
- `oriscwm.c`'s entire header block, and many in-body comments, describe
  forwarding "to the underlying terminal." There is no underlying terminal; the
  WM renders locally.
- `tools/devices/README.md`'s `oriscterm` section and `ouroboros/README.md`'s
  boot description both predate Phase 60.

---

## 1. TL;DR + architecture diagram

**TL;DR.** Ouroboros runs as a handful of single-CPU `simorisc` processes wired
together by a software crossbar (`oriscbar`). Two of those CPUs run the window
manager ([`oriscwm.c`](../ouroboros/oriscwm.c)); each owns a host Tk window, a
local framebuffer, and local keyboard/pointer input sinks, and *is* the terminal
for its display. Other CPUs run the supervisor ([`supervisor.c`](../ouroboros/supervisor.c)),
which establishes a WM session (one window + console/keyboard/grid surface caps)
and hands it down the `sysinit → login → shell` chain. Programs talk to the WM
purely by message-passing: they `SEND` capability-addressed requests (open a
window, bind a surface, write text/grid/vector/raster, subscribe to input) and
poll a reply mailbox. Host input flows host → Tk worker (or a headless
`PKT_HOST_INPUT` packet) → the WM's input sink → the *focused* window's
subscriber → the client's `term_getkey`/`pointer_getevent`. Output flows client
`SEND` → the WM's per-window surface service → `ObjFetchBytes` of the referenced
bytes → composite into the framebuffer → mirrored to the Tk window.

```
                              HOST (your Mac/Linux desktop)
              ┌───────────────────────────────────────────────────────────┐
              │   Tk window(s)         keyboard / mouse events              │
              └────────▲───────────────────────┬──────────────────────────-┘
                       │ framebuffer mirror      │ host input
                       │ (1 byte/pixel → RGB)    │
        ┌──────────────┴─────────────────────────▼──────────────────────────┐
        │  simorisc process  (one OS-process per CPU; --display tk on WMs)   │
        │  ┌──────────────────────┐        ┌──────────────────────┐          │
        │  │ CPU pid=2  oriscwm   │        │ CPU pid=3  oriscwm   │  …        │
        │  │  • framebuffer obj   │        │  (terminal 1)        │          │
        │  │    (TAG_FRAMEBUFFER) │        └──────────────────────┘          │
        │  │  • kbd sink  kind=0  │ TAG_INPUT_SINK                            │
        │  │  • ptr sink  kind=1  │                                          │
        │  │  • per-window CONSOLE / GRID / VECTOR / RASTER services         │
        │  │  • main service @ /sys/wm/<term>/0                              │
        │  └──────────▲───────────┘                                          │
        └─────────────┼────────────────────────────────────────────────────-┘
                      │  SEND / ObjFetchBytes / replies  (Vol IV wire packets)
        ┌─────────────┼────────────────────────────────────────────────────┐
        │                       oriscbar  (the crossbar)                    │
        │     content-blind packet router; dispatches by dst_pid            │
        └──▲────────────────▲───────────────────▲───────────────────▲───────┘
           │                │                   │                   │
   ┌───────┴──────┐  ┌──────┴───────┐   ┌───────┴──────┐    ┌───────┴──────┐
   │ CPU pid=0    │  │ CPU pid=1    │   │ oriscdir     │    │ hostfsd      │
   │ supervisor   │  │ supervisor   │   │ pid=18       │    │ pid=17       │
   │ (leader)     │  │ (worker)     │   │ name→ref     │    │ host FS jail │
   │  └ sysinit   │  │              │   │ directory    │    │              │
   │    └ login   │  └──────────────┘   └──────────────┘    └──────────────┘
   │      └ shell │
   │        └ run'd guest programs (hello, edit, …)                          │
   └──────────────┘
```

Each box on the bottom row is a crossbar *port*. The crossbar knows nothing
about message contents — it routes by the `dst_pid` header field
([`tools/devices/README.md:81-84`](../tools/devices/README.md#L81)).

---

## 2. The capability / message model (the minimum you need)

Object RISC is a capability machine. The full model is in
[`docs/OBJECT_SYSTEM.md`](OBJECT_SYSTEM.md) and
[`docs/SYSTEM_FIRMWARE_INTERFACE.md`](SYSTEM_FIRMWARE_INTERFACE.md); here is just
enough to follow the WM.

### Objects, references, capabilities

An **object** is a typed blob of storage on some home CPU. A program never holds
storage directly — it holds **object references** in the 16 dedicated *object
registers* `O0..O15` (`O0` is hardwired null). A reference packs `(generation,
home CPU, table index, capability bits)`. Capability bits gate what you can do:
`R`ead, `W`rite, e`X`ecute, `S`end, deri`V`e-free, dupli`C`ate (the derive-rights
bit). Constants: [`obj.h:43-49`](../tools/cc/lib/obj.h#L43).

Firmware primitives are invoked with `CALL #<num>`; status returns in `R2`,
integer outputs in `R3/R4`, object outputs in `O1..`. The ones this subsystem
uses (simulator dispatch at [`simorisc:4456-4521`](../tools/sim/simorisc#L4456),
privilege table at [`simorisc:4410-4439`](../tools/sim/simorisc#L4410)):

| Prim | Name | What it does (regs) | Handler |
|------|------|---------------------|---------|
| `#0x100` | ObjAlloc | byte object: `R4`=len `R5`=tag `R6`=caps → `O1`=ref | [`simorisc:3267`](../tools/sim/simorisc#L3267) |
| `#0x101` | ObjFree | free `O1` (needs `V`) | [`simorisc:3805`](../tools/sim/simorisc#L3805) |
| `#0x10A` | **ObjAllocFramebuffer** | `R4`=w `R5`=h `R6`=caps `R7`=flags(bit0=offscreen) → `O1`=1-byte/pixel FB, registers a Tk window unless offscreen | [`simorisc:3282`](../tools/sim/simorisc#L3282) |
| `#0x103` | ObjDerive | weaken `O1` by mask `R4` (needs `C`) → `O1` | [`simorisc:3881`](../tools/sim/simorisc#L3881) |
| `#0x106` | ObjAllocStore | like ObjAlloc but OR-typed (OREFLD/OREFST) storage | [`simorisc:3788`](../tools/sim/simorisc#L3788) |
| `#0x108` | **ObjFetchBytes** | copy `R6` bytes from `O1`@`R4` (any home) into local `O2`@`R5` → `R3`=count | [`simorisc:3897`](../tools/sim/simorisc#L3897) |
| `#0x109` | **ObjStoreBytes** | mirror of `#0x108`, write direction (local `O1` → possibly-remote `O2`) | [`simorisc:3982`](../tools/sim/simorisc#L3982) |
| `#0x10B` | **ObjAllocInputSink** | `R4`=kind(0=kbd,1=ptr) `R5`=caps → `O1`=`TAG_INPUT_SINK` with auto-attached depth-64 queue | [`simorisc:3740`](../tools/sim/simorisc#L3740) |
| `#0x10C` | ObjBlitGlyphs | render 8×16 mono glyphs into a FB | [`simorisc:3337`](../tools/sim/simorisc#L3337) |
| `#0x10D` | ObjFillRect | fill a rect in a FB with a palette index | [`simorisc:3477`](../tools/sim/simorisc#L3477) |
| `#0x10E` | ObjFbScroll | scroll a FB sub-rect | [`simorisc:3539`](../tools/sim/simorisc#L3539) |
| `#0x10F` | ObjBlitCopy | rect-copy FB→FB (used to composite) | [`simorisc:3644`](../tools/sim/simorisc#L3644) |
| `#0x110` | MapObject | install a VA→object mapping | [`simorisc:4340`](../tools/sim/simorisc#L4340) |
| `#0x200` | InstallHandler | register a SEND handler on an object | [`simorisc:4186`](../tools/sim/simorisc#L4186) |
| `#0x203` | ReceiveQueueAttach | give `O1` a receive queue (`R4`=depth) so SENDs queue instead of dispatching a handler | [`simorisc:4081`](../tools/sim/simorisc#L4081) |
| `#0x204` | ReceiveQueuePoll | pop one message off `O1`'s queue (`R4`=timeout; `-1` blocks, `0` polls) | [`simorisc:4121`](../tools/sim/simorisc#L4121) |
| `#0x000` | TaskCreate | spawn a child task with its own address space + OPRs | [`simorisc:2758`](../tools/sim/simorisc#L2758) |
| `#0x320` | ConsoleWrite | debug: write object bytes to host stdout (bypasses the WM) | [`simorisc:3239`](../tools/sim/simorisc#L3239) |

### `SEND` and what it carries

`SEND O1` delivers a message to the service named by `O1` (the ref needs the `S`
cap). The payload is **four integer words `R4..R7`** plus **up to four object
references `O1..O4`** (`O1` is the recipient; `O2..O4` are the OR payload slots).
See [`docs/CONTRACT.md:225-232`](CONTRACT.md#L225).

If the target object has a receive queue (from `ReceiveQueueAttach`), the SEND is
**queued** rather than dispatched to a handler. The receiver later drains it with
`ReceiveQueuePoll`. Crucially, on delivery the registers are loaded **shifted by
one**: the sender's `R4..R7` arrive in the receiver's **`R3..R6`**, and the
sender's `O1..O4` arrive in `O1..O4` verbatim
([`_deliver_queue_msg`, `simorisc:4109-4118`](../tools/sim/simorisc#L4109)). This
shift is why, throughout this doc, a client that *sends* `R4=code` is read by the
receiver as `R3=code`. Keep it in mind — it is the single most confusing thing
about the wire format.

### `ReceiveQueueAttach` / `ReceiveQueuePoll`

A *service mailbox* is just an object (usually `TAG_SERVICE`) with a queue
attached. The handle-based libc wraps this: `obj_queue_attach`
([`obj.c:347`](../tools/cc/lib/obj.c#L347)), `obj_recv` (blocking) and `obj_poll`
(non-blocking) ([`obj.c:443-501`](../tools/cc/lib/obj.c#L443)). `obj_poll`
returns the four payload words as `out[0..3]` (from `R3..R6`).

### `ObjFetchBytes` — the bulk-data move

A `SEND`'s four int words can't carry a string or a pixel buffer. The convention
is: the sender puts a **segment reference** in `O2` (its boot stack ref or boot
data ref) and a **byte offset/length** in the int payload; the receiver then
calls `ObjFetchBytes` (`#0x108`) to copy those bytes out of the sender's segment
into a local buffer it controls. `ObjFetchBytes` works across CPUs — for a remote
source it issues an `OBJ_READ_REQ` and blocks until the response
([`simorisc:3955-3979`](../tools/sim/simorisc#L3955)). The bytes land **in the
caller-provided destination object**, not in registers; registers carry only
status (`R2`) and the count transferred (`R3`). This "data-send keystone" is
wrapped by `obj_send_bytes` ([`obj.h:119-128`](../tools/cc/lib/obj.h#L119)).

### The two new object types

Phase 60 added two object type tags ([`simorisc:112-113`](../tools/sim/simorisc#L112)):

- **`TAG_FRAMEBUFFER = 0x4106`** — a host-display-backed pixel object: `w*h`
  bytes, **one byte per pixel = palette index**. When allocated non-offscreen on
  a `--display tk` CPU, a Tk window opens and mirrors it. The drawing primitives
  (`#0x10C..#0x10F`) operate on it; the display worker repaints it at ~60 Hz
  whenever its `fb_dirty` flag is set.
- **`TAG_INPUT_SINK = 0x4105`** — a host-input event queue. `ObjAllocInputSink`
  makes a zero-byte object with an auto-attached depth-64 queue and registers it
  in the CPU's `input_sinks` dict keyed by `kind` (0=keyboard, 1=pointer)
  ([`simorisc:3785-3787`](../tools/sim/simorisc#L3785)). Host input is appended as
  an ordinary `SEND_DELIVER`, so the WM drains it with plain `ReceiveQueuePoll`.

> **Resolved.** Earlier Phase-60 revisions defined `TAG_FRAMEBUFFER` as `0x4104`,
> the **same value** as `TAG_TASK` — so a framebuffer could be misclassified by a
> `type_tag == TAG_TASK` branch (e.g. `ObjFree`). Commit `2df5b10` moved
> `TAG_FRAMEBUFFER` to `0x4106` ([`simorisc:111-115`](../tools/sim/simorisc#L111));
> the two tags are now distinct.

---

## 3. Boot topology

### `make boot` → `boot.sh` → `oriscrun` → `simorisc`

`make boot` ([`Makefile:81-82`](../Makefile#L81)) builds everything and runs
[`scripts/boot.sh`](../scripts/boot.sh). After rebuilding the shell with a
date-shifted banner, `boot.sh` `exec`s [`tools/oriscrun`](../tools/oriscrun) with
this exact topology ([`boot.sh:125-132`](../scripts/boot.sh#L125)):

```
exec python3 tools/oriscrun \
    --directory pid=18 \
    --hostfsd "pid=17,instance=0,root=$ROOT" \
    --cpu "pid=2:program=…/oriscwm.orx,service=0=0@0,service=0=0@0,service=0=0@0,service=18=1@9,init-r4=1,display=tk" \
    --cpu "pid=3:program=…/oriscwm.orx,service=0=0@0,service=0=0@0,service=0=0@0,service=18=1@9,init-r4=2,display=tk" \
    --cpu "pid=0:program=…/supervisor.orx,service=0=0@0,service=0=0@0,service=0=0@0,service=18=1@9,service=0=0@0,service=0=0@0" \
    --cpu "pid=1:program=…/supervisor.orx,service=0=0@0,service=0=0@0,service=0=0@0,service=18=1@9,service=0=0@0,service=0=0@0" \
    --leader 0 --leader-timeout 600
```

`oriscrun` ([main loop `tools/oriscrun:326-521`](../tools/oriscrun#L481)) launches
the processes **in this order**, waiting for each to print `READY`:

1. **`oriscbar`** — the crossbar, on a temp UNIX socket ([`oriscrun:327-345`](../tools/oriscrun#L327)).
2. **`oriscdir` (pid 18)** — the directory daemon, *first* among devices so later
   self-registrations have somewhere to land ([`oriscrun:360-385`](../tools/oriscrun#L360)).
3. **`hostfsd` (pid 17)** — host filesystem, jailed to the repo root.
4. **CPUs**, each a `simorisc --connect <sock> --pid N --service … [--init-r4 N] [--display tk] program.orx`
   ([`oriscrun:483-504`](../tools/oriscrun#L483)):
   - **pid 2, pid 3 = WM CPUs** (`oriscwm.orx`), `--display tk`, `--init-r4 1`/`2`.
     `init-r4` is the WM's **terminal index + 1** — it composes `/sys/wm/<idx>/0`
     for self-registration and per-terminal paths.
   - **pid 0 = leader supervisor**, **pid 1 = worker supervisor** (`supervisor.orx`).
5. `oriscrun` waits for the **leader** (pid 0) to exit, then tears everything down
   ([`oriscrun:510-521`](../tools/oriscrun#L510)).

Each `--cpu`'s `service=PID=INDEX@CAPS` clauses become `--service` args that
`simorisc` synthesizes into the next free `O5..O15` slot. So `service=0=0@0`
(×3) installs **null pads in O5/O6/O7**, and `service=18=1@9` installs a ref to
**oriscdir's primary mailbox (pid 18, index 1, caps `R|S`=9) in O8** — the single
irreducible wire every CPU needs to bootstrap directory walks.

> **Stale comment.** `boot.sh`'s header ([`boot.sh:30-35`](../scripts/boot.sh#L30))
> still lists "oriscterm (Tk terminal, pid 16)" among spawned processes. The
> actual `exec` spawns no oriscterm. (Lines 111-124 of the same file *do*
> correctly describe the WM-owns-terminal model.) `oriscrun`'s own docstring and
> `ouroboros/README.md`'s boot section are likewise pre-Phase-60.

There is **no `make test` / no aggregating runner** for the device tests; see
[§11](#11-testing--headless-input).

### The boot-OR register contract

When the loader instantiates a task it sets the object registers as follows
([`docs/CONTRACT.md:179-183`](CONTRACT.md#L179) and `:208-228`):

| Reg | At loader entry |
|-----|-----------------|
| `O0` | null (hardwired) |
| `O1` | **code** object (R+X, full caps) |
| `O2` | **stack** object (R+W) |
| `O3` | **data** object (R+W; null if no data segment) |
| `O4` | **own service** object — the task's "self-service" mailbox (full caps) |
| `O5..O15` | synthesized `--service` refs, in order (WM/sup boot: O5/O6/O7 = null pads, **O8 = oriscdir mailbox**) |
| `R7` | PROCID; `R4` = `init_r4` |

libc then re-homes these volatile boot registers into **stable, well-known
slots** so later code (and SENDs, which clobber `O2..O4`) can always find them.
`task_init` ([contract `liborisc.h:395-405`](../tools/cc/lib/liborisc.h#L395))
and `term_init` ([`term.c:9-21`](../tools/cc/lib/term.c#L9)) do this parking:

| Reg | Stable home after libc init | Parked by |
|-----|------------------------------|-----------|
| `O11` | boot **stack** ref | `task_init` / `term_init` (from O2) |
| `O12` | the **task-table OBJSTORE** (allocated) — holds all named O12 slots | `task_init` |
| `O13` | parent's boot **code** ref | `task_init` |
| `O14` | boot **self-service** ref | `term_init` (from O4) |
| `O15` | boot **data** ref | `task_init` / `term_init` (from O3) |

For a **terminal-using program** (after the supervisor's WM handoff, [§9](#9-the-supervisors-role)),
the surface caps live in the boot service registers
([`term.c:11-16`](../tools/cc/lib/term.c#L11), [`grid.c:21-23`](../tools/cc/lib/grid.c#L21)):

| Reg | Terminal-program convention |
|-----|-----------------------------|
| `O5` | **console** surface cap |
| `O6` | **keyboard** surface cap |
| `O7` | **grid** surface cap |
| `O8` | hostfsd reply mailbox (`hf_init`) / dir mailbox at boot |
| `O9` | private keyboard mailbox (`term_init`) / supervisor spawn mailbox |
| `O10` | hostfsd service ref |

`O12` is an **OBJSTORE** (OR-typed storage accessed by `OREFLD`/`OREFST`). All the
`NNN(o12)` byte offsets sprinkled through the libc and the WM are slots in this
per-task object store. Key ones: `BOOT_PARENT_SLOT`@544, `REPLY_MB_SLOT`@552,
`DIR_SLOT`@584, `DIR_RESULT_SLOT`@616, `WM_SLOT`@680, and the 8-entry obj-handle
table @1704 ([`obj.h:35`](../tools/cc/lib/obj.h#L35),
[`wm.c:100-106`](../tools/cc/lib/wm.c#L100), [`task.c:260-276`](../tools/cc/lib/task.c#L260)).

---

## 4. The window manager (`oriscwm.c`)

[`ouroboros/oriscwm.c`](../ouroboros/oriscwm.c) is ~5000 lines. Because the C
language can't represent capability values, *all* cap-bearing per-window state
lives in O12 objstore slots, not C globals
([`oriscwm.c:670-671`](../ouroboros/oriscwm.c#L670)); the C globals hold only
plain ints (window type, cursor position, z-order, titles). `main()` is at
[`oriscwm.c:4846`](../ouroboros/oriscwm.c#L4846).

### Responsibilities

1. **Own the local hardware** — one screen framebuffer + one keyboard sink + one
   pointer sink, allocated at boot.
2. **Serve a main control service** at `/sys/wm/<term>/0` — new-window, bind-
   surface, destroy, set-title, query-geometry, subscribe-events.
3. **Serve per-window surfaces** — a CONSOLE, GRID, VECTOR, and RASTER service
   object per window; clients SEND drawing requests to these.
4. **Composite** every window's offscreen framebuffer onto the screen
   framebuffer in z-order, draw chrome (title bar, border, close box), and run a
   desktop menu + window dragging.
5. **Route input** to the focused window's keyboard/pointer subscriber.

### Local hardware ownership

At boot, `main()` allocates ([`oriscwm.c:4881-4955`](../ouroboros/oriscwm.c#L4881)):

- **Screen framebuffer** — `alloc_local_framebuffer()` calls `#0x10A` with
  `w=FB_W=1280, h=FB_H=768, caps=R|W, flags=0` (mirror to display), stored at
  `WM_SURF_FRAMEBUFFER_SLOT_OFFSET`=440 ([`oriscwm.c:881-900`](../ouroboros/oriscwm.c#L881)).
- **Keyboard sink** — `alloc_local_keyboard_sink()` calls `#0x10B` with `kind=0`,
  `caps=31` (`R|W|S|V|C`; `V` is required for `ReceiveQueuePoll`), stored at
  `WM_KBD_EVENTS_SLOT_OFFSET`=1168 ([`oriscwm.c:836-852`](../ouroboros/oriscwm.c#L836)).
- **Pointer sink** — same with `kind=1`, stored at `WM_PTR_EVENTS_SLOT_OFFSET`=1136
  ([`oriscwm.c:854-870`](../ouroboros/oriscwm.c#L854)).

It also allocates the WM-wide keyboard/pointer *subscribe* services (TAG_SERVICE
objects clients send subscriptions to), and registers its main service at
`/sys/wm/<term>/0` via a directory walk built from `--init-r4`. Per-window
framebuffers are allocated offscreen (`#0x10A` with `flags=1`).

Geometry constants: cells are `CELL_W=8 × CELL_H=16`; the screen FB is
`1280×768`; each window's content grid is `N_COLS=80 × N_ROWS=24` and its
offscreen FB is `656×432` ([`oriscwm.c:318-374`](../ouroboros/oriscwm.c#L318)).
The window table holds at most `MAX_WINDOWS=16` ([`oriscwm.c:556`](../ouroboros/oriscwm.c#L556)).

### The main loop

The loop ([`oriscwm.c:4995-5033`](../ouroboros/oriscwm.c#L4995)) is:

```c
for (;;) {
    int status = poll_one_request(&op, &wid_or_zero, &arg);   // ReceiveQueuePoll, ~100ms timeout
    if (status == 0) {
        stash_reply_cap_o3();                  // save sender's reply mailbox (their O3)
        switch (op) {                          // WM_OP_NEW_WINDOW / BIND_SURFACE / …
            …  handle_new_window / handle_bind_surface / handle_destroy_window
               handle_subscribe_events / handle_query_geometry / handle_set_title
        }
    } else {
        scan_owner_exits();                    // timeout → auto-destroy windows whose owner exited
    }
    poll_window_consoles();   poll_window_grids();                // non-blocking drains,
    poll_window_vectors();    poll_window_rasters();              // each loops wid = 1..16
    poll_pointer_subscribes(); poll_pointer_events();
    poll_keyboard_subscribes(); poll_keyboard_events();
}
```

So each iteration **blocks up to ~100ms (`WM_POLL_TICKS`) on the main control
queue**, then **non-blocking-drains every per-window surface queue and both input
sinks** (`ReceiveQueuePoll` with `timeout=0`,
e.g. [`poll_window_consoles`, `oriscwm.c:3791`](../ouroboros/oriscwm.c#L3791)).
The per-window polls iterate all 16 window slots, skipping freed ones.

> **Stale comment.** The loop's doc comment ([`oriscwm.c:4982`](../ouroboros/oriscwm.c#L4982))
> still says step 2 will "forward writes to the underlying terminal." It renders
> locally.

### Focus model

`focused_wid` ([`oriscwm.c:972`](../ouroboros/oriscwm.c#L972)) is the window that
receives keyboard input and into whose content area pointer events route.

- **Set-on-create:** `handle_new_window` sets `focused_wid = wid` immediately
  ([`oriscwm.c:2353`](../ouroboros/oriscwm.c#L2353)), so a freshly-spawned
  program's subsequent keyboard subscribe lands in *its own* window slot.
- **Click-to-focus:** a left button-down hit-tests the topmost window at the
  click point and raises it ([`wm_handle_pointer`, `oriscwm.c:4555-4583`](../ouroboros/oriscwm.c#L4555));
  because forwarding always targets `focused_wid` and the raised window becomes
  focused, the click moves focus. `set_focus` repaints both title bars
  ([`oriscwm.c:983-991`](../ouroboros/oriscwm.c#L983)).
- **Auto-revert:** destroying a window refocuses the new topmost
  ([`oriscwm.c:1099-1107`](../ouroboros/oriscwm.c#L1099)).
- Both input polls early-return unless `focused_wid` is a live window, so input
  is gated by focus.

Compositing is covered together with the output trace in [§7](#7-output-flow-end-to-end).

---

## 5. The surface model

A **surface** is one typed drawing/input endpoint of a window. Types
([`liborisc.h:680-685`](../tools/cc/lib/liborisc.h#L680), mirrored in
[`oriscwm.c:108-113`](../ouroboros/oriscwm.c#L108)):

| `WSURF_*` | value | kind |
|-----------|-------|------|
| `WSURF_CONSOLE` | 1 | scrolling text (cursor-advancing byte stream) |
| `WSURF_KEYBOARD` | 2 | keyboard input subscription |
| `WSURF_GRID` | 3 | positioned text at (col,row), no cursor |
| `WSURF_VECTOR` | 4 | lines/rects/ovals |
| `WSURF_RASTER` | 5 | pixel-buffer blits |
| `WSURF_POINTER` | 6 | pointer/mouse input subscription |

### `wm_new_window`

A client SENDs `WM_OP_NEW_WINDOW` to the WM main service, with the owner-task ref
in `O2` (or null to opt out of auto-destroy) and a reply cap in `O3`. The WM's
`handle_new_window` ([`oriscwm.c:2218`](../ouroboros/oriscwm.c#L2218)) accepts
only `WIN_TYPE_CONSOLE`, finds a free window slot, **ObjAllocs the four
per-window surface service objects** (CONSOLE/GRID/VECTOR/RASTER, queue depth 256)
plus the offscreen window FB, assigns a cascade position, pushes the window onto
the z-stack, **takes focus**, paints title+border, composites, and replies with
`(status=0, geom_a, geom_b, wid)`. The libc wrapper is `wm_new_window`
([`wm.c:271-340`](../tools/cc/lib/wm.c#L271)); it returns the window id and the
cell-grid dimensions.

### `wm_bind_surface` — and where the cap lands

A client SENDs `WM_OP_BIND_SURFACE` with `R5=wid, R6=kind` and a reply cap in
`O3`. The WM's `handle_bind_surface` ([`oriscwm.c:2577`](../ouroboros/oriscwm.c#L2577))
loads the relevant full per-window service cap into `O1`, **derives an `R|S`
sub-cap of it** (`#0x103 ObjDerive`), and replies with that sub-cap in **`O2`**
([`oriscwm.c:2603-2623`](../ouroboros/oriscwm.c#L2603)):

```c
if (kind == WSURF_CONSOLE) {
    load_console_to_o1(wid);              /* full per-window CONSOLE cap → O1 */
    asm(... "addiu r4, r0, %1\n"          /* R | S */
        "call #0x103\n"                   /* ObjDerive → O1 = sub-cap */
        "omov o14, o1\n" ...);
    wm_reply_with_ref_o14(0);             /* reply: R3=0, O2 = the sub-cap */
}
```

GRID/VECTOR/RASTER are identical against their per-window service objects.
KEYBOARD/POINTER instead derive an `R|S` sub-cap of the **single WM-wide**
keyboard/pointer subscribe service.

On the **client side**, the libc `wm_bind_surface` ([`wm.c:346-391`](../tools/cc/lib/wm.c#L346))
polls its reply mailbox and, before restoring boot ORs, **parks the returned `O2`
into `DIR_RESULT_SLOT`@616** of its O12 objstore
([`wm.c:381`](../tools/cc/lib/wm.c#L381)). So a bound surface cap reaches a client
in the **`DIR_RESULT_SLOT` (offset 616) of O12**; the caller then `OREFLD`s it
into whatever register or slot it wants. Examples:

- `wm_open_session` ([`wm.c:406-459`](../tools/cc/lib/wm.c#L406)) binds
  console/keyboard/grid and OREFLDs each `DIR_RESULT_SLOT` straight into `O5/O6/O7`.
- `vec_init_from_dir_result` / `raster_init_from_dir_result` adopt
  `DIR_RESULT_SLOT` into an obj-handle (`wm_vec_h` / `raster_svc_h`) via
  `obj_adopt_dir_result` (both migrated — see [`OBJECT_API.md`](OBJECT_API.md)).

> **Stale comment.** The bind-handler doc ([`oriscwm.c:2565-2576`](../ouroboros/oriscwm.c#L2565))
> says CONSOLE writes are forwarded "to the underlying terminal" and keyboard is
> "passthrough." Both are obsolete; the body even notes "oriscterm is gone now"
> at the keyboard case ([`oriscwm.c:2713-2719`](../ouroboros/oriscwm.c#L2713)).

---

## 6. Input flow, end to end

Worked example: a keystroke reaching a focused program's `term_getkey`.

```
host keypress
   │
   ▼  Tk <Key> event   (TkDisplayWorker._on_key, simorisc:2385)
_key_codepoint(event) → code (printable byte, or 0x108 BS / 0x10D RET / 0x11B ESC / 0x180+ arrows / 0x190+ Fn)
   │  mods from event.state
   ▼  cpu.enqueue_input_sink(kind=0, [code, mods, 0, 0])      (simorisc:2378 → 2383)
WM CPU's keyboard TAG_INPUT_SINK queue  ← built as a self-addressed SEND_DELIVER (simorisc:1093-1125)
   │
   ▼  WM main loop: poll_keyboard_events()    (oriscwm.c:4765)
ReceiveQueuePoll(kbd sink, timeout 0) → R3=code, R4=mods
   │  if focused_wid invalid or no subscriber → drop
   ▼  SEND to focused window's keyboard subscriber  (oriscwm.c:4803-4816: r4=code, r5=mods)
client's per-program keyboard mailbox queue (O9)
   │
   ▼  term_getkey(): ReceiveQueuePoll(O9, timeout -1) → R3=code, R4=mods   (term.c:509-532)
returns the key code (mods via out param)
```

A few details that make this concrete:

- **How a client subscribes.** `term_init` ([`term.c:155-237`](../tools/cc/lib/term.c#L155))
  allocates a private mailbox in `O9` (`ObjAlloc` TAG_SERVICE, caps `R|W|S|V|C`),
  attaches a depth-**64** queue, derives an `R|S` sub-cap of it, and SENDs that
  sub-cap (in `O2`, `R4=0` = subscribe) to the keyboard service in `O6`. The WM's
  `poll_keyboard_subscribes` ([`oriscwm.c:4738`](../ouroboros/oriscwm.c#L4738))
  receives that SEND and stores the subscriber cap in **the currently-focused
  window's** `WM_KBD_SUB_BASE`@1312 slot. `pointer_subscribe`
  ([`pointer.c:51-76`](../tools/cc/lib/pointer.c#L51)) is the analog via the
  handle API: `obj_alloc` mailbox, `obj_queue_attach`, `obj_derive(R|S)`,
  `obj_send_or(wm_ptr_h, sub, 0,0,0,0)`; the WM stores it in `WM_PTR_SUB_BASE`@1440.
- **The register shift** (see [§2](#send-and-what-it-carries)): the WM *sends*
  `R4=code, R5=mods`; `term_getkey` *reads* `R3=code, R4=mods`. Verified
  end-to-end: [`oriscwm.c:4803`](../ouroboros/oriscwm.c#L4803) ↔
  [`term.c:518-520`](../tools/cc/lib/term.c#L518).
- **Pointer events** carry `[evt_type, packed_xy, button, btn_state]`; the WM
  first runs `wm_handle_pointer` (drag/menu/close-box/raise) and only forwards
  *content-area-local* events to the focused window's pointer subscriber
  ([`poll_pointer_events`, `oriscwm.c:4647-4727`](../ouroboros/oriscwm.c#L4647)).
  `pointer_getevent` ([`pointer.c:99-115`](../tools/cc/lib/pointer.c#L99)) reads
  them via `obj_poll`.
- **Headless path:** with no Tk window, a test injects a `PKT_HOST_INPUT` crossbar
  packet whose payload is `[kind, w0, w1, w2, w3]`; `simorisc` calls the *same*
  `enqueue_input_sink(kind, [w0..w3])` ([`simorisc:1082-1089`](../tools/sim/simorisc#L1082)),
  so the WM cannot tell injected input from a real keypress
  ([`simorisc:1099-1102`](../tools/sim/simorisc#L1099)). The CPU is selected by
  the packet's `dst_pid`; `kind` selects keyboard vs pointer sink. See
  [§11](#11-testing--headless-input).

---

## 7. Output flow, end to end

Worked example: drawing pixels via `raster_blit`, plus the text and grid paths.

```
client raster_blit(packed_xy, packed_wh, pixels)            (raster.c:42-63)
   │  pick boot stack(O11) or data(O15) segment by the buffer's VA; compute byte_offset
   ▼  obj_send_bytes(raster_svc, src, reply=NULL, RST_OP_BLIT, packed_xy, packed_wh, byte_off)   (obj.c:411)
SEND to per-window RASTER service:  O1=service, O2=src segment, O3=null, R4..R7=(op,xy,wh,off)
   │
   ▼  WM main loop: poll_window_rasters()  →  forward_raster_write()   (oriscwm.c:4099 → 4013)
delivered as R3=op, R4=packed1, R5=packed2, R6=byte_off, O2=source pixel ref
   │  per row:
   │    ObjFetchBytes(#0x108): O2 source @ off → local scratch row   (oriscwm.c:4045-4070)
   │    ObjStoreBytes (#0x109): scratch row → the window's offscreen FB
   ▼  composite_content_area(): ObjBlitCopy(#0x10F) window FB → screen FB, in z-order   (oriscwm.c:1300, 1224)
screen framebuffer (TAG_FRAMEBUFFER, flags=0)
   │
   ▼  TkDisplayWorker.tick(): on fb_dirty, palette-LUT → PPM → PhotoImage   (simorisc:2420-2457, ~60Hz)
pixels on the host Tk window
```

The **text (console)** and **grid** paths share the same shape but render glyphs
rather than raw pixels:

- **`term_print`** ([`term.c:326-336`](../tools/cc/lib/term.c#L326)) picks the
  stack/data segment by the string's VA and SENDs to the console service `O5`
  with `O2`=segment, `R4`=offset, `R5`=count. The WM's `forward_console_write`
  ([`oriscwm.c:3625`](../ouroboros/oriscwm.c#L3625)) `ObjFetchBytes`es up to 1024
  bytes into a stack buffer *before* replying (so the client's stack source stays
  valid), then `render_buffer` → `flush_strip` blits glyphs with `#0x10C
  ObjBlitGlyphs` (font via boot data `O15`, text via boot stack `O3`) and
  composites the strip. Console is a **raw byte stream with no op code**; control
  bytes like `\f` (clear) and `\b` (backspace) are interpreted inline. Cursor
  overflow scrolls via `#0x10E ObjFbScroll`.
- **`grid_print_n`** ([`grid.c:97-107`](../tools/cc/lib/grid.c#L97)) SENDs to grid
  service `O7` with `O2`=segment, `R4`=offset, `R5`=count, `R6`=col, `R7`=row;
  `col=row=-1` is the clear-all sentinel. The WM's `forward_grid_write`
  ([`oriscwm.c:3730`](../ouroboros/oriscwm.c#L3730)) fetches the bytes and
  positions them with no cursor advance.
- **`vec_*`** carry their whole payload in the int words (no `ObjFetchBytes`):
  `R4=op, R5=packed1, R6=packed2` → `forward_vector_write`
  ([`oriscwm.c:3907`](../ouroboros/oriscwm.c#L3907)) decodes `VEC_OP_*` and
  rasterizes lines/rects/ovals into the window FB.

**Compositing.** Each window paints into its own **offscreen** FB; the WM then
`ObjBlitCopy`-composites the dirty region of each window onto the **screen** FB
in z-order (`composite_screen_rect` [`oriscwm.c:1224`](../ouroboros/oriscwm.c#L1224)),
adding chrome (title bar, border, close box) and the desktop menu when active.
There is no explicit "present" call — because the screen FB was allocated
non-offscreen, every `ObjBlitCopy` into it is mirrored to the Tk window by the
host display worker with no wire round-trip ([`oriscwm.c:872-880`](../ouroboros/oriscwm.c#L872)).

> **Stale comment.** `forward_console_write`'s doc ([`oriscwm.c:3587-3623`](../ouroboros/oriscwm.c#L3587))
> describes a four-step "re-emit the SEND to the underlying terminal CONSOLE"
> dance; the body itself notes "with oriscterm gone, the WM IS the CONSOLE
> receiver" ([`oriscwm.c:3672`](../ouroboros/oriscwm.c#L3672)).

> **⚠ Pitfall: async buffer lifetime — corruption that masquerades as a compiler bug.**
> The data SENDs above (`term_print`, `grid_print_n`, `raster_blit`,
> `obj_send_bytes`) are **fire-and-forget**: the receiver issues its
> `OBJ_READ_REQ`/`ObjFetchBytes` for the payload *after* the sending CPU has moved
> on, and there is **no ack** in the wire protocol. So the source buffer must stay
> alive **and unchanged** until that async fetch. A buffer on a **stack frame that
> a later call pops and reuses** — especially a deeper/blocking call like
> `term_getkey` → `ReceiveQueuePoll` — is overwritten before the fetch, and the
> receiver pulls garbage. Because the *caller's* source didn't change (only a
> callee got deeper) and adding a debug print shifts the stack/timing, this looks
> exactly like a non-deterministic **compiler / register-allocation Heisenbug** —
> it is not. **Diagnose** it by `print_str`-ing the buffer right before the SEND:
> correct in memory there but corrupt in the render ⇒ this race. **Fix** it by
> keeping the buffer alive past the fetch — a sync variant that waits for an ack
> (`term_print_n_sync`), a persistent caller frame (never a transient callee
> local), or a `const` data-segment table (term.c's single-char table). This bit
> the Phase-4 `term.c` migration: `cmd_view`'s status line, async-sent from
> `view_render`'s frame, was clobbered by the next `term_getkey`; the fix moved
> the buffer into `cmd_view`'s persistent loop frame.

---

## 8. The libc client APIs

All live under [`tools/cc/lib/`](../tools/cc/lib). Wire op constants are in
[`liborisc.h`](../tools/cc/lib/liborisc.h); the WM-side decoders are in
`oriscwm.c`. Several clients were recently migrated onto a **handle-based object
API** ([`obj.h`](../tools/cc/lib/obj.h)/[`obj.c`](../tools/cc/lib/obj.c)) because
the v1 C compiler cannot hold a capability *value* across a call — so capabilities
stay in an 8-slot table in O12 (offset 1704) and the program holds opaque integer
handles ([`obj.h:1-20`](../tools/cc/lib/obj.h#L1)). The handle API and the
client-migration pattern have their own guide:
[`docs/OBJECT_API.md`](OBJECT_API.md) (authoritative for migration status).

### `obj.{h,c}` — the handle layer

`obj_alloc`/`obj_alloc_store`/`obj_derive`/`obj_free` wrap `#0x100/#0x106/#0x103/#0x101`;
`obj_queue_attach` wraps `#0x203`; `obj_recv`/`obj_poll` wrap `#0x204`. The two
SEND wrappers that matter:

- **`obj_send_or(h, or_h, a0..a3)`** — SEND with `O2`=`or_h`'s cap (or null), used
  to hand a service a sub-cap of your mailbox (subscribe). [`obj.c:387`](../tools/cc/lib/obj.c#L387).
- **`obj_send_bytes(svc, src, reply, a0..a3)`** — the data-send keystone: `O2`=
  boot stack/data segment (so the service can `ObjFetchBytes` it), `O3`=reply cap
  or null, `R4..R7`=`a0..a3`. [`obj.c:411`](../tools/cc/lib/obj.c#L411).

### `term.c` — console + keyboard

Targets `O5` (console) and `O6` (keyboard); private mailbox in `O9`.

| Function | Emits |
|----------|-------|
| `term_init` | alloc O9 mailbox + queue (depth 64) + subscribe to keyboard (SEND O6, O2=sub-cap, R4=0) |
| `term_print` / `term_print_n` | console SEND (O5): O2=segment, R4=offset, R5=count |
| `term_print_char` / `term_print_int` / `term_print_hex` | one or more console SENDs via a static 256-byte table |
| `term_clear` | console SEND of `\f` |
| `term_getkey` / `term_pollkey` | `ReceiveQueuePoll(O9)` blocking / non-blocking → R3=code, R4=mods |
| `term_shutdown` / `term_resubscribe` | keyboard SEND with R4=1 (unsub) / R4=0 (re-sub) |

> The **keyboard path is migrated onto `obj.h`**: the private mailbox is now an
> `obj_t` handle (`kbd_mbox_h` via `obj_alloc`, not `O9`), the keyboard service is
> `obj_adopt_o6()`, and `term_getkey` is `obj_recv_full` — so the `O9` /
> `ReceiveQueuePoll(O9)` details in the table above predate the migration (see
> [`OBJECT_API.md`](OBJECT_API.md)). The **console path (`term_print*`) is still
> raw asm** to `O5` ([`term.c:33-34`](../tools/cc/lib/term.c#L33)). Header comments
> still say "oriscterm" (read "the WM"); the queue is depth 64 (not the "depth 16
> ... shared with hostfsd" the old comment claims — hostfsd uses O8).

### `grid.c` — positioned text

`grid_print` / `grid_print_n` / `grid_clear`, all to grid service `O7`. Wire:
`O2`=segment, `R4`=offset, `R5`=count, `R6`=col, `R7`=row; `col=row=-1` clears.
Still wire-asm; comments say "oriscterm grid (idx 3), currently 80×24" and "the
terminal pulls the bytes via OBJ_READ_REQ" — read "the WM" and "`ObjFetchBytes`."

### `vector.c` — `VEC_OP_*` drawing

Migrated onto `obj.h` (Phase 4). `vec_init_from_dir_result` adopts the WM-mediated
VECTOR cap from the dir-walk result into a handle (`wm_vec_h`); each helper
`obj_send`s `R4=op, R5=packed1, R6=packed2` (the old `WM_VECTOR_CAP_SLOT` hand-copy
is gone). Ops ([`liborisc.h:212-218`](../tools/cc/lib/liborisc.h#L212)): `LINE`=0,
`RECT_FILL`=1, `RECT_OUTLINE`=2, `OVAL_FILL`=3, `OVAL_OUTLINE`=4, `CLEAR`=5,
`SET_COLOR`=6. Coordinates packed as two signed-16 halves per word. Vector ops
carry their whole payload in the int words, so this client is immune to the §7
async-buffer pitfall.

### `raster.c` — `RST_OP_*` blits

Migrated onto `obj.h`. `raster_blit(packed_xy, packed_wh, pixels)` calls
`obj_send_bytes(raster_svc, src, NULL, RST_OP_BLIT, packed_xy, packed_wh,
byte_off)` — `O2`=pixel-buffer segment for the WM to `ObjFetchBytes`. `raster_clear`
sends `RST_OP_CLEAR` with no byte source. Ops: `BLIT`=0, `CLEAR`=1
([`liborisc.h:253-254`](../tools/cc/lib/liborisc.h#L253)).

### `pointer.c` — mouse subscription

Migrated onto `obj.h`. `pointer_subscribe` allocs an event mailbox, attaches a
queue, derives an `R|S` sub-cap and `obj_send_or`s it to the WM pointer service;
`pointer_getevent` `obj_poll`s `[evt_type, packed_xy, button, btn_state]`;
`pointer_unsubscribe` sends a null `O2`. The old O10 boot-OPR mailbox was
reclaimed by the migration ([`pointer.c:1-18`](../tools/cc/lib/pointer.c#L1)).

### `wm.c` — the WM control protocol

Lazy-inits `WM_SLOT`@680 by walking `/sys/wm/<my_term>/0`
([`wm.c:214-239`](../tools/cc/lib/wm.c#L214)); each op is a synchronous SEND +
poll on `REPLY_MB_SLOT`@552. Ops ([`wm.c:71-76`](../tools/cc/lib/wm.c#L71), must
match `oriscwm.c`): `NEW_WINDOW`=1, `BIND_SURFACE`=2, `DESTROY_WINDOW`=3,
`SUBSCRIBE_EVENTS`=4, `QUERY_GEOMETRY`=5, `SET_TITLE`=6. Public functions:
`wm_init`, `wm_new_window`, `wm_bind_surface`, `wm_open_session`,
`wm_destroy_window`, `wm_get_geometry`, `wm_set_title`, `wm_subscribe_events`.
This is the one file whose comments are *current* — it correctly documents that
on no-WM systems `wm_init` returns `WM_NO_WM` and callers fall back to boot-OPR
surfaces.

---

## 9. The supervisor's role

[`ouroboros/supervisor.c`](../ouroboros/supervisor.c) is each CPU's init process;
`main()` is at [`supervisor.c:1788`](../ouroboros/supervisor.c#L1788). The leader
(PROCID 0) drives all visible activity; workers sit in a dispatch loop servicing
relayed spawn requests.

**It does *not* do a single "try `/sys/term/0`, else `wm_init`" fallback.**
Instead `main()` runs two sequential blocks:

1. **Direct-terminal walk (expected to fail under Phase 60).** It walks
   `/sys/term/<procid>/{console,keyboard,grid}` and, on success, OREFLDs the
   resolved refs into `O5/O6/O7` ([`supervisor.c:1880-1901`](../ouroboros/supervisor.c#L1880)).
   In a normal WM boot **nothing publishes `/sys/term/<n>/*`**, so these walks
   retry a few times, time out with NOT_FOUND, and the OREFLDs never execute —
   `O5/O6/O7` keep their (null) boot values. This is the "expected failure."
2. **WM session establishment (the path that actually wins).** It then calls
   `wm_init` (retrying on NOT_FOUND) and, on success, calls `maybe_lazy_wm_bind`
   ([`supervisor.c:1939-1950`](../ouroboros/supervisor.c#L1939)).

`maybe_lazy_wm_bind` ([`supervisor.c:790-865`](../ouroboros/supervisor.c#L790))
establishes the session:

- `wm_new_window(WIN_TYPE_CONSOLE)` with a null owner ref (the supervisor is a
  long-running daemon — no auto-destroy).
- `wm_bind_surface(wid, WSURF_CONSOLE)` → OREFLDs `DIR_RESULT_SLOT` into **`O5`**,
  caches it in `WM_LEADER_CONSOLE_SLOT`@696, and `dir_register`s it at
  `/sys/wm/<term>/leader-console`.
- `wm_bind_surface(wid, WSURF_KEYBOARD)` → OREFLDs into **`O6`** (not cached, not
  registered).
- `wm_bind_surface(wid, WSURF_GRID)` → OREFLDs into **`O7`**, caches in
  `WM_LEADER_GRID_SLOT`@704, registers at `/sys/wm/<term>/leader-grid`.
- `wm_set_title(wid, "Terminal N")`.

By **overwriting the boot `O5/O6/O7` in place** with the WM-mediated caps, the
supervisor makes surface inheritance automatic.

**Surface inheritance via TaskCreate's OPR copy.** The supervisor spawns the
`sysinit → login → shell` chain through `sup_spawn_named → orx_spawn → TaskCreate`.
`TaskCreate` copies the parent's object registers into the child verbatim
([`supervisor.c:2049-2058`](../ouroboros/supervisor.c#L2049) for sysinit,
`:2092-2141` for login). Since the supervisor already overwrote `O5/O6/O7` with
WM caps, **the child inherits the WM console/keyboard/grid surfaces with no
per-spawn work** — this is the dominant path in a normal boot. (`login.orx`, not
the supervisor, spawns the shell.) A secondary per-spawn path exists for relayed
`run @N` / hot-attach: `populate_child_term_slots` fills dedicated child-override
slots `ORX_SLOT_CHILD_O5/O6/O7`@632/648/664 (and `O8`@560 = the spawn-service
sub-cap so children can SEND back), which `orx_task_create` swaps into the child's
OPRs just before `TaskCreate` ([`supervisor.c:890-964`](../ouroboros/supervisor.c#L890)).

> **Asymmetry worth knowing.** In the per-spawn path, keyboard is resolved only
> from the direct `/sys/term/<idx>/keyboard` path
> ([`supervisor.c:927`](../ouroboros/supervisor.c#L927)), which doesn't resolve
> under Phase 60; relayed children therefore fall back to the inherited `O6`.
> Console/grid prefer `/sys/wm/<idx>/leader-*` first. Many `oriscterm`/`/sys/term`
> comments in this file are stale (e.g. [`:2060-2073`](../ouroboros/supervisor.c#L2060),
> [`:1011-1018`](../ouroboros/supervisor.c#L1011)); no `oriscterm.orx` is ever
> spawned.

---

## 10. The simulator's role (`tools/sim/simorisc`)

[`tools/sim/simorisc`](../tools/sim/simorisc) is a pure-Python ISA simulator; one
process = one CPU. In a multi-process boot it connects to an external `oriscbar`
crossbar with `--connect <socket> --pid N` (the README's `--bar` is a misnomer —
there is no `--bar` flag; it is `--connect`).

**There is no special "WM CPU" in the simulator.** A CPU *is* the WM only because
it loaded `oriscwm.orx` and allocated framebuffer/input-sink objects; the sim
treats all CPUs identically apart from `pid` and whether `--display tk` gave it a
`display_worker` ([`simorisc:5342-5354`](../tools/sim/simorisc#L5342)).

- **`TAG_INPUT_SINK` + `enqueue_input_sink`.** `ObjAllocInputSink`
  ([`simorisc:3740`](../tools/sim/simorisc#L3740)) registers a sink in
  `cpu.input_sinks[kind]`. `CPU.enqueue_input_sink(kind, [w0..w3])`
  ([`simorisc:1093`](../tools/sim/simorisc#L1093)) builds a self-addressed
  `SEND_DELIVER` carrying that 4-word int payload and appends it to the sink's
  queue (silent-dropping if full). Both the live Tk path and the headless path
  funnel through this one method.
- **The Tk display worker** (`class TkDisplayWorker`,
  [`simorisc:2261`](../tools/sim/simorisc#L2261)). `--display tk` attaches one per
  CPU; `System.run` ticks it. `register_framebuffer` (called from
  `ObjAllocFramebuffer`) opens a `Toplevel` + `Canvas` sized exactly to the FB —
  **1 host pixel per FB byte, no scaling**. `_on_key`/`_on_pointer` translate Tk
  events (key codepoints at [`simorisc:2204-2240`](../tools/sim/simorisc#L2204):
  `BACKSPACE`=0x108, `RETURN`=0x10D, `ESCAPE`=0x11B, arrows 0x180–0x183, F1=0x190)
  and call `enqueue_input_sink`.
- **Framebuffer mirroring.** `tick` is throttled to ~60 Hz
  ([`simorisc:2432`](../tools/sim/simorisc#L2432)); for each FB whose `fb_dirty`
  is set it builds an RGB blob through a 256-entry palette LUT, wraps it as a PPM,
  and feeds a `PhotoImage` ([`_repaint`, `simorisc:2420`](../tools/sim/simorisc#L2420)).
  No wire round-trip — the bytes are in-process.
- **Headless `PKT_HOST_INPUT` (`0x50`).** Its handler
  ([`simorisc:1082-1089`](../tools/sim/simorisc#L1082)) reads a 5-word payload
  `[kind, w0, w1, w2, w3]` and calls `enqueue_input_sink(kind & 0xFF, [w0..w3])`.
  CPU targeting is by the packet's `dst_pid`; `kind` (`p[0]`) selects keyboard vs
  pointer. Byte-identical to live input.

---

## 11. Testing & headless input

The headless harness is
[`tools/devices/tests/fake_terminal.py`](../tools/devices/tests/fake_terminal.py)
— a Python stand-in for the Tk terminal that connects to the crossbar like a
device. Its two relevant modes:

- **Surface device** (default, `--directory-pid` set): self-registers
  console/keyboard/grid/pointer/framebuffer under `/sys/term/<instance>/*` and
  serves them — used by older direct-terminal and the vector/raster smoke tests
  (where it is the passive framebuffer backend).
- **`--inject-cpu N`** (the Phase-60 path): injects host input straight into CPU
  `N`'s input sink via `PKT_HOST_INPUT`, because the WM owns the input surfaces
  and there is no terminal keyboard to subscribe to. `build_host_input`
  ([`fake_terminal.py:103-111`](../tools/devices/tests/fake_terminal.py#L103))
  builds the 5-word `[kind, w0..w3]` packet; `send_key`
  ([`fake_terminal.py:492-500`](../tools/devices/tests/fake_terminal.py#L492))
  injects keyboard as `kind=0, [code, mods, 0, 0]`; `_send_pointer` injects
  `kind=1, [evt, packed_xy, button, state]`. `--event key:A`, `motion:X,Y`,
  `down:X,Y,BTN`, `up:…`, `sleep:N`, `wait-kbd:N` script the sequence.

**How a headless test drives input with no Tk window:** it launches the CPUs
*without* `--display tk` (so no window), waits for a readiness marker on the CPU's
stdout, then runs `fake_terminal.py --inject-cpu <wm-pid>` to push `PKT_HOST_INPUT`
packets at the WM CPU. Those land in the same input sink the Tk worker would have
fed, so the WM and client code run unchanged.

The named tests (run each script directly — there is **no** `make test` or
aggregating runner):

| Test | Topology | Input | Asserts |
|------|----------|-------|---------|
| [`test_wm_boot.sh`](../tools/devices/tests/test_wm_boot.sh) | xbar + oriscdir(18) + hostfsd(17) + **WM on CPU 2** (`--init-r4 1`) + supervisor leader CPU 0; **no terminal device** | `--inject-cpu 2`: RET, `run /programs/hello.orx`, RET, `exit` | `WM-mediated session (term=0`, `hello-from-wm-session`, `shell exited; halting` |
| [`test_ptr_smoke.sh`](../tools/devices/tests/test_ptr_smoke.sh) | xbar + oriscdir + WM on CPU 0 (`--init-r4 1`) + `ptr_smoke` on CPU 1 | `--inject-cpu 0`: motion/down/motion/up/motion | `pointer mediation ready`, `ptr_smoke: PASS` |
| [`test_raster_smoke.sh`](../tools/devices/tests/test_raster_smoke.sh) | xbar + oriscdir + fake_terminal surface(16) + WM CPU 0 + `raster_smoke` CPU 1 | none (program self-drives `raster_blit`/`raster_clear`) | `oriscwm: ready`, `raster_smoke: PASS` |
| [`test_vec_smoke.sh`](../tools/devices/tests/test_vec_smoke.sh) | same shape as raster | none (program self-drives `vec_*`) | `oriscwm: ready`, `vec_smoke: PASS` |
| [`test_shell_view.sh`](../tools/devices/tests/test_shell_view.sh) | xbar + hostfsd + fake_terminal(16) + shell CPU 0 (O5/O6/O7=term, O10=hostfsd) | direct-terminal SENDs: `view greeting.txt`, jjj, q, exit | grid "last frame" rows + `/> ` prompt returns |
| [`test_kbd_echo.sh`](../tools/devices/tests/test_kbd_echo.sh) | xbar + fake_terminal(16) + `kbd_echo` CPU 0 (O5=console, O6=keyboard) | direct-terminal: A, B, ESC | `key=65 'A'`, `key=66 'B'`, `ESC — exiting` |

So `test_wm_boot.sh` and `test_ptr_smoke.sh` are the worked examples of the *full*
Phase-60 wiring (input injected into the WM CPU's sink); `test_kbd_echo.sh` and
`test_shell_view.sh` exercise the *legacy direct-terminal* path (fake_terminal as
a subscriber/surface device).

---

## 12. Evolution / phase history

A reader hitting odd-looking code will usually find a phase tag explaining it.
The major arc (from `oriscwm.c` phase markers and `docs/HISTORY.md`):

| Phase | Name | What changed |
|-------|------|--------------|
| 38–40 | (pre-WM) | interactive guests sharing one keyboard; F1 focus switching on `oriscterm` |
| 49/51 | terminal pass-through | supervisor injects `/sys/term/<N>/*` console/keyboard/grid into spawned children's OPRs; `terminal_idx` propagation |
| 58 | **WM β** | WM allocates a *per-window* CONSOLE service so client console writes land in a per-window queue ([`oriscwm.c:166`](../ouroboros/oriscwm.c#L166)) |
| 59 | **WM γ** | the framebuffer arrives; the WM rasterizes locally. γ.9 adds the GRID service, γ.11 the VECTOR service (`VEC_OP_*`), γ.12 the RASTER service, γ.13 pointer mediation, γ.15 multi-WM (one WM per terminal, `/sys/wm/<N>` paths from `--init-r4`) ([`oriscwm.c:168`](../ouroboros/oriscwm.c#L168)) |
| 60 | **terminal-firmware unification** | the WM *becomes* the terminal. **step 2:** allocate the framebuffer locally (`#0x10A`, originally `#0x102`), drop the `/sys/term/<N>/framebuffer` walk. **step 3:** local keyboard/pointer input sinks (`#0x10B`); oriscterm no longer mediates input. **steps 5–22:** local rect-fill/scroll, offscreen per-window backing stores, title bars + `SET_TITLE`, z-order + window positioning (lifting the 1-window cap), borders, outline dragging, the **focus model**, the desktop root menu, the close box. |

The practical upshot for a reader: the heavy stack of per-surface services
(CONSOLE/GRID/VECTOR/RASTER) is WM-β/γ scaffolding; the "WM owns the hardware and
is the terminal" reality — and the resulting wave of now-stale "underlying
terminal" comments — is all Phase 60.

---

## Open questions / things to verify

- **Keyboard binding asymmetry** in the supervisor's per-spawn path
  ([`supervisor.c:927`](../ouroboros/supervisor.c#L927)): relayed/hot-attach
  children resolve keyboard only from `/sys/term/<idx>/keyboard`, which won't
  resolve under Phase 60. They fall back to the inherited `O6`, which is correct
  for the common case, but means a hot-attached terminal's keyboard wiring relies
  entirely on OPR inheritance — worth confirming under a real multi-terminal
  hot-attach.
