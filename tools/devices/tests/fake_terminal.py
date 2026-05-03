#!/usr/bin/env python3
"""fake_terminal.py — headless stand-in for oriscterm, used to test
the keyboard and pointer event flows without a Tk window.

Connects to oriscbar, claims pid 16, accepts subscribe SENDs on the
keyboard (idx 2) and/or pointer (idx 6) services, and emits a
sequence of synthetic events from the command line.

Event-spec format (--event, repeatable, in dispatch order):

    key:CODE                — keyboard event with codepoint = CODE
                              (decimal, hex, or single ASCII char)
    motion:X,Y              — pointer motion to (X, Y)
    down:X,Y,BTN            — pointer button-down (BTN: 1=L, 2=M, 3=R)
    up:X,Y,BTN              — pointer button-up

Examples:

    fake_terminal.py --socket S --pid 16 \\
        --event key:A --event key:B --event key:0x11B
        # type "AB" then ESC

    fake_terminal.py --socket S --pid 16 \\
        --event down:100,150,1 --event motion:110,150 \\
        --event up:110,150,1
        # left-click at (100,150), drag to (110,150), release

The script blocks until each event's required subscription has
arrived (kbd events wait for a kbd subscribe; pointer events wait
for a pointer subscribe).
"""
import argparse, errno, selectors, socket, struct, sys, time

HELLO_MAGIC = 0xC0FFEEAA
PKT_OBJ_READ_REQ  = 0x10
PKT_OBJ_READ_RESP = 0x11
PKT_SEND_DELIVER  = 0x20

CONSOLE_INDEX  = 1
KEYBOARD_INDEX = 2
POINTER_INDEX  = 6

PTR_MOTION = 0x00
PTR_DOWN   = 0x01
PTR_UP     = 0x02


def pack_packet(src, dst, mtype, flags, trans, words):
    hdr = struct.pack(">BBBBHH", src, dst, mtype, flags, trans, len(words))
    pl = b''.join(struct.pack(">I", w & 0xFFFFFFFF) for w in words)
    h0, h1 = struct.unpack(">II", hdr)
    chk = h0 ^ h1
    for w in words:
        chk ^= w & 0xFFFFFFFF
    return hdr + pl + struct.pack(">I", chk & 0xFFFFFFFF)


def unpack_packet(data):
    src, dst, mtype, flags, trans, n = struct.unpack(">BBBBHH", data[:8])
    words = list(struct.unpack(f">{n}I", data[8:8+n*4])) if n else []
    return {"src": src, "dst": dst, "type": mtype, "flags": flags,
            "trans": trans, "payload": words}


def build_send_deliver(src_pid, dst_pid, trans, recipient_ref,
                       int_payload, or_payload):
    pl = [recipient_ref & 0xFFFFFFFF, (recipient_ref >> 32) & 0xFFFFFFFF]
    for i in range(4):
        pl.append(int_payload[i] & 0xFFFFFFFF if i < len(int_payload) else 0)
    for i in range(4):
        ref = or_payload[i] if i < len(or_payload) else 0
        pl.append(ref & 0xFFFFFFFF)
        pl.append((ref >> 32) & 0xFFFFFFFF)
    return pack_packet(src_pid, dst_pid, PKT_SEND_DELIVER, 0, trans, pl)


def build_obj_read_req(src_pid, dst_pid, trans, ref, offset, width):
    payload = [ref & 0xFFFFFFFF, (ref >> 32) & 0xFFFFFFFF,
               offset & 0xFFFFFFFF, width & 0xFFFFFFFF]
    return pack_packet(src_pid, dst_pid, PKT_OBJ_READ_REQ, 0, trans, payload)


def ref_home(r): return (r >> 40) & 0xFF
def ref_index(r): return (r >> 16) & 0xFFFFFF


