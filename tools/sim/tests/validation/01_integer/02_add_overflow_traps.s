; @description: ADD on INT_MAX + 1 traps with arithmetic-overflow
; @expect-trap: arithmetic-overflow

.entry main
.text
main:
    lui   r2, 0x7fff
    ori   r2, r2, 0xffff   ; r2 = 0x7FFFFFFF (INT_MAX)
    addiu r3, r0, 1
    add   r4, r2, r3       ; signed overflow → trap
    call  #0x001
    nop
