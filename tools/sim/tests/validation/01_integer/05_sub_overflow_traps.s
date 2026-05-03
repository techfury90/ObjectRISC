; @description: SUB on INT_MIN - 1 traps with arithmetic-overflow
; @expect-trap: arithmetic-overflow

.entry main
.text
main:
    lui   r2, 0x8000       ; r2 = 0x80000000 (INT_MIN)
    addiu r3, r0, 1
    sub   r4, r2, r3       ; signed underflow → trap
    call  #0x001
    nop
