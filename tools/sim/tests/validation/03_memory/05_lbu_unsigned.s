; @description: SB 0xFF then LBU: zero-ext to 0x000000FF; high 24 bits = 0; exit 0
; @expect-exit: 0

.entry main
.text
main:
    addiu r2, r0, 0xFF
    sb    r2, -16(sp)
    lbu   r3, -16(sp)      ; zero-ext: 0x000000FF
    srl   r4, r3, 8        ; high 24 bits: 0
    call  #0x001
    nop
