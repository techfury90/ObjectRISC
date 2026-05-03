; @description: capability-violation trap captures the exact PC of the failed OSB
; @expect-trap: capability-violation
; @expect-trap-pc: 0x10004

.entry main
.text
main:
    addiu r2, r0, 0x42     ; 0x10000
    osb   r2, 0(o3)        ; 0x10004 — O3 lacks W → traps here
    call  #0x001
    nop

.data
    .string "x"
