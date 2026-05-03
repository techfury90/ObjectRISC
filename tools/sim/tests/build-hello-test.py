#!/usr/bin/env python3
"""Hand-encode tests/hello-test.orx from the assembly in hello-test.s.

This proves the simulator's idea of the contract encoding agrees with
CONTRACT.md without depending on the assembler agent's output. Each
instruction word is derived from the encoding tables in CONTRACT.md
Section 5 with a comment showing the bit-field breakdown.
"""

import os
import struct
import sys


# --- Encoding helpers --------------------------------------------------------

def i_type(op, rs, rt, imm):
    return ((op & 0x3F) << 26) | ((rs & 0x1F) << 21) | ((rt & 0x1F) << 16) | (imm & 0xFFFF)

def j_type(op, target26):
    return ((op & 0x3F) << 26) | (target26 & 0x03FFFFFF)

def o_rr(op, os_, ot, rd, rs, funct):
    # See CONTRACT.md 5.8: 6 | 4 | 4 | 5 | 5 | 4 | 4
    return ((op & 0x3F) << 26) | ((os_ & 0xF) << 22) | ((ot & 0xF) << 18) \
         | ((rd & 0x1F) << 13) | ((rs & 0x1F) << 8) | ((funct & 0xF) << 4)


# --- Hello-world program -----------------------------------------------------

INSTRUCTIONS = [
    # main:
    ("omov  o1, o3",              o_rr(op=0x30, os_=1, ot=3, rd=0, rs=0, funct=0x0)),
    ("addiu r4, r0, 0",           i_type(op=0x09, rs=0,  rt=4, imm=0)),
    ("addiu r5, r0, 14",          i_type(op=0x09, rs=0,  rt=5, imm=14)),
    ("call  #0x320 ; ConsoleWrite", j_type(op=0x3D, target26=0x320)),
    ("addiu r4, r0, 0",           i_type(op=0x09, rs=0,  rt=4, imm=0)),
    ("call  #0x001 ; TaskExit",   j_type(op=0x3D, target26=0x001)),
]

DATA_BYTES = b"Hello, world!\n"  # 14 bytes


def build():
    text = b"".join(struct.pack(">I", w) for _, w in INSTRUCTIONS)
    assert len(text) == 24, len(text)
    assert len(DATA_BYTES) == 14

    header = struct.pack(">8sIIIIII",
        b"ORISC\x00\x00\x00",   # magic
        1,                       # version
        0,                       # flags
        0,                       # entry offset (= "main")
        len(text),               # text size
        len(DATA_BYTES),         # data size
        0,                       # stack size (0 -> loader default)
    )
    assert len(header) == 32

    return header + text + DATA_BYTES


def main():
    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "hello-test.orx")
    blob = build()
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"wrote {out_path} ({len(blob)} bytes)")
    # Show the encoded words for human inspection.
    print("encoded text section:")
    for asm, word in INSTRUCTIONS:
        print(f"  0x{word:08x}  {asm}")


if __name__ == "__main__":
    main()
