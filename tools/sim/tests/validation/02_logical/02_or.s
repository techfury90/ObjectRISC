; @description: OR: 0x0F | 0xF0 = 0xFF
; @expect-exit: 255

.entry main
.text
main:
    addiu r2, r0, 0x0F
    addiu r3, r0, 0xF0
    or    r4, r2, r3
    call  #0x001
    nop
