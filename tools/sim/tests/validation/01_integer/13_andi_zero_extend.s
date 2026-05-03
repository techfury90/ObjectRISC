; @description: ANDI's immediate is zero-extended (vs ADDI's sign-extension)
; ANDI 0xFFFFFFFF & 0xFFFF = 0x0000FFFF; low byte 0xFF = 255
; @expect-exit: 255

.entry main
.text
main:
    addi  r2, r0, -1       ; r2 = 0xFFFFFFFF
    andi  r4, r2, 0xFFFF   ; zero-extended → r4 = 0x0000FFFF
    call  #0x001
    nop
