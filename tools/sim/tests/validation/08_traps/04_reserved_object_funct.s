; @description: OBJECT funct 0x9 (reserved range 0x9-0xF in this revision) traps as reserved-instruction
; Encoding: opcode=0x30 (110000) << 26 = 0xC0000000; funct=0x9 << 4 = 0x90
;   -> 0xC0000090
; @expect-trap: reserved-instruction

.entry main
.text
main:
    .word 0xC0000090
    call  #0x001
    nop
