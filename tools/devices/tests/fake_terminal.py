#!/usr/bin/env python3
"""fake_terminal.py — headless stand-in for oriscterm, used to test
the keyboard event flow without a Tk window.

Connects to oriscbar, claims pid 16, accepts a subscribe SEND on
the keyboard service (idx 2), then emits the keystrokes given on
the command line as SEND_DELIVERs aimed at the recorded subscriber.

Usage:
    fake_terminal.py --socket /tmp/oriscbar.sock --pid 16 \\
                     --keys "AB" --final 0x11B
                                            ^ ESC to make the demo exit

The --keys arg is interpreted as plain ASCII bytes (0x20–0x7E).
For special keys use --extra CODE (repeatable). --final is the last
key to send; defaults to 0x11B (Escape) so demos terminate cleanly.
"""
import argparse, errno, selectors, socket, struct, sys, time

HELLO_MAGIC = 0xC0FFEEAA
PKT_SEND_DELIVER = 0x20

def pack_packet(src, dst, mtype, flags, trans, words):
    hdr = struct.pack(">BBBBHH", src, dst, mtype, flags, trans, len(words))
    pl = b''.join(struct.pack(">I", w & 0xFFFFFFFF) for w in words)
    h0, h1 = struct.unpack(">II", hdr)
    chk = h0 ^ h1
    for w in words: chk ^= w & 0xFFFFFFFF
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

def ref_home(r): return (r >> 40) & 0xFF

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--socket", required=True)
    ap.add_argument("--pid", type=int, default=16)
    ap.add_argument("--keys", default="",
                    help="ASCII bytes to send as keystrokes (in order)")
    ap.add_argument("--extra", type=lambda x: int(x, 0), action="append",
                    default=[],
                    help="extra special-key codepoint(s) to send AFTER --keys")
    ap.add_argument("--final", type=lambda x: int(x, 0), default=0x11B,
                    help="last codepoint to send (default 0x11B = Escape)")
    ap.add_argument("--delay", type=float, default=0.05,
                    help="seconds between keystrokes (default 0.05)")
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
    buf = bytearray()
    subscriber_ref = None
    deadline = time.time() + 10.0

    # Wait for the subscribe SEND.
    while subscriber_ref is None and time.time() < deadline:
        try:
            chunk = s.recv(65536)
            if chunk:
                buf.extend(chunk)
        except (BlockingIOError, OSError):
            time.sleep(0.01)
        while len(buf) >= 4:
            (n,) = struct.unpack(">I", bytes(buf[:4]))
            if len(buf) < 4 + n:
                break
            pkt_bytes = bytes(buf[4:4 + n])
            del buf[:4 + n]
            pkt = unpack_packet(pkt_bytes)
            if pkt["type"] == PKT_SEND_DELIVER and len(pkt["payload"]) == 14:
                # OR slot 1 (sender's O2) = subscription ref
                p = pkt["payload"]
                subscriber_ref = p[8] | (p[9] << 32)
    if subscriber_ref is None:
        sys.exit("fake_terminal: no subscribe SEND arrived within 10s")
    print(f"fake_terminal subscribed: 0x{subscriber_ref:016x}", flush=True)

    # Send each requested key as a SEND_DELIVER aimed at the subscriber.
    trans = 0
    codes = [ord(c) for c in args.keys] + list(args.extra)
    if args.final is not None:
        codes.append(args.final)
    for code in codes:
        time.sleep(args.delay)
        pkt = build_send_deliver(
            src_pid=args.pid,
            dst_pid=ref_home(subscriber_ref),
            trans=trans,
            recipient_ref=subscriber_ref,
            int_payload=[code, 0, 0, 0],          # R4=code, R5=mods=0
            or_payload=[subscriber_ref, 0, 0, 0],
        )
        s.sendall(struct.pack(">I", len(pkt)) + pkt)
        trans = (trans + 1) & 0xFFFF
        print(f"fake_terminal sent code=0x{code:x}", flush=True)

    # Hold the connection briefly so the receiver can drain.
    time.sleep(0.3)
    return 0

if __name__ == "__main__":
    sys.exit(main())
