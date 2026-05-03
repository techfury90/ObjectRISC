; @description: SH then LH: store 0xFFFF, load as signed half -> 0xFFFFFFFF; low byte 0xFF
; @expect-exit: 255

.entry main
.text
main:
    addiu r2, r0, 0xFFFF
    sh    r2, -16(sp)      ; store half (16-bit), value 0xFFFF
    lh    r4, -16(sp)      ; load signed half -> 0xFFFFFFFF
    call  #0x001
    nop
