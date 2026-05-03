; @description: SH then LHU: load as unsigned half -> 0x0000FFFF; high half is 0 (vs LH which would give 0xFFFF)
; @expect-exit: 0

.entry main
.text
main:
    addiu r2, r0, 0xFFFF
    sh    r2, -16(sp)
    lhu   r3, -16(sp)      ; if zero-extending: r3 = 0x0000FFFF
    srl   r4, r3, 16       ; high half: should be 0 (vs 0xFFFF if sign-extending)
    call  #0x001
    nop
