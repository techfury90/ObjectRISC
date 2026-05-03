; @description: BLTZ taken on -1 (signed lt 0); exit 6
; @expect-exit: 6

.entry main
.text
main:
    addi  r2, r0, -1
    bltz  r2, taken
    nop
    addiu r4, r0, 0
    call  #0x001
    nop
taken:
    addiu r4, r0, 6
    call  #0x001
    nop
