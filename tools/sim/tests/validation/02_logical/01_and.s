; @description: AND: 0xFF & 0x0F = 0x0F
; @expect-exit: 15

.entry main
.text
main:
    addiu r2, r0, 0xFF
    addiu r3, r0, 0x0F
    and   r4, r2, r3
    call  #0x001
    nop
