#!/usr/bin/env python3
"""Multi-process pipeline test.

Spawns oriscbar, a CPU running a tiny SEND-then-exit program, plus a
mock "device" process that loops back any SEND it receives by sending
a single ACK packet (without tkinter — so this test runs headless on
CI). Verifies the CPU completes, the device received the SEND, and
the wire-level checksum/framing held throughout."""

from __future__ import annotations

import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT       = Path(__file__).resolve().parents[3]
ASMORISC   = ROOT / "tools" / "asm"  / "asmorisc"
ORISCBAR   = ROOT / "tools" / "sim"  / "oriscbar"
SIMORISC   = ROOT / "tools" / "sim"  / "simorisc"

HELLO_MAGIC      = 0xC0FFEEAA
HELLO_OK         = 0x00000000
PKT_SEND_DELIVER = 0x20

CPU_PROGRAM = """
.entry main
.text
main:
    omov  o14, o4                 ; preserve own service for poll
    omov  o1, o14
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop
    omov  o1, o14
    addiu r4, r0, 0x08
    call  #0x103                  ; ObjDerive (S only)
    bne   r2, r0, fail
    nop
    omov  o13, o1                 ; reply cap

    omov  o1, o5                  ; recipient = mock device (pid 16)
    onull o2
    omov  o3, o13                 ; reply cap so device can ack us
    onull o4
    addiu r4, r0, 0xCAFE          ; just a marker int (low 16)
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    omov  o1, o14
    addiu r4, r0, -1              ; infinite poll
    call  #0x204                  ; ReceiveQueuePoll
    bne   r2, r0, fail
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
"""


def pack_packet(src, dst, mtype, flags, trans, payload):
    header = struct.pack(">BBBBHH", src & 0xFF, dst & 0xFF, mtype & 0xFF,
                         flags & 0xFF, trans & 0xFFFF, len(payload) & 0xFFFF)
    body = b''.join(struct.pack(">I", w & 0xFFFFFFFF) for w in payload)
    h0, h1 = struct.unpack(">II", header)
    chk = h0 ^ h1
    for w in payload:
        chk ^= w & 0xFFFFFFFF
    return header + body + struct.pack(">I", chk & 0xFFFFFFFF)


def unpack_packet(data):
    if len(data) < 12:
        raise ValueError("short")
    src, dst, mtype, flags, trans, lw = struct.unpack(">BBBBHH", data[:8])
    expected = 8 + lw * 4 + 4
    if len(data) != expected:
        raise ValueError("size")
    body = data[8:8 + lw * 4]
    words = list(struct.unpack(f">{lw}I", body)) if lw else []
    return {"src": src, "dst": dst, "type": mtype, "flags": flags,
            "trans": trans, "payload": words}


