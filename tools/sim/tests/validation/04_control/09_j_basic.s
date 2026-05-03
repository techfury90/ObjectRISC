; @description: J (unconditional jump) to a label
; @expect-exit: 9

.entry main
.text
main:
    j     target
    nop
    addiu r4, r0, 0        ; skipped
    call  #0x001
    nop
target:
    addiu r4, r0, 9
    call  #0x001
    nop
