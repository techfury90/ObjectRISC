; @description: Variable shifts use only the low 5 bits of rs.
; Set count = 0x24 (low 5 = 4); 0xFFFF >> 4 = 0x0FFF; low byte = 0xFF = 255
; @expect-exit: 255

.entry main
.text
main:
    addiu r2, r0, 0xFFFF
    addiu r3, r0, 0x24     ; only low 5 = 4 used
    srlv  r4, r2, r3
    call  #0x001
    nop
