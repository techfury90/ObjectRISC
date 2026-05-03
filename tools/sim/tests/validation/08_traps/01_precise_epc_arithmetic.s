; @description: Arithmetic-overflow trap captures the exact faulting PC
; main is at 0x10000; the ADD that overflows is the 4th instruction at 0x1000C
; @expect-trap: arithmetic-overflow
; @expect-trap-pc: 0x1000C

.entry main
.text
main:
    lui   r2, 0x7fff       ; 0x10000
    ori   r2, r2, 0xffff   ; 0x10004
    addiu r3, r0, 1        ; 0x10008
    add   r4, r2, r3       ; 0x1000C — traps here
    call  #0x001
    nop
