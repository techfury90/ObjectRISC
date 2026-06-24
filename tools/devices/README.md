# Object RISC device processes

External devices that participate in the Object RISC system as ports
on the crossbar — same wire format as CPUs, different innards. A
device is just another process that opens a UNIX socket to
[`oriscbar`](../sim/oriscbar), performs a small handshake, and
exchanges packets per Volume IV.

The architecture spec ([Vol IV §2](../../INTERCONNECT_PROTOCOL.md))
treats the crossbar as a flat fabric of processor ports. Devices
were not part of the original spec — every port was a CPU. The
device model is a deliberately small extension: a device announces
itself with a `pid` like any CPU, presents one or more *service
objects* at fixed `(index, generation, caps)` tuples, and is
addressed by SENDs to those objects. CPUs see no distinction between
a remote CPU and a remote device — both are reachable via SEND, and
any object the device hosts is read back via the same `OBJ_READ_REQ`
round-trip a remote `OL*` would use.

## Wire layer

Three layers stack on top of each other; the architecture spec
covers the top one, the implementation owns the lower two.

### 1. Architectural packet (Vol IV §3)

Every packet on the wire is

```
[ 64-bit header ][ length × 32-bit payload words ][ 32-bit XOR checksum ]
```

The header carries `(src_pid, dst_pid, type, flags, trans_id,
length_words)` in the bit positions given in Vol IV §3.1. The
checksum is the bitwise XOR of every header and payload word — a
mismatch means the receiver discards the packet and the source
times out (Vol IV §9). Big-endian throughout.

Message types relevant to devices:

| Code   | Name               | Used for                                   |
|--------|--------------------|--------------------------------------------|
| `0x10` | `OBJ_READ_REQ`     | Device fetches bytes from a remote object  |
| `0x11` | `OBJ_READ_RESP`    | Reply with the bytes (or a fault flag)     |
| `0x20` | `SEND_DELIVER`     | A CPU invokes the device's service object  |

`SEND_DELIVER` carries 14 payload words: 2 for the recipient ref,
4 for the integer payload (`R4`–`R7` at the issuing CPU), and 8
for the four-OR payload (`O1`–`O4` at the issuing CPU). See Vol IV
§4.2 for the canonical layout.

### 2. Length-prefixed framing (implementation)

Vol IV is silent on how packets are demarcated on a particular
transport. On UNIX sockets the architecture's "one packet per
arbitration cycle" is replaced with explicit framing: each packet
is preceded by a 4-byte big-endian length giving the size of the
packet body that follows. The receiver reads 4 bytes, then the
indicated number of bytes, then loops.

```
[ 4B length ][ 64-bit header ][ payload words ][ 32-bit checksum ]
\__________/  \_______________________________________________/
   framing                  Vol IV §3 packet
```

### 3. Handshake (implementation)

Before sending any packet, the device opens the socket and exchanges
a fixed 8-byte handshake to claim its `pid`:

```
client → server: HELLO_MAGIC (0xC0FFEEAA, 4B BE) | pid (4B BE)
server → client: HELLO_MAGIC (0xC0FFEEAA, 4B BE) | status (4B BE)
                 status: 0x00 = OK
                         0x01 = pid in use
                         0x02 = bad magic
```

A client that receives a non-OK status closes immediately. The
crossbar (`oriscbar`) is otherwise content-blind: it knows nothing
about the meaning of any packet, only the `dst_pid` field in the
header, which it uses to route bytes to the right registered
connection. CPUs and devices share this discipline equally.

## oriscterm