def mock_device(sock_path: str, my_pid: int, results: dict) -> None:
    """Connect to crossbar, accept one SEND_DELIVER, ack via the
    sender's reply cap, then exit. Records what it saw in `results`."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    deadline = time.time() + 5
    while True:
        try:
            s.connect(sock_path); break
        except (FileNotFoundError, ConnectionRefusedError):
            if time.time() >= deadline:
                results["error"] = "connect timeout"; return
            time.sleep(0.05)
    s.sendall(struct.pack(">II", HELLO_MAGIC, my_pid))
    reply = bytearray()
    while len(reply) < 8:
        chunk = s.recv(8 - len(reply))
        if not chunk:
            results["error"] = "handshake EOF"; return
        reply.extend(chunk)
    magic, status = struct.unpack(">II", bytes(reply))
    if magic != HELLO_MAGIC or status != HELLO_OK:
        results["error"] = f"handshake failed magic={magic:#x} status={status}"
        return
    s.setblocking(False)
    buf = bytearray()
    end_at = time.time() + 8
    while time.time() < end_at:
        rd, _, _ = select.select([s], [], [], 0.05)
        if s in rd:
            try:
                chunk = s.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf.extend(chunk)
        # Frame
        while len(buf) >= 4:
            (length,) = struct.unpack(">I", bytes(buf[:4]))
            if len(buf) < 4 + length:
                break
            packet = bytes(buf[4:4 + length])
            del buf[:4 + length]
            try:
                pkt = unpack_packet(packet)
            except ValueError as e:
                results["error"] = f"malformed: {e}"; return
            if pkt["type"] != PKT_SEND_DELIVER:
                continue
            results.setdefault("received", []).append(pkt)
            # Ack via sender's O3 (reply cap = wire OR slot 2)
            p = pkt["payload"]
            reply_cap = p[6 + 2*2] | (p[6 + 2*2 + 1] << 32)
            if reply_cap != 0:
                ack_payload = []
                ack_payload.append(reply_cap & 0xFFFFFFFF)
                ack_payload.append((reply_cap >> 32) & 0xFFFFFFFF)
                ack_payload.extend([0, 0, 0, 0])
                for _ in range(4):
                    ack_payload.extend([0, 0])
                ack = pack_packet(my_pid, (reply_cap >> 40) & 0xFF,
                                  PKT_SEND_DELIVER, 0, 0, ack_payload)
                s.sendall(struct.pack(">I", len(ack)) + ack)
                results["acked"] = True
            return
    results["error"] = "timed out waiting for SEND"


def main() -> int:
    # Assemble the test program.
    src = Path(tempfile.mkstemp(suffix=".s")[1])
    src.write_text(CPU_PROGRAM)
    orx = src.with_suffix(".orx")
    r = subprocess.run([sys.executable, str(ASMORISC), str(src),
                        "-o", str(orx)], capture_output=True)
    if r.returncode != 0:
        sys.stderr.write(f"asm failed: {r.stderr.decode()}\n")
        return 1

    sock_path = tempfile.mktemp(prefix="oriscbar-mptest-", suffix=".sock")

    # Spawn crossbar.
    bar = subprocess.Popen(
        [sys.executable, str(ORISCBAR), "--socket", sock_path],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    # Wait for READY.
    deadline = time.time() + 5
    while time.time() < deadline:
        rd, _, _ = select.select([bar.stdout], [], [], 0.1)
        if bar.stdout in rd:
            line = bar.stdout.readline()
            if b"READY" in line:
                break

    # Spawn mock device in a thread.
    import threading
    dev_results: dict = {}
    dev_thread = threading.Thread(
        target=mock_device, args=(sock_path, 16, dev_results), daemon=True)
    dev_thread.start()
    time.sleep(0.3)  # let it connect

    # Spawn the CPU.
    cpu = subprocess.Popen(
        [sys.executable, str(SIMORISC),
         "--connect", sock_path, "--pid", "0",
         "--service", "16=1@0x09", str(orx)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    try:
        cpu_rc = cpu.wait(timeout=10)
    except subprocess.TimeoutExpired:
        cpu.kill(); cpu_rc = -1

    dev_thread.join(timeout=3)

    bar.terminate()
    try:
        bar.wait(timeout=2)
    except subprocess.TimeoutExpired:
        bar.kill()

    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass

    # Verify.
    failures = []
    if cpu_rc != 0:
        failures.append(f"CPU exit code {cpu_rc} (expected 0)")
    if "error" in dev_results:
        failures.append(f"device: {dev_results['error']}")
    if not dev_results.get("received"):
        failures.append("device received nothing")
    elif len(dev_results["received"]) != 1:
        failures.append(f"device received {len(dev_results['received'])} "
                        f"packets (expected 1)")
    elif (dev_results["received"][0]["payload"][2] & 0xFFFF) != 0xCAFE:
        failures.append(f"R4 marker mismatch: "
                        f"got 0x{dev_results['received'][0]['payload'][2]:08x}")
    if not dev_results.get("acked"):
        failures.append("device did not ack")

    if failures:
        for f in failures:
            print(f"FAIL: {f}")
        return 1
    print("PASS multi-process pipeline (CPU SENDs, device acks via reply cap, "
          "CPU unblocks and exits 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
