# Object RISC device processes

External devices that participate in the Object RISC system as Ports
on the crossbar — same wire format as CPUs, different innards.

## oriscterm

A graphics terminal device. Opens a tkinter window, connects to the
crossbar at a chosen pid (typically `16+` to avoid clashing with CPU
pids), exposes a single "console" object at index `1`, generation `1`,
caps `R|W|S|V|C` (the terminal's view of itself; CPUs synthesize a
`R|S = 0x09` view to reach it).

Protocol: when a CPU SENDs to the console object, the terminal:
1. Reads wire OR slot 1 of the SEND payload as a *source buffer*
   reference (sender's `O2`).
2. Reads wire OR slot 2 as an optional *reply capability*
   (sender's `O3`); zero means no ack.
3. Reads wire int slots 0, 1 as `(byte offset, byte length)`.
4. Issues an `OBJ_READ_REQ` packet to the source buffer's home pid for
   `length` bytes starting at `offset`.
5. On `OBJ_READ_RESP`, decodes the data words back to bytes and
   appends them to the on-screen text widget.
6. If a reply cap was provided, sends a header-only `SEND_DELIVER`
   ack back through it.

Usage:

    python3 tools/devices/oriscterm --socket /tmp/oriscbar.sock --pid 16

Or, more typically, via the launcher:

    python3 tools/oriscrun \
        --terminal pid=16 \
        --cpu pid=0:program=examples/hello_terminal.orx,service=16=1@9

## Adding new devices

A device is anything that:
1. Connects to oriscbar via UNIX domain socket
2. Performs the handshake (`HELLO_MAGIC` + 4-byte pid; expects
   `HELLO_MAGIC` + status word back)
3. Exchanges length-prefixed wire-format packets per Volume IV §3

Devices typically expose one or more "service" objects at fixed
(index, generation, caps) tuples and document the SEND payload
convention CPUs should use to reach each one. There is no central
registry; CPUs receive references to remote services either at boot
(via the launcher's `--service` flag) or via earlier SENDs.

`tools/sim/tests/test_multiprocess.py` includes a minimal headless
"mock device" that may be a useful starting point for new devices.
