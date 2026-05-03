#!/usr/bin/env python3
"""Direct unit tests for the wire-level packet helpers in simorisc.
These exercise the byte-level pack/unpack logic without going through the
full simulator, so a regression in the format shows up immediately and
in isolation."""

import importlib.util
import importlib.machinery
import sys
from pathlib import Path

SIM = Path(__file__).resolve().parents[1] / "simorisc"

loader = importlib.machinery.SourceFileLoader("simorisc", str(SIM))
spec = importlib.util.spec_from_loader("simorisc", loader)
sim = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sim)


def test_header_roundtrip():
    pkt = sim.pack_packet(src_pid=3, dst_pid=7, msg_type=sim.PKT_SEND_DELIVER,
                          flags=0xAB, trans_id=0x1234, payload_words=[])
    assert len(pkt) == 8 + 0 + 4, len(pkt)
    parsed = sim.unpack_packet(pkt)
    assert parsed == {
        "src_pid": 3, "dst_pid": 7, "type": sim.PKT_SEND_DELIVER,
        "flags": 0xAB, "trans_id": 0x1234, "payload": [],
    }, parsed


def test_payload_roundtrip():
    payload = [0xDEADBEEF, 0x12345678, 0xCAFEBABE, 0x00000001]
    pkt = sim.pack_packet(src_pid=0, dst_pid=1, msg_type=sim.PKT_OBJ_READ_REQ,
                          flags=0, trans_id=42, payload_words=payload)
    parsed = sim.unpack_packet(pkt)
    assert parsed["payload"] == payload, parsed["payload"]
    assert parsed["src_pid"] == 0 and parsed["dst_pid"] == 1
    assert parsed["type"] == sim.PKT_OBJ_READ_REQ
    assert parsed["trans_id"] == 42


def test_checksum_detection():
    pkt = bytearray(sim.pack_packet(src_pid=0, dst_pid=1,
                                    msg_type=sim.PKT_SEND_DELIVER,
                                    flags=0, trans_id=0,
                                    payload_words=[0x12345678]))
    pkt[-1] ^= 0xFF  # corrupt checksum
    try:
        sim.unpack_packet(bytes(pkt))
    except ValueError as e:
        assert "checksum" in str(e), str(e)
    else:
        raise AssertionError("expected ValueError on bad checksum")


def test_truncation_detection():
    pkt = sim.pack_packet(0, 0, sim.PKT_SEND_DELIVER, 0, 0, [0xAAAA])
    try:
        sim.unpack_packet(pkt[:-1])  # drop one byte
    except ValueError as e:
        assert "length mismatch" in str(e), str(e)
    else:
        raise AssertionError("expected ValueError on truncation")


def test_send_deliver_roundtrip():
    int_payload = [0x10, 0x20, 0x30, 0x40]
    or_payload  = [
        sim.make_ref(generation=1, home=0, index=5, caps=0x4F),
        sim.make_ref(generation=2, home=1, index=7, caps=0x09),
        0,
        sim.make_ref(generation=99, home=3, index=0xABCDEF, caps=0xFF),
    ]
    recipient = sim.make_ref(generation=42, home=2, index=0xBEEF, caps=0x5B)
    msg = sim.PendingSend(recipient_ref=recipient,
                          int_payload=int_payload,
                          or_payload=or_payload,
                          src_pid=0, trans_id=7)
    wire = msg.to_wire(dst_pid=2)
    pkt = sim.unpack_packet(wire)
    assert pkt["src_pid"] == 0 and pkt["dst_pid"] == 2
    assert pkt["type"] == sim.PKT_SEND_DELIVER
    assert pkt["trans_id"] == 7
    assert len(pkt["payload"]) == 14
    # Refs are transmitted low half first (Vol IV §4.1).
    assert pkt["payload"][0] == recipient & 0xFFFFFFFF
    assert pkt["payload"][1] == (recipient >> 32) & 0xFFFFFFFF
    # Int payload at words 2..5
    assert pkt["payload"][2:6] == int_payload
    # Round-trip through PendingSend.from_wire
    parsed = sim.PendingSend.from_wire(wire)
    assert parsed.recipient_ref == recipient
    assert parsed.int_payload == int_payload
    assert parsed.or_payload == or_payload
    assert parsed.src_pid == 0
    assert parsed.trans_id == 7


def main():
    tests = [v for k, v in globals().items() if k.startswith("test_")]
    passed = 0
    for t in tests:
        try:
            t()
        except AssertionError as e:
            print(f"FAIL {t.__name__}: {e}")
            return 1
        passed += 1
        print(f"PASS {t.__name__}")
    print(f"{passed}/{len(tests)} wire-format unit tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
