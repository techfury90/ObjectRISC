; 01_formats.s — exercise R, I, J, and O instruction formats.

.entry start

.text
start:
    addu  r2, r3, r4        ; R-type: SPECIAL funct 0x21
    addiu r5, r6, 7         ; I-type: opcode 0x09
    j     end               ; J-type: opcode 0x02
    omov  o2, o5            ; O-type (rr): opcode 0x30 funct 0
end:
    nop
