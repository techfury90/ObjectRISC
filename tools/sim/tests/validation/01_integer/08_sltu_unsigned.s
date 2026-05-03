; @description: SLTU: 0xFFFFFFFF < 1 (unsigned) -> 0 (the same operands gave 1 under SLT)
; @expect-exit: 0

.entry main
.text
main:
    addi  r2, r0, -1       ; r2 = 0xFFFFFFFF
    addiu r3, r0, 1
    sltu  r4, r2, r3       ; unsigned: 0xFFFFFFFF > 1 → 0
    call  #0x001
    nop
