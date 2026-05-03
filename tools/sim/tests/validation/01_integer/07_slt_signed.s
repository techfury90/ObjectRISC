; @description: SLT: -5 < 1 (signed) -> 1
; @expect-exit: 1

.entry main
.text
main:
    addi  r2, r0, -5
    addiu r3, r0, 1
    slt   r4, r2, r3       ; signed compare: -5 < 1 → 1
    call  #0x001
    nop
