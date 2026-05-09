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
PKT_OBJ_READ_REQ   = 0x10
PKT_OBJ_READ_RESP  = 0x11
PKT_OBJ_WRITE_REQ  = 0x12
PKT_OBJ_WRITE_RESP = 0x13
PKT_SEND_DELIVER   = 0x20

# OBJ-RESP fault codes (Vol IV §4.1).
RESP_OK     = 0x00
RESP_BOUNDS = 0x02

CONSOLE_INDEX     = 1
KEYBOARD_INDEX    = 2
GRID_INDEX        = 3
POINTER_INDEX     = 6
FRAMEBUFFER_INDEX = 7

# Framebuffer dimensions for the test environment.  fake_terminal
# doesn't render the pixels (no Tk surface) — it just satisfies the
# wire protocol so a smoke test can OBJ_WRITE_REQ + OBJ_READ_REQ
# round-trip.  Sized large enough for any test pattern; bytearrays
# are cheap. */
FB_TEST_W = 640
FB_TEST_H = 384
FB_TEST_SIZE = FB_TEST_W * FB_TEST_H

GRID_COLS = 80
GRID_ROWS = 24

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


def build_obj_read_resp(src_pid, dst_pid, trans, flags, data):
    """Pad data to a 4-byte word boundary; pack as response payload."""
    if flags == RESP_OK:
        pad = (-len(data)) % 4
        padded = data + bytes(pad)
        words = list(struct.unpack(f">{len(padded)//4}I", padded)) \
            if padded else []
    else:
        words = []
    return pack_packet(src_pid, dst_pid, PKT_OBJ_READ_RESP, flags,
                       trans, words)


def build_obj_write_resp(src_pid, dst_pid, trans, flags):
    return pack_packet(src_pid, dst_pid, PKT_OBJ_WRITE_RESP, flags,
                       trans, [])


def ref_home(r): return (r >> 40) & 0xFF
def ref_index(r): return (r >> 16) & 0xFFFFFF

def make_ref(generation, home, index, caps):
    return ((generation & 0xFFFF) << 48) | \
           ((home       & 0xFF)   << 40) | \
           ((index      & 0xFFFFFF) << 16) | \
           ((caps       & 0xFF)   << 8)

# Phase 47: oriscdir's wire-protocol op for inline self-registration.
# Mirrors the real oriscterm: fake_terminal must publish its own
# console/keyboard/grid sub-caps under /sys/term/<instance>/* so the
# supervisor's directory-walk-init finds them (no boot --service
# wiring on multi-terminal-aware CPUs).
DIR_SERVICE_INDEX  = 1
SERVICE_GENERATION = 1
OP_REG_INLINE      = 5
CAP_R = 0x01
CAP_W = 0x02
CAP_S = 0x08
CAP_V = 0x10

def send_inline_register(sock, my_pid, dir_pid, my_index, path,
                          caps=CAP_R | CAP_S):
    """Send a single DIR_OP_REG_INLINE packet that publishes a sub-cap
    of (my_pid, my_index) at `path` with the given caps.  Path is
    packed inline into int_payload[2..3] + or_payload[1..3] (32-byte
    budget) per oriscdir's docstring.  Fire-and-forget — no reply
    awaited.

    Default caps R|S are right for service objects; the framebuffer
    publishes with R|W|V so clients can OBJ_READ/WRITE_REQ against it."""
    path_bytes = path.encode("utf-8")
    if not 0 < len(path_bytes) <= 32:
        return
    my_ref  = make_ref(SERVICE_GENERATION, my_pid,  my_index,        caps)
    dir_ref = make_ref(SERVICE_GENERATION, dir_pid, DIR_SERVICE_INDEX, CAP_R | CAP_S)
    padded = path_bytes + b'\x00' * (32 - len(path_bytes))
    (b0_3,)   = struct.unpack("<I", padded[0:4])
    (b4_7,)   = struct.unpack("<I", padded[4:8])
    (b8_15,)  = struct.unpack("<Q", padded[8:16])
    (b16_23,) = struct.unpack("<Q", padded[16:24])
    (b24_31,) = struct.unpack("<Q", padded[24:32])
    int_payload = [OP_REG_INLINE, len(path_bytes), b0_3, b4_7]
    or_payload  = [my_ref, b8_15, b16_23, b24_31]
    pkt = build_send_deliver(my_pid, dir_pid, 0, dir_ref,
                             int_payload, or_payload)
    sock.sendall(struct.pack(">I", len(pkt)) + pkt)


