; @description: SB 0xFF then LB: signed extend to 0xFFFFFFFF; arithmetic shift right by 24 = 0xFFFFFFFF; low byte 0xFF
; @expect-exit: 255

.entry main
.text
main:
    addiu r2, r0, 0xFF
    sb    r2, -16(sp)
    lb    r3, -16(sp)      ; sign-ext: 0xFFFFFFFF
    sra   r4, r3, 24       ; if signed-ext: 0xFFFFFFFF; if zero-ext: 0
    call  #0x001
    nop
