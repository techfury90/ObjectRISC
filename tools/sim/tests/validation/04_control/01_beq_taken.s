; @description: BEQ taken: 5 == 5 -> branch to label, exit 1
; @expect-exit: 1

.entry main
.text
main:
    addiu r2, r0, 5
    addiu r3, r0, 5
    beq   r2, r3, taken
    nop                    ; delay slot
    addiu r4, r0, 0        ; not taken: would exit 0
    call  #0x001
    nop
taken:
    addiu r4, r0, 1
    call  #0x001
    nop