> **Legacy / vestigial as of Phase 60 — not used in a WM-mediated boot.**
> The window manager now owns the framebuffer and the keyboard/pointer
> input sinks *inside its own CPU* (firmware primitives `#0x102
> ObjAllocFramebuffer` and `#0x10B ObjAllocInputSink`) and **is** the
> terminal; `scripts/boot.sh` (the `make boot` path) launches no
> `oriscterm` — only `oriscwm` CPUs ([`boot.sh:125-131`](../../scripts/boot.sh#L125)).
> See [`docs/wm-terminal-overview.md`](../../docs/wm-terminal-overview.md)
> for the current architecture. `oriscterm` survives because:
>
> - It is the **reference Tk implementation** of the over-the-wire
>   console/keyboard/grid/vector/pointer/framebuffer protocol documented
>   below, and several **direct-terminal demos** still drive it — all via
>   `oriscrun --terminal pid=16` ([`oriscrun:40,233`](../oriscrun#L40)):
>   [`run_hello_terminal.sh`](../../examples/run_hello_terminal.sh),
>   [`run_kbd_echo.sh`](../../examples/cc/run_kbd_echo.sh),
>   [`run_paint.sh`](../../examples/cc/run_paint.sh), and
>   [`run_parallel_primes.sh`](../../examples/run_parallel_primes.sh).
> - The protocol below is the spec that the WM's per-window surface
>   services reimplement locally.
>
> Note: `test_framebuffer.sh` exercises the framebuffer `OBJ_READ/WRITE`
> protocol against [`fake_terminal.py`](tests/fake_terminal.py) (a headless
> stand-in that implements those handlers itself,
> [`fake_terminal.py:208-286`](tests/fake_terminal.py#L208)), **not** the
> real `oriscterm` — its own in-script comment claiming "only oriscterm"
> implements them is stale.

A graphical terminal device. Opens a Tk window, connects to the
crossbar at a chosen pid (typically `16+` to avoid clashing with
CPU pids), and exposes two *service objects*: a text console and a
keyboard subscription endpoint. Each is a separate capability;
CPUs hold a ref to whichever they want to use. Both live at
generation `1` and grant `R|W|S|V|C` (`0x5B`) when the launcher
synthesizes refs to them.

| Index | Service     | What it does                                                |
|-------|-------------|-------------------------------------------------------------|
| `1`   | **console** | CPU SENDs here to append text to the scrolling text pane    |
| `2`   | **keyboard**| CPU SENDs here to subscribe to (or unsubscribe from) keystroke events |
| `3`   | **grid**    | CPU SENDs here to write text at a fixed (col, row) on the graphics canvas |
| `4`   | **vector**  | CPU SENDs here to draw lines, rectangles, ovals on the graphics canvas |
| `5`   | **raster**  | (protocol pinned, implementation deferred) bitmap blit      |
| `6`   | **pointer** | CPU SENDs here to subscribe to (or unsubscribe from) pointer / mouse events |

The terminal window has two regions: a scrolling text pane on top
(driven by service `1`) and a fixed-size graphics canvas below
(driven by services `3` and `4`). Both share the same monospace
font, so the grid service's character cells line up visually with
the text pane's columns.

Usage:

    python3 tools/devices/oriscterm --socket /tmp/oriscbar.sock --pid 16

Or, more typically, via the launcher:

    python3 tools/oriscrun \
        --terminal pid=16 \
        --cpu pid=0:program=examples/hello_terminal.orx,service=16=1@9

The `--service 16=1@9` clause synthesizes a `R|S = 0x09` (read +
send-permitted) capability on the terminal's console object and
installs it into the CPU's next free `O5..O15` slot at boot.

For interactive programs that want both output AND keyboard input,
pass both services:

    python3 tools/oriscrun \
        --terminal pid=16 \
        --cpu "pid=0:program=demo.orx,service=16=1@9,service=16=2@9"

The CPU then sees the console at `O5` and the keyboard at `O6`
(slots are filled in `--service` order). See the keyboard section
below for the subscribe/event protocol, and
[`examples/cc/run_kbd_echo.sh`](../../examples/cc/run_kbd_echo.sh)
for a worked C demo.

### The console-write protocol

A CPU writes to the terminal in two phases. The terminal does not
hold the bytes itself — they live in the sender's data segment (or
stack, or any byte-typed object) — so a SEND to the console
delivers a *reference* and the terminal pulls the bytes back over
the wire.

**Phase 1: CPU SENDs a write request.** The CPU stages a SEND with
the following convention:

| Slot                 | Meaning                                                |
|----------------------|--------------------------------------------------------|
| `O1` (recipient)     | The console-object reference itself                    |
| `O2` (data ref)      | The byte-typed object holding the text to write        |
| `O3` (reply cap)     | An optional reply object to ack on; `0` = no ack       |
| `R4` (offset)        | Byte offset into the data object where the text starts |
| `R5` (length)        | Number of bytes to write                               |
| `O4`, `R6`, `R7`     | Unused; should be zero                                 |

The CPU executes `SEND O1`. The crossbar serializes this as a
`SEND_DELIVER` packet aimed at the terminal's pid. (The wire
mapping isn't quite symmetric: per Vol IV §4.2 the four wire OR
slots map to the handler's `O1..O4`, but `O1` is overridden with
a fresh self-reference at delivery, which matters for handler
authoring but not for the convention above — the terminal looks
at wire OR slot 1 for the data ref regardless.)

**Phase 2: Terminal pulls the bytes.** On receipt of the
`SEND_DELIVER`, the terminal:

1. Parses the SEND payload, extracting the data ref, reply cap,
   offset, and length.
2. Sends an `OBJ_READ_REQ` to the data ref's home `pid` (decoded
   from bits 47:40 of the reference) requesting `length` bytes
   starting at `offset`.
3. Waits for the corresponding `OBJ_READ_RESP` (matched by
   `trans_id`).
4. Decodes the response data words back to bytes and appends them
   to the on-screen text widget.
5. If a reply cap was provided, sends a header-only `SEND_DELIVER`
   (no real payload, just the recipient ref) back through it so
   the original CPU can unblock from a `ReceiveQueuePoll` and
   exit.

The whole exchange is three packets on the wire:
`SEND_DELIVER (CPU→term)`, `OBJ_READ_REQ (term→CPU)`,
`OBJ_READ_RESP (CPU→term)`, plus an optional fourth ack
`SEND_DELIVER (term→CPU)`. The terminal is just a Vol IV port
that happens to render to a screen.

### Wire trace example

A CPU at pid 0 writing 14 bytes (`"Hello, world!\n"`) starting at
offset 0 of its data object (let's say pid 0, index 5, generation
1) to the terminal at pid 16 produces (with `--trace`):

```
CROSSBAR CPU0  -> CPU16 SEND_DELIVER trans=0001 len=14w
CROSSBAR CPU16 -> CPU0  OBJ_READ_REQ trans=0001 len=4w
CROSSBAR CPU0  -> CPU16 OBJ_READ_RESP trans=0001 len=4w
                                                       (14B padded to 4 words)
```

The terminal's text widget then shows `Hello, world!`.

### The keyboard-subscription protocol

The keyboard service (index `2`) is the terminal's input side. CPUs
register a *subscription capability* — a reference the terminal will
SEND keystroke events back through.

**Subscribe.** SEND to the keyboard service with:

| Slot                 | Meaning                                                 |
|----------------------|---------------------------------------------------------|
| `O1` (recipient)     | The keyboard-service reference                          |
| `O2` (subscription)  | An object the terminal will SEND key events to. Typically the CPU's own `R|S`-derived self-service. |
| `O3`, `O4`, `R4..R7` | Unused; should be zero                                  |

The terminal records the `O2` reference in its subscriber list.
Every subsequent key press is dispatched as a SEND aimed at that
ref, with payload:

| Slot      | Meaning                                                              |
|-----------|----------------------------------------------------------------------|
| `O1`      | The subscription ref itself (the recipient at the SEND layer)        |
| `O2..O4`  | Null                                                                 |
| `R4`      | Codepoint (see table below)                                          |
| `R5`      | Modifier mask (`MOD_*` bits below)                                   |
| `R6`, `R7`| Reserved (zero)                                                      |

CPUs typically attach a receive queue to the subscription target
and `ReceiveQueuePoll` for events. Queue dispatch lands the wire
ints in `R3..R6`, so the codepoint reads from `R3` and the modifier
mask from `R4`.

**Unsubscribe.** SEND to the keyboard service with `O2 = null`. The
terminal clears its subscriber list. (v1 is coarse — a null SEND
removes *all* subscriptions; per-CPU unsubscribe will land when
multiple subscribers are needed.)

**OR hygiene for subscribers.** ReceiveQueuePoll's overlay sets
`O1..O4` from the wire payload (Vol VI §6) — for our key events
that's `[sub_ref, 0, 0, 0]`. Programs that later use:

- `print_int` / `print_char` (stack-buffer console_write) need
  `O2` (stack ref);
- `print_str` (data-segment console_write) needs `O3` (data ref);
- the *next* poll itself needs `O4` (self-service ref);

…must save those before the first poll and restore after each
one. `examples/cc/kbd_echo.c` shows the canonical pattern: park
`O2/O3/O4` into `O13/O14/O15` once at startup, copy them back
after each poll. Symptoms when you forget: prints silently
produce empty output, then the next poll returns `EFAULT`.

**Codepoints.** Plain ASCII bytes 0x00–0x7F come through verbatim
in `R4`. Special keys use a portable encoding ≥ `0x100`:

| Code    | Key            |
|---------|----------------|
| `0x108` | BackSpace      |
| `0x109` | Tab            |
| `0x10D` | Return / Enter |
| `0x11B` | Escape         |
| `0x17F` | Delete         |
| `0x180` | Up arrow       |
| `0x181` | Down arrow     |
| `0x182` | Left arrow     |
| `0x183` | Right arrow    |
| `0x184` | Home           |
| `0x185` | End            |
| `0x186` | Page Up        |
| `0x187` | Page Down      |
| `0x190` | F1             |
| …       | …              |
| `0x19B` | F12            |

**Modifiers.** `R5` is a bitmask:

| Bit    | Modifier                           |
|--------|------------------------------------|
| `0x01` | Shift                              |
| `0x02` | Control                            |
| `0x04` | Alt (Option on macOS)              |
| `0x08` | Meta (Command on macOS)            |

The modifier bits decode best-effort from Tk's `event.state`; rely
on the codepoint as the primary indicator and use modifiers as
hints rather than ground truth on macOS edge cases.

### The grid-write protocol

The grid service (index `3`) writes text at a specific
character-cell position on the graphics canvas. Same two-phase
shape as the console service — SEND a reference, terminal pulls
the bytes via OBJ_READ_REQ — but with explicit (col, row)
positioning instead of "wherever the next text cell happens to
be."

| Slot                 | Meaning                                                |
|----------------------|--------------------------------------------------------|
| `O1` (recipient)     | The grid-service reference                             |
| `O2` (data ref)      | Byte-typed object holding the text to draw             |
| `O3`, `O4`           | Unused                                                 |
| `R4`                 | Byte offset into the data object                       |
| `R5`                 | Byte length to draw                                    |
| `R6`                 | Grid column (0 = leftmost)                             |
| `R7`                 | Grid row    (0 = topmost)                              |

Cell metrics come from the canvas's monospace font — the same one
the text pane uses — so column N lands at pixel `N * cell_width`
and row N lands at `N * cell_height`. Multiple writes to the same
cell stack visually (the terminal doesn't clear before drawing);
clear the canvas with the vector service's `VEC_CLEAR` if you need
a fresh slate.

### The vector-drawing protocol

The vector service (index `4`) accepts immediate drawing commands
— no buffer reads, just SEND with the command code in `R4` and
arguments in `R5`/`R6`. Two-coordinate ops pack `(x, y)` into a
single 32-bit word as `(x << 16) | y`; sizes pack as
`(w << 16) | h`. The 16-bit halves are signed, so coordinates can
go negative if you really want.

| `R4`   | Command            | `R5`             | `R6`             | `R7`         |
|--------|--------------------|------------------|------------------|--------------|
| `0x00` | `VEC_LINE`         | `(x1<<16)\|y1`   | `(x2<<16)\|y2`   | unused       |
| `0x01` | `VEC_RECT_FILL`    | `(x<<16)\|y`     | `(w<<16)\|h`     | unused       |
| `0x02` | `VEC_RECT_OUTLINE` | `(x<<16)\|y`     | `(w<<16)\|h`     | unused       |
| `0x03` | `VEC_OVAL_FILL`    | `(x<<16)\|y`     | `(w<<16)\|h`     | unused       |
| `0x04` | `VEC_OVAL_OUTLINE` | `(x<<16)\|y`     | `(w<<16)\|h`     | unused       |
| `0x05` | `VEC_CLEAR`        | unused           | unused           | unused       |
| `0x06` | `VEC_SET_COLOR`    | palette index    | unused           | unused       |

The palette is a small fixed 9-colour set (index 0 is the canvas
background, 1 is the default foreground, 2–8 are red / green /
blue / yellow / cyan / magenta / bright-white). `VEC_SET_COLOR`
sets the current pen colour; subsequent draws use it until the
next `SET_COLOR`.

`examples/cc/paint.c` (run with
[`run_paint.sh`](../../examples/cc/run_paint.sh)) is an
interactive demo wiring up keyboard + grid + vector together —
arrow keys move a logical cursor, letter keys drop dots / lines /
rectangles / ovals at it, `C` cycles colour, space clears, ESC
exits.

### The raster service

The raster service (index `5`) is reserved for bitmap blits — a
SEND carries a byte buffer interpreted as one palette index per
pixel along with a destination rectangle. The protocol is pinned
in the source but not yet implemented; SENDs to it currently log
and drop. v1 will land alongside the first demo that actually
needs raster (probably a small framebuffer animation).

### The pointer-subscription protocol

The pointer service (index `6`) is the terminal's mouse / tablet
input. Same shape as keyboard: CPUs SEND a derived self-ref to
subscribe; the terminal records it and SENDs a pointer event back
through that ref every time the mouse moves or a button changes
state. Coordinates are absolute canvas pixel coordinates (the
"tablet / touchscreen" model — the terminal owns cursor display,
the protocol just delivers (x, y)).

**Subscribe / unsubscribe** — same payload shape as keyboard
(`O2 = subscriber ref` to subscribe, `O2 = null` to unsubscribe
all).

**Event payload** (terminal → CPU):

| Slot      | Meaning                                                              |
|-----------|----------------------------------------------------------------------|
| `O1`      | The subscription ref (the recipient at the SEND layer)               |
| `O2..O4`  | Null                                                                 |
| `R4`      | Event type — `0` motion, `1` button-down, `2` button-up              |
| `R5`      | Packed coordinates: `(x << 16) \| y`, both 16-bit unsigned, in canvas pixels |
| `R6`      | Button (`1` left, `2` middle, `3` right). Zero for motion events.    |
| `R7`      | Button-state mask (bit N for button N currently held). Updated *before* the event is dispatched, so a `down` event for button 1 reports `state | 0x02`. |

Queue dispatch lands the wire ints in `R3..R6`, so a polling
receiver reads:
- `R3` = event type
- `R4` = packed (x, y)
- `R5` = button
- `R6` = button-state mask

Coordinates are clamped to `[0, 0xFFFF]` and to the canvas bounds
before packing — events generated outside the canvas during a drag
are reported at the nearest edge.

**OR hygiene applies the same way as for keyboard subscribers**:
ReceiveQueuePoll's overlay clobbers `O2/O3/O4`, so a polling
receiver should park them at startup and restore on the way out
of every helper that polls or SENDs. See
[`examples/cc/mouse_paint.c`](../../examples/cc/mouse_paint.c)
for the canonical pattern (each helper calls `restore_or_state()`
on its way out, so the loop body never has to think about it).

## hostfsd

A Python-side host-filesystem server. Lets CPU-side C programs
read and write the host's actual files via a small set of file
ops dispatched as SENDs. The first piece of "OS-shaped" plumbing
in the project — a long way from a real syscall layer, but
enough to start writing programs that need to read configuration
or persist state.

Usage:

    tools/devices/hostfsd --socket /tmp/oriscbar.sock --pid 17
    tools/devices/hostfsd --socket /tmp/oriscbar.sock --pid 17 \
                          --root /var/lib/orisc-fixtures

`--root DIR` jails the service: paths are resolved relative to
`DIR`, and any path that resolves outside (via `..` or absolute
escapes) is rejected with `EACCES`. Without `--root` the service
is unjailed and reads/writes anywhere the launching user has
permission. The latter is fine for single-user development; the
former is right for any shared environment.

### Wire protocol

A single service object at index `1`. CPUs SEND requests to it;
all responses come back as SENDs to a per-CPU reply ref established
by an explicit subscribe call. The dispatch by op code (R4):

| `R4` | Op            | Request body                                          | Response (R3 / R4)               |
|------|---------------|-------------------------------------------------------|----------------------------------|
| `4`  | `SUBSCRIBE`   | O2 = subscriber's reply ref                           | (no response — establishes session) |
| `0`  | `OPEN`        | O2 = path buf, R5 = path off, R6 = path len, R7 = flags | R3 = fd or `-errno`; R4 = file size |
| `1`  | `CLOSE`       | R5 = fd                                               | R3 = 0 or `-errno`               |
| `2`  | `READ`        | O2 = dst buf (W cap), R5 = fd, R6 = dst off, R7 = count | R3 = bytes read or `-errno`      |
| `3`  | `WRITE`       | O2 = src buf, R5 = fd, R6 = src off, R7 = count       | R3 = bytes written or `-errno`   |

Flags for `OPEN`:

| Bit    | Meaning            |
|--------|--------------------|
| `0x01` | Read               |
| `0x02` | Write              |
| `0x04` | Create if missing  |
| `0x08` | Truncate on open   |

`READ` is implemented as: hostfsd reads from the host file, then
issues an `OBJ_WRITE_REQ` to land the bytes in the CPU's buffer.
This means the buffer ref handed to hostfsd needs `W` cap — which
the boot stack ref has, but the boot data ref doesn't. Callers
must use stack-allocated buffers for `hf_read`. (`WRITE` is the
reverse: hostfsd `OBJ_READ_REQ`s the source, then writes to the
host file. Source bufs can live anywhere with `R` cap.)

### Errors (negative `R3`)

| Code | Name      | Meaning                                                   |
|------|-----------|-----------------------------------------------------------|
| `-1` | `EBADF`   | bad fd, or no subscription                                |
| `-2` | `EINVAL`  | bad arguments / unknown op / oversized path               |
| `-3` | `ENOENT`  | file does not exist                                       |
| `-4` | `EACCES`  | permission denied, or path escapes `--root` jail          |
| `-5` | `EIO`     | host I/O error                                            |
| `-6` | `ENOMEM`  | per-CPU fd table full (limit 64)                          |

### C library

[`tools/cc/lib/host_io.c`](../cc/lib/host_io.c) wraps the protocol
in the obvious shape — `hf_init`, `hf_open`, `hf_close`, `hf_read`,
`hf_write`. Programs need to follow the OR-hygiene contract
documented at the top of that file: park the boot O2 (stack), O3
(data), and O4 (self-svc) in O11/O15/O14 once at startup, and
hostfsd's service ref needs to be in O10 at boot (the runner's
job, via `--service`). See
[`examples/cc/host_cat.c`](../../examples/cc/host_cat.c) for
the canonical use, and
[`examples/cc/run_host_cat.sh`](../../examples/cc/run_host_cat.sh)
for the launcher wiring.

## linkbootd

A Python-side link-boot server. Connects to the crossbar like any
other port, hosts a boot image (extracted from a `.orx` file or
read raw), and answers link-boot requests from CPUs running
[`examples/linkboot/linkboot.s`](../../examples/linkboot/linkboot.s).
Same shape as `oriscterm` — the only difference is what each does
with the SENDs it receives. (`oriscterm` renders bytes; `linkbootd`
ships executable code.)

Usage:

    python3 tools/devices/linkbootd \
        --socket /tmp/oriscbar.sock --pid 0 \
        --image path/to/module.orx [--entry OFFSET]

The image is either a `.orx` file (text section is extracted
automatically — magic `"ORISC\0\0\0"`) or a raw byte file.

### How it works

`linkbootd` synthesizes one descriptor for the hosted image at
`(home=our_pid, index=0x100, generation=1, caps=R|S|V|C)` — that
ref is what CPUs end up holding as their "code source" after
boot. Then it loops on the socket waiting for two packet kinds:

- **`SEND_DELIVER`** — treated as a boot announce. The sender's
  R|S self-ref is in OR slot 1 of the payload (per `linkboot.s`),
  and the sender's PROCID is in `R4`. `linkbootd` replies with
  another `SEND_DELIVER` aimed at the loader's self-ref:
  - `O2` = `image_ref` (the synthesized descriptor)
  - `R4` = byte length of the image
  - `R5` = entry offset (`--entry`, default 0)
- **`OBJ_READ_REQ`** — generated when the loader OLW's words from
  `image_ref` during its copy stage. `linkbootd` validates the
  reference (home/index/generation/caps), bounds-checks the
  offset and width, and replies with an `OBJ_READ_RESP` carrying
  the bytes (or a fault flag).

A single `linkbootd` can serve any number of loader CPUs — each
announce gets its own boot reply with the same image. To boot
different CPUs with different images, run one `linkbootd` per
image (each at a distinct pid) and arrange `--service` accordingly
on each loader.

### Try it

The runner script wires it all up: crossbar + `linkbootd` + N
loader CPUs running `linkboot.orx`, all in separate processes:

```sh
examples/linkboot/run_python_master.sh                # 1 CPU boots
NCPUS=4 examples/linkboot/run_python_master.sh        # 4 CPUs boot
NCPUS=8 examples/linkboot/run_python_master.sh        # 8 CPUs boot
```

Each CPU prints `Booted!` from its loaded module. The boot image
is a tiny .orx assembled inline by the runner; swap it out for
your own to ship arbitrary code to dynamic worker pools.

## Adding new devices

A device is anything that:

1. Connects to `oriscbar` over a UNIX domain socket.
2. Performs the handshake above (`HELLO_MAGIC` + 4-byte pid;
   expects `HELLO_MAGIC` + status word back).
3. Exchanges length-prefixed Vol IV §3 packets.

Devices typically expose one or more service objects at fixed
`(index, generation, caps)` tuples and document the SEND payload
convention CPUs should use to reach each one. There is no central
registry; CPUs receive references to remote services either at
boot (via the launcher's `--service` flag) or via earlier SENDs
from a service that knows how to bootstrap them.

[`tools/sim/tests/test_multiprocess.py`](../sim/tests/test_multiprocess.py)
includes a minimal headless "mock device" that may be a useful
starting point for new devices — no Tk dependency, just the wire
protocol in ~80 lines.
