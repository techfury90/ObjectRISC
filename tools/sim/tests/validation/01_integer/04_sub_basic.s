; @description: SUB: 17 - 5 = 12
; @expect-exit: 12

.entry main
.text
main:
    addiu r2, r0, 17
    addiu r3, r0, 5
    sub   r4, r2, r3
    call  #0x001
    nop
