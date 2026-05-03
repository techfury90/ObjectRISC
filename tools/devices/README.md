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

A graphical terminal device. Opens a Tk window, connects to the
crossbar at a chosen pid (typically `16+` to avoid clashing with
CPU pids), exposes a single *console object* at index `1`,
generation `1`, max caps `R|W|S|V|C` (`0x5B`).

Usage:

    python3 tools/devices/oriscterm --socket /tmp/oriscbar.sock --pid 16

Or, more typically, via the launcher:

    python3 tools/oriscrun \
        --terminal pid=16 \
        --cpu pid=0:program=examples/hello_terminal.orx,service=16=1@9

The `--service 16=1@9` clause synthesizes a `R|S = 0x09` (read +
send-permitted) capability on the terminal's console object and
installs it into the CPU's next free `O5..O15` slot at boot.

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
