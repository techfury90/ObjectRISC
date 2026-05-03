; @description: OBJECT funct 0x8 (reserved for the future fence instruction) traps as reserved-instruction
; Encoding: opcode=0x30 (110000) << 26 = 0xC0000000; funct=0x8 << 4 = 0x80
;   -> 0xC0000080
; @expect-trap: reserved-instruction

.entry main
.text
main:
    .word 0xC0000080
    call  #0x001
    nop
