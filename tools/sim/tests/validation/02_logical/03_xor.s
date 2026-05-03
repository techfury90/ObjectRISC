; @description: XOR: 0xFF ^ 0x0F = 0xF0 = 240
; @expect-exit: 240

.entry main
.text
main:
    addiu r2, r0, 0xFF
    addiu r3, r0, 0x0F
    xor   r4, r2, r3
    call  #0x001
    nop
