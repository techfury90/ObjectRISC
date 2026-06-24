#!/usr/bin/env python3
"""test_backpressure.py — regression test for oriscbar's write-side
backpressure (the >8 KiB silent-drop bug).

Before the fix, oriscbar.Connection.write_packet relayed each packet
with a single non-blocking `sendall`. On macOS a UNIX-domain socket's
send buffer is only 8 KiB (net.local.stream.sendspace), so a packet
that didn't fit — or a stream of packets to a peer that wasn't draining
its recv buffer — raised BlockingIOError, which the `except OSError`
swallowed. The packet (or a partial prefix of it) vanished, desyncing
the receiver's length-prefixed framing and hanging it forever.

This test drives that exact scenario through the real oriscbar process:

  * a "receiver" connects but deliberately stops reading, so its recv
    buffer and the crossbar's send buffer both fill;
  * a "sender" then pushes 1 MiB across a mix of 64 KiB and tiny
    packets (each far over the 8 KiB ceiling in aggregate);
  * the receiver finally drains and must see EVERY packet, in order,
    byte-for-byte identical.

With the old code the receiver stalls / loses bytes and the test times
out. With the backpressure queue + EVENT_WRITE flush, all bytes arrive.

Exit 0 = pass, non-zero = fail. No CPU / .orx / Tk involved — this is a
pure transport test of oriscbar.
"""

import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORISCBAR = HERE.parent / "oriscbar"

HELLO_MAGIC = 0xC0FFEEAA
HELLO_OK    = 0x00000000

RECV_PID = 200
SEND_PID = 100


def _connect(sock_path: str, timeout: float = 5.0) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deadline = time.time() + timeout
    while True:
        try:
            s.connect(sock_path)
            break
        except (FileNotFoundError, ConnectionRefusedError):
            if time.time() >= deadline:
                raise RuntimeError("connect timeout")
            time.sleep(0.05)
    return s


def _handshake(s: socket.socket, my_pid: int) -> None:
    s.sendall(struct.pack(">II", HELLO_MAGIC, my_pid))
    reply = bytearray()
    while len(reply) < 8:
        chunk = s.recv(8 - len(reply))
        if not chunk:
            raise RuntimeError("handshake EOF")
        reply.extend(chunk)
    magic, status = struct.unpack(">II", bytes(reply))
    if magic != HELLO_MAGIC or status != HELLO_OK:
        raise RuntimeError(f"handshake failed magic={magic:#x} status={status}")


def _make_packet(src: int, dst: int, seq: int, size: int) -> bytes:
    """A routable packet of `size` total bytes: an 8-byte header whose
    byte[1] is the dst pid (all oriscbar inspects), then a deterministic
    payload keyed by `seq` so the receiver can verify integrity."""
    assert size >= 8
    # header: src, dst, type, flags, trans(2B), length_words(2B)
    hdr = struct.pack(">BBBBHH", src & 0xFF, dst & 0xFF, 0x07, 0,
                      seq & 0xFFFF, ((size - 8) // 4) & 0xFFFF)
    body = bytes(((seq * 31 + i) & 0xFF) for i in range(size - 8))
    return hdr + body


def main() -> int:
    sock_path = tempfile.mktemp(prefix="oriscbar-bptest-", suffix=".sock")
    bar = subprocess.Popen([sys.executable, str(ORISCBAR),
                            "--socket", sock_path],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        # Wait for the listening socket to appear.
        deadline = time.time() + 5
        while not os.path.exists(sock_path):
            if bar.poll() is not None:
                print("FAIL: oriscbar exited during startup", file=sys.stderr)
                return 1
            if time.time() >= deadline:
                print("FAIL: oriscbar never created its socket", file=sys.stderr)
                return 1
            time.sleep(0.02)

        recv = _connect(sock_path)
        _handshake(recv, RECV_PID)
        send = _connect(sock_path)
        _handshake(send, SEND_PID)

        # Build a mix of large (64 KiB) and tiny packets, ~1 MiB total —
        # vastly over the 8 KiB single-socket send buffer.
        packets = []
        for seq in range(20):
            size = 0x10000 if (seq % 2 == 0) else 16   # 64 KiB / 16 B
            packets.append(_make_packet(SEND_PID, RECV_PID, seq, size))
        total_bytes = sum(len(p) for p in packets)

        # The receiver is NOT reading yet. Push everything from the
        # sender; the crossbar must queue what the receiver can't take
        # instead of dropping it. (The sender's own sendall may block
        # briefly while the crossbar's RAM queue grows — that's fine.)
        for p in packets:
            send.sendall(struct.pack(">I", len(p)) + p)

        # Now drain the receiver and reassemble the framed stream.
        recv.setblocking(False)
        buf = bytearray()
        got = []
        deadline = time.time() + 15
        while len(got) < len(packets):
            if time.time() >= deadline:
                print(f"FAIL: timed out — received {len(got)}/{len(packets)} "
                      f"packets ({len(buf)} bytes left in frame buffer)",
                      file=sys.stderr)
                return 1
            rd, _, _ = select.select([recv], [], [], 0.1)
            if recv in rd:
                try:
                    chunk = recv.recv(65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    print("FAIL: receiver got EOF mid-stream", file=sys.stderr)
                    return 1
                buf.extend(chunk)
            while len(buf) >= 4:
                (length,) = struct.unpack(">I", bytes(buf[:4]))
                if len(buf) < 4 + length:
                    break
                got.append(bytes(buf[4:4 + length]))
                del buf[:4 + length]

        # Verify count, order, and byte-exactness.
        if len(got) != len(packets):
            print(f"FAIL: packet count {len(got)} != {len(packets)}",
                  file=sys.stderr)
            return 1
        for i, (want, have) in enumerate(zip(packets, got)):
            if want != have:
                print(f"FAIL: packet {i} corrupted "
                      f"(len want={len(want)} have={len(have)})", file=sys.stderr)
                return 1

        print(f"PASS: {len(packets)} packets / {total_bytes} bytes "
              f"relayed intact across the 8 KiB buffer (largest "
              f"{max(len(p) for p in packets)} B)")
        return 0
    finally:
        try:
            recv.close()
        except Exception:
            pass
        try:
            send.close()
        except Exception:
            pass
        bar.terminate()
        try:
            bar.wait(timeout=3)
        except subprocess.TimeoutExpired:
            bar.kill()
        try:
            os.unlink(sock_path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    sys.exit(main())