class FakeTerminal:
    def __init__(self, sock, pid):
        self.sock = sock
        self.pid = pid
        self.buf = bytearray()
        self.kbd_sub = None
        self.ptr_sub = None
        self.ptr_state = 0
        self.trans = 0
        # Outstanding console-write reads: trans_id → expected length.
        # On OBJ_READ_RESP we decode and append to console_render
        # (kept as a buffer so we can dump it cleanly at exit; per-byte
        # writes to a redirected stdout get block-buffered and lost).
        self.console_reads: dict = {}
        self.console_render = bytearray()

    def drain_pending_subs(self, timeout):
        """Pull bytes off the socket until both `timeout` seconds have
        passed without progress OR all expected subscriptions arrive."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                chunk = self.sock.recv(65536)
                if chunk:
                    self.buf.extend(chunk)
            except (BlockingIOError, OSError):
                time.sleep(0.01)
            while len(self.buf) >= 4:
                (n,) = struct.unpack(">I", bytes(self.buf[:4]))
                if len(self.buf) < 4 + n:
                    break
                pkt_bytes = bytes(self.buf[4:4 + n])
                del self.buf[:4 + n]
                self._handle(unpack_packet(pkt_bytes))

    def _handle(self, pkt):
        if pkt["type"] == PKT_OBJ_READ_RESP:
            self._handle_read_resp(pkt)
            return
        if pkt["type"] != PKT_SEND_DELIVER:
            print(f"fake_terminal: ignoring pkt type 0x{pkt['type']:02x}",
                  file=sys.stderr, flush=True)
            return
        if len(pkt["payload"]) != 14:
            return
        p = pkt["payload"]
        recipient_ref = p[0] | (p[1] << 32)
        sub_ref = p[8] | (p[9] << 32)
        idx = ref_index(recipient_ref)
        print(f"fake_terminal: SEND to idx={idx} recipient=0x{recipient_ref:016x} "
              f"sub=0x{sub_ref:016x} R4={p[2]} R5={p[3]}",
              file=sys.stderr, flush=True)
        if idx == KEYBOARD_INDEX and sub_ref:
            self.kbd_sub = sub_ref
            print(f"fake_terminal: kbd subscribe 0x{sub_ref:016x}", flush=True)
        elif idx == POINTER_INDEX and sub_ref:
            self.ptr_sub = sub_ref
            print(f"fake_terminal: ptr subscribe 0x{sub_ref:016x}", flush=True)
        elif idx == CONSOLE_INDEX:
            # CPU sent us a console-write request — same shape as
            # oriscterm's idx-1 service: O2 = source ref, R4 = offset,
            # R5 = byte count. The OR payload starts at word 6; OR
            # slot 1 (sender's O2) is words [8..10].
            source_ref = p[8] | (p[9] << 32)
            offset = p[2]
            length = p[3]
            if source_ref and length:
                trans = self._next_trans()
                self.console_reads[trans] = length
                pkt_out = build_obj_read_req(
                    self.pid, ref_home(source_ref), trans,
                    source_ref, offset, length)
                self.sock.sendall(struct.pack(">I", len(pkt_out)) + pkt_out)

    def _handle_read_resp(self, pkt):
        n = self.console_reads.pop(pkt["trans"], None)
        if n is None:
            return
        if pkt["flags"] & 0x3F:
            return    # fault; ignore
        words = pkt["payload"]
        raw = b''.join(struct.pack(">I", w & 0xFFFFFFFF) for w in words)
        self.console_render.extend(raw[:n])

    def wait_for(self, want_kbd, want_ptr, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.drain_pending_subs(0.05)
            if (not want_kbd or self.kbd_sub is not None) \
               and (not want_ptr or self.ptr_sub is not None):
                return True
        return False

    def _next_trans(self):
        t = self.trans & 0xFFFF
        self.trans = (self.trans + 1) & 0xFFFF
        return t

    def send_key(self, code, mods=0):
        if self.kbd_sub is None:
            sys.exit("fake_terminal: no kbd subscriber for key event")
        pkt = build_send_deliver(
            src_pid=self.pid, dst_pid=ref_home(self.kbd_sub),
            trans=self._next_trans(), recipient_ref=self.kbd_sub,
            int_payload=[code, mods, 0, 0],
            or_payload=[self.kbd_sub, 0, 0, 0],
        )
        self.sock.sendall(struct.pack(">I", len(pkt)) + pkt)
        print(f"fake_terminal: sent key code=0x{code:x} mods=0x{mods:x}",
              flush=True)

    def _send_pointer(self, evt_type, x, y, button):
        if self.ptr_sub is None:
            sys.exit("fake_terminal: no ptr subscriber for pointer event")
        if evt_type == PTR_DOWN:
            self.ptr_state |= 1 << button
        elif evt_type == PTR_UP:
            self.ptr_state &= ~(1 << button)
        packed_xy = ((x & 0xFFFF) << 16) | (y & 0xFFFF)
        pkt = build_send_deliver(
            src_pid=self.pid, dst_pid=ref_home(self.ptr_sub),
            trans=self._next_trans(), recipient_ref=self.ptr_sub,
            int_payload=[evt_type, packed_xy, button, self.ptr_state],
            or_payload=[self.ptr_sub, 0, 0, 0],
        )
        self.sock.sendall(struct.pack(">I", len(pkt)) + pkt)
        kind = {PTR_MOTION: "motion", PTR_DOWN: "down", PTR_UP: "up"}[evt_type]
        print(f"fake_terminal: sent ptr {kind} x={x} y={y} btn={button} "
              f"state=0x{self.ptr_state:x}", flush=True)

    def send_motion(self, x, y):
        self._send_pointer(PTR_MOTION, x, y, 0)

    def send_down(self, x, y, button):
        self._send_pointer(PTR_DOWN, x, y, button)

    def send_up(self, x, y, button):
        self._send_pointer(PTR_UP, x, y, button)


def parse_code(s):
    if len(s) == 1:
        return ord(s)
    return int(s, 0)


def parse_event(spec):
    """Return (kind, args)."""
    if ":" not in spec:
        sys.exit(f"fake_terminal: malformed --event {spec!r} (need kind:args)")
    kind, rest = spec.split(":", 1)
    parts = rest.split(",")
    if kind == "key":
        if len(parts) == 1:
            return ("key", parse_code(parts[0]), 0)
        return ("key", parse_code(parts[0]), int(parts[1], 0))
    if kind == "motion":
        return ("motion", int(parts[0]), int(parts[1]))
    if kind == "down":
        return ("down", int(parts[0]), int(parts[1]), int(parts[2]))
    if kind == "up":
        return ("up", int(parts[0]), int(parts[1]), int(parts[2]))
    sys.exit(f"fake_terminal: unknown event kind {kind!r}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    ap.add_argument("--pid", type=int, default=16)
    ap.add_argument("--event", action="append", default=[],
                    help="event spec (repeatable, in dispatch order)")
    ap.add_argument("--delay", type=float, default=0.05,
                    help="seconds between events (default 0.05)")
    ap.add_argument("--linger", type=float, default=0.3,
                    help="seconds to keep the connection open after the "
                         "last event (default 0.3)")
    ap.add_argument("--subscribe-timeout", type=float, default=10.0)
    args = ap.parse_args()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(args.socket)
    s.sendall(struct.pack(">II", HELLO_MAGIC, args.pid))
    magic, status = struct.unpack(">II", s.recv(8))
    if magic != HELLO_MAGIC or status != 0:
        sys.exit(f"fake_terminal: handshake failed magic={magic:#x} "
                 f"status={status:#x}")
    print(f"fake_terminal READY pid={args.pid}", flush=True)
    s.setblocking(False)

    term = FakeTerminal(s, args.pid)
    events = [parse_event(e) for e in args.event]

    # Figure out which subscriptions we'll need.
    need_kbd = any(e[0] == "key" for e in events)
    need_ptr = any(e[0] in ("motion", "down", "up") for e in events)
    if not term.wait_for(need_kbd, need_ptr, args.subscribe_timeout):
        sys.exit(f"fake_terminal: required subscriptions did not arrive "
                 f"(kbd={'ok' if term.kbd_sub else 'pending'}, "
                 f"ptr={'ok' if term.ptr_sub else 'pending'})")

    for ev in events:
        # Keep the socket drained between events so console writes
        # arrive in order with the keystrokes that triggered them.
        term.drain_pending_subs(args.delay)
        if ev[0] == "key":
            term.send_key(ev[1], ev[2])
        elif ev[0] == "motion":
            term.send_motion(ev[1], ev[2])
        elif ev[0] == "down":
            term.send_down(ev[1], ev[2], ev[3])
        elif ev[0] == "up":
            term.send_up(ev[1], ev[2], ev[3])

    # Final drain — give the CPU a chance to flush its last outputs.
    term.drain_pending_subs(args.linger)
    # Dump everything the CPU asked us to render. Single block at the
    # end of the run so test scripts can grep cleanly without the
    # event-log lines interleaved.
    sys.stdout.write("--- console render ---\n")
    sys.stdout.write(term.console_render.decode("utf-8", errors="replace"))
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
