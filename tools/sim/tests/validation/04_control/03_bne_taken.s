; @description: BNE taken: 5 != 7 -> branch, exit 3
; @expect-exit: 3

.entry main
.text
main:
    addiu r2, r0, 5
    addiu r3, r0, 7
    bne   r2, r3, taken
    nop
    addiu r4, r0, 0
    call  #0x001
    nop
taken:
    addiu r4, r0, 3
    call  #0x001
    nop
