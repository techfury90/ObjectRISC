; @description: BLEZ taken on -1 (signed le 0); exit 4
; @expect-exit: 4

.entry main
.text
main:
    addi  r2, r0, -1
    blez  r2, taken
    nop
    addiu r4, r0, 0
    call  #0x001
    nop
taken:
    addiu r4, r0, 4
    call  #0x001
    nop
