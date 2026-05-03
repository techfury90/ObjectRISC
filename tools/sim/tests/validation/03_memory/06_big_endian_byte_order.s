; @description: Store word 0x12345678; big-endian means LBU at +0 = 0x12 (highest byte first)
; @expect-exit: 0x12

.entry main
.text
main:
    lui   r2, 0x1234
    ori   r2, r2, 0x5678
    sw    r2, -16(sp)      ; store 0x12345678 as a word
    lbu   r4, -16(sp)      ; byte at +0: 0x12 if big-endian, 0x78 if little
    call  #0x001
    nop
