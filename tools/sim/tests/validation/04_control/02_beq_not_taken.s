; @description: BEQ not taken: 5 != 7 -> fall through, exit 2
; @expect-exit: 2

.entry main
.text
main:
    addiu r2, r0, 5
    addiu r3, r0, 7
    beq   r2, r3, taken
    nop
    addiu r4, r0, 2        ; fall through: exit 2
    call  #0x001
    nop
taken:
    addiu r4, r0, 0
    call  #0x001
    nop
