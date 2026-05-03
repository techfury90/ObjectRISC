; @description: Bounds-violation trap captures the exact faulting PC
; @expect-trap: bounds-violation
; @expect-trap-pc: 0x10000

.entry main
.text
main:
    olw   r4, 100(o3)      ; 0x10000 — traps here
    call  #0x001
    nop

.data
    .string "x"
