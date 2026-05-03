; @description: LW from an unmapped address (0x0) traps with tlb-miss-d
; @expect-trap: tlb-miss-d

.entry main
.text
main:
    lw    r4, 0(r0)
    call  #0x001
    nop
