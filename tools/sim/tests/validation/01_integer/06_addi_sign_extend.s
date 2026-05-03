; @description: ADDI immediate is sign-extended; 100 + (-37) = 63
; @expect-exit: 63

.entry main
.text
main:
    addiu r2, r0, 100
    addi  r4, r2, -37      ; -37 is 0xFFDB, sign-extended to 0xFFFFFFDB
    call  #0x001
    nop