class FakeTerminal:
    def __init__(self, sock, pid):
        self.sock = sock
        self.pid = pid
        self.buf = bytearray()
        # Multiple kbd subscribers can register (e.g. shell + a
        # backgrounded editor program). The focused index decides
        # which one each `key:` event lands on; a `focus` event
        # cycles it. Mirrors oriscterm's F1-cycle behaviour.
        self.kbd_subs: list = []
        self.kbd_focus: int = 0
        self.ptr_sub = None
        self.ptr_state = 0
        self.trans = 0
        # Phase 57: framebuffer storage for the OBJ_READ/WRITE_REQ
        # round-trip.  fake_terminal doesn't render the pixels, but
        # it has to satisfy the wire protocol so smoke tests can
        # write + read back.
        self.framebuffer = bytearray(FB_TEST_SIZE)
        # Outstanding console-write reads: trans_id → expected length.
        # On OBJ_READ_RESP we decode and append to console_render
        # (kept as a buffer so we can dump it cleanly at exit; per-byte
        # writes to a redirected stdout get block-buffered and lost).
        self.console_reads: dict = {}
        self.console_render = bytearray()
        # Grid-service rendering. Each cell starts as a space; grid
        # SENDs paint bytes at (col, row), VEC_CLEAR resets to all-
        # spaces. We also keep the *last non-empty* grid snapshot
        # before each VEC_CLEAR — useful for tests against full-
        # screen apps (the viewer, the future editor) which wipe the
        # canvas on exit so the final post-quit grid is empty.
        self.grid = [[ord(' ')] * GRID_COLS for _ in range(GRID_ROWS)]
        self.grid_last_frame = None

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
        if pkt["type"] == PKT_OBJ_READ_REQ:
            self._handle_obj_read_req(pkt)
            return
        if pkt["type"] == PKT_OBJ_WRITE_REQ:
            self._handle_obj_write_req(pkt)
            return
        if pkt["type"] != PKT_SEND_DELIVER:
            print(f"fake_terminal: ignoring pkt type 0x{pkt['type']:02x}",
                  file=sys.stderr, flush=True)
            return
        self._handle_send_deliver(pkt)

    def _handle_obj_read_req(self, pkt):
        """OBJ_READ_REQ for the framebuffer.  Same wire shape as
        oriscterm's handler — see ouroboros/oriscwm.c's milestone-3-α
        notes for the full protocol."""
        p = pkt["payload"]
        if len(p) < 4:
            return
        ref = p[0] | (p[1] << 32)
        offset = p[2]
        width = p[3]
        if ref_index(ref) != FRAMEBUFFER_INDEX:
            return
        if offset > FB_TEST_SIZE or width > FB_TEST_SIZE \
                or offset + width > FB_TEST_SIZE:
            resp = build_obj_read_resp(self.pid, pkt["src"],
                                        pkt["trans"], RESP_BOUNDS, b'')
            self.sock.sendall(struct.pack(">I", len(resp)) + resp)
            return
        data = bytes(self.framebuffer[offset:offset + width])
        resp = build_obj_read_resp(self.pid, pkt["src"], pkt["trans"],
                                    RESP_OK, data)
        self.sock.sendall(struct.pack(">I", len(resp)) + resp)

    def _handle_obj_write_req(self, pkt):
        """OBJ_WRITE_REQ for the framebuffer."""
        p = pkt["payload"]
        if len(p) < 4:
            return
        ref = p[0] | (p[1] << 32)
        offset = p[2]
        width = p[3]
        if ref_index(ref) != FRAMEBUFFER_INDEX:
            return
        if offset > FB_TEST_SIZE or width > FB_TEST_SIZE \
                or offset + width > FB_TEST_SIZE:
            resp = build_obj_write_resp(self.pid, pkt["src"],
                                         pkt["trans"], RESP_BOUNDS)
            self.sock.sendall(struct.pack(">I", len(resp)) + resp)
            return
        data_words = p[4:]
        data = b''.join(struct.pack(">I", w & 0xFFFFFFFF) for w in data_words)
        data = data[:width]
        self.framebuffer[offset:offset + width] = data
        resp = build_obj_write_resp(self.pid, pkt["src"], pkt["trans"],
                                     RESP_OK)
        self.sock.sendall(struct.pack(">I", len(resp)) + resp)

    def _handle_send_deliver(self, pkt):
        if len(pkt["payload"]) != 14:
            return
        p = pkt["payload"]
        recipient_ref = p[0] | (p[1] << 32)
        sub_ref = p[8] | (p[9] << 32)
        idx = ref_index(recipient_ref)
        print(f"fake_terminal: SEND to idx={idx} recipient=0x{recipient_ref:016x} "
              f"sub=0x{sub_ref:016x} R4={p[2]} R5={p[3]}",
              file=sys.stderr, flush=True)
        if idx == KEYBOARD_INDEX:
            cmd = p[2]   # R4 = 0 subscribe, 1 unsubscribe (mirrors oriscterm)
            if cmd == 1 and sub_ref:
                # Targeted unsubscribe by sub-ref.
                if sub_ref in self.kbd_subs:
                    self.kbd_subs.remove(sub_ref)
                    if self.kbd_focus >= len(self.kbd_subs):
                        self.kbd_focus = 0
                    print(f"fake_terminal: kbd unsubscribe 0x{sub_ref:016x} "
                          f"(now {len(self.kbd_subs)} sub(s))", flush=True)
                else:
                    print(f"fake_terminal: kbd unsubscribe for unknown "
                          f"0x{sub_ref:016x} (no-op)", flush=True)
            elif sub_ref:
                if sub_ref not in self.kbd_subs:
                    self.kbd_subs.append(sub_ref)
                print(f"fake_terminal: kbd subscribe 0x{sub_ref:016x} "
                      f"(now {len(self.kbd_subs)} sub(s))", flush=True)
            else:
                # Legacy unsubscribe-all on null sub_ref (kept for
                # callers that still use the coarse v1 convention).
                n = len(self.kbd_subs)
                self.kbd_subs.clear()
                self.kbd_focus = 0
                print(f"fake_terminal: kbd unsubscribe "
                      f"(was {n} sub(s), now 0 sub(s))", flush=True)
        elif idx == POINTER_INDEX and sub_ref:
            self.ptr_sub = sub_ref
            print(f"fake_terminal: ptr subscribe 0x{sub_ref:016x}", flush=True)
        elif idx == CONSOLE_INDEX:
            # CPU sent us a console-write request — same shape as
            # oriscterm's idx-1 service: O2 = source ref, R4 = offset,
            # R5 = byte count. OR slot 2 (sender's O3) optionally
            # carries a reply_cap — when present we send back an
            # empty SEND_DELIVER aimed at it after the OBJ_READ_RESP
            # is processed. That's what term_print_n_sync uses to
            # know "the bytes have been pulled, my buffer can be
            # reused now". Mirrors oriscterm's reply_cap path.
            source_ref = p[8] | (p[9] << 32)
            reply_cap  = p[10] | (p[11] << 32)
            offset = p[2]
            length = p[3]
            if source_ref and length:
                trans = self._next_trans()
                self.console_reads[trans] = {
                    "kind": "console",
                    "len": length, "reply_cap": reply_cap,
                }
                pkt_out = build_obj_read_req(
                    self.pid, ref_home(source_ref), trans,
                    source_ref, offset, length)
                self.sock.sendall(struct.pack(">I", len(pkt_out)) + pkt_out)
        elif idx == GRID_INDEX:
            # Grid SEND: O2 = source ref, R4 = offset, R5 = length,
            # R6 = col, R7 = row. Special: col=row=0xFFFFFFFF clears
            # the whole canvas (no payload pulled). Otherwise we
            # OBJ_READ_REQ the bytes and paint them into the in-
            # memory grid at the named cell.
            source_ref = p[8] | (p[9] << 32)
            offset = p[2]
            length = p[3]
            col    = p[4]
            row    = p[5]
            if col == 0xFFFFFFFF and row == 0xFFFFFFFF:
                # Stash the last frame iff anything had been painted.
                # Empty wipes (the viewer's enter-and-exit clears)
                # don't overwrite an earlier substantive frame.
                if any(c != ord(' ') for r in self.grid for c in r):
                    self.grid_last_frame = [list(r) for r in self.grid]
                self.grid = [[ord(' ')] * GRID_COLS for _ in range(GRID_ROWS)]
                return
            if source_ref and length:
                trans = self._next_trans()
                self.console_reads[trans] = {
                    "kind": "grid", "len": length,
                    "col": col, "row": row,
                }
                pkt_out = build_obj_read_req(
                    self.pid, ref_home(source_ref), trans,
                    source_ref, offset, length)
                self.sock.sendall(struct.pack(">I", len(pkt_out)) + pkt_out)

    def _send_ack(self, recipient_ref):
        """Empty SEND_DELIVER aimed at recipient_ref — used as the
        reply for a console SEND that carried a reply_cap."""
        trans = self._next_trans()
        # build_send_deliver from this file (defined elsewhere in the
        # module); 14-word payload, all int + or slots zero.
        pkt = build_send_deliver(
            self.pid, ref_home(recipient_ref), trans,
            recipient_ref, [0, 0, 0, 0], [0, 0, 0, 0])
        self.sock.sendall(struct.pack(">I", len(pkt)) + pkt)

    def _handle_read_resp(self, pkt):
        info = self.console_reads.pop(pkt["trans"], None)
        if info is None:
            return
        n = info["len"]
        reply_cap = info.get("reply_cap", 0)
        if pkt["flags"] & 0x3F:
            # Fault — still send the ack so the sender doesn't block
            # forever waiting on a reply that will never come.
            if reply_cap:
                self._send_ack(reply_cap)
            return
        words = pkt["payload"]
        raw = b''.join(struct.pack(">I", w & 0xFFFFFFFF) for w in words)
        kind = info.get("kind", "console")
        if kind == "grid":
            col = info["col"]
            row = info["row"]
            if 0 <= row < GRID_ROWS:
                for i, b in enumerate(raw[:n]):
                    c = col + i
                    if 0 <= c < GRID_COLS:
                        self.grid[row][c] = b
            return
        # Default: console rendering. Mirror oriscterm's control-byte
        # handling:
        #   - 0x08 (\b): delete the previous byte from the stream
        #     (shell's read_line uses this for backspace-erase).
        #   - 0x0C (\f): clear the entire rendered stream (login.orx
        #     fires one before each welcome banner so the new
        #     session starts on a blank canvas).
        for b in raw[:n]:
            if b == 0x08:
                if self.console_render:
                    del self.console_render[-1]
            elif b == 0x0C:
                self.console_render = bytearray()
            else:
                self.console_render.append(b)
        if reply_cap:
            self._send_ack(reply_cap)

    def wait_for(self, want_kbd, want_ptr, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.drain_pending_subs(0.05)
            if (not want_kbd or self.kbd_subs) \
               and (not want_ptr or self.ptr_sub is not None):
                return True
        return False

    def wait_for_n_kbd(self, n, timeout=10.0):
        """Block until at least `n` keyboard subscribers have arrived
        (used after spawning extra programs that take their own kbd
        subscriptions, like the standalone editor)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.drain_pending_subs(0.05)
            if len(self.kbd_subs) >= n:
                return True
        return False

    def _next_trans(self):
        t = self.trans & 0xFFFF
        self.trans = (self.trans + 1) & 0xFFFF
        return t

    def cycle_focus(self):
        """Mirror oriscterm's F1 behaviour: advance kbd_focus to the
        next subscriber, then SEND a synthetic KEY_FOCUS_IN to the
        newly-focused subscriber so it can repaint immediately. No-op
        when 0 or 1 subs."""
        n = len(self.kbd_subs)
        if n > 1:
            self.kbd_focus = (self.kbd_focus + 1) % n
            print(f"fake_terminal: kbd focus → {self.kbd_focus + 1}/{n}",
                  flush=True)
            self.send_key(0x10E, 0)   # KEY_FOCUS_IN — see oriscterm

    def send_key(self, code, mods=0):
        # Phase 48: when one program (e.g. login.orx) unsubscribes
        # and the next (e.g. the shell it just spawned) hasn't
        # subscribed yet, the kbd subscriber list goes briefly
        # empty. Loading shell.orx via hostfsd takes ~15+ chunked
        # reads + TaskCreate + term_init, easily 5-10s of wall
        # clock under simulator pace. Wait up to 30s for a new
        # subscriber to arrive before declaring failure.
        if not self.kbd_subs:
            deadline = time.time() + 30.0
            while not self.kbd_subs and time.time() < deadline:
                self.drain_pending_subs(0.05)
            if not self.kbd_subs:
                sys.exit("fake_terminal: no kbd subscriber for key event "
                         "(waited 30s — handoff stalled?)")
        idx = self.kbd_focus if self.kbd_focus < len(self.kbd_subs) else 0
        sub = self.kbd_subs[idx]
        pkt = build_send_deliver(
            src_pid=self.pid, dst_pid=ref_home(sub),
            trans=self._next_trans(), recipient_ref=sub,
            int_payload=[code, mods, 0, 0],
            or_payload=[sub, 0, 0, 0],
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
    # Bare events (no payload) — focus is the only one for now.
    if spec == "focus":
        return ("focus",)
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
    if kind == "wait-kbd":
        return ("wait-kbd", int(parts[0]))
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
    ap.add_argument("--directory-pid", type=int, default=None,
                    help="oriscdir pid; if set, fake_terminal "
                         "self-registers at /sys/term/<instance>/* "
                         "via the inline-register wire op (Phase 47)")
    ap.add_argument("--instance", type=int, default=0,
                    help="terminal instance number for the directory "
                         "path; defaults to 0")
    args = ap.parse_args()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(args.socket)
    s.sendall(struct.pack(">II", HELLO_MAGIC, args.pid))
    magic, status = struct.unpack(">II", s.recv(8))
    if magic != HELLO_MAGIC or status != 0:
        sys.exit(f"fake_terminal: handshake failed magic={magic:#x} "
                 f"status={status:#x}")

    # Phase 47: self-register console/keyboard/grid in oriscdir
    # before printing READY so any supervisor that boots after we
    # do can find us via dir_walk. This must happen on the blocking
    # socket — once we go non-blocking the bytes might not flush.
    if args.directory_pid is not None:
        base = f"/sys/term/{args.instance}"
        send_inline_register(s, args.pid, args.directory_pid,
                             CONSOLE_INDEX,  f"{base}/console")
        send_inline_register(s, args.pid, args.directory_pid,
                             KEYBOARD_INDEX, f"{base}/keyboard")
        send_inline_register(s, args.pid, args.directory_pid,
                             GRID_INDEX,     f"{base}/grid")
        send_inline_register(s, args.pid, args.directory_pid,
                             FRAMEBUFFER_INDEX, f"{base}/framebuffer",
                             caps=CAP_R | CAP_W | CAP_V)

    print(f"fake_terminal READY pid={args.pid}", flush=True)
    s.setblocking(False)

    term = FakeTerminal(s, args.pid)
    events = [parse_event(e) for e in args.event]

    # Figure out which subscriptions we'll need.
    need_kbd = any(e[0] == "key" for e in events)
    need_ptr = any(e[0] in ("motion", "down", "up") for e in events)
    if not term.wait_for(need_kbd, need_ptr, args.subscribe_timeout):
        sys.exit(f"fake_terminal: required subscriptions did not arrive "
                 f"(kbd={'ok' if term.kbd_subs else 'pending'}, "
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
        elif ev[0] == "focus":
            # Local-only: cycle which kbd subscriber receives keys.
            # No SEND goes out — mirrors oriscterm's F1 (terminal
            # consumes the hotkey and just changes its routing).
            term.cycle_focus()
        elif ev[0] == "wait-kbd":
            # Block until at least N keyboard subscribers have
            # registered. Useful between launching a backgrounded
            # CPU-side program and sending it keystrokes — without
            # this you race the program's term_init.
            n = ev[1]
            if not term.wait_for_n_kbd(n, timeout=20.0):
                sys.exit(f"fake_terminal: only {len(term.kbd_subs)}/{n} "
                         f"kbd subscribers arrived")

    # Final drain — give the CPU a chance to flush its last outputs.
    term.drain_pending_subs(args.linger)
    # Dump everything the CPU asked us to render. Single block at the
    # end of the run so test scripts can grep cleanly without the
    # event-log lines interleaved.
    sys.stdout.write("--- console render ---\n")
    sys.stdout.write(term.console_render.decode("utf-8", errors="replace"))
    # Grid render: 24 rows of 80 cells. Right-trimmed to the last
    # non-space column so the snapshot stays readable.
    sys.stdout.write("\n--- grid render ---\n")
    for row in term.grid:
        line = bytes(row).decode("utf-8", errors="replace").rstrip()
        sys.stdout.write(line + "\n")
    if term.grid_last_frame is not None:
        sys.stdout.write("\n--- grid last frame ---\n")
        for row in term.grid_last_frame:
            line = bytes(row).decode("utf-8", errors="replace").rstrip()
            sys.stdout.write(line + "\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
