; @description: SRA: 0xFF000000 >> 24 = 0xFFFFFFFF (sign-extending); low byte 0xFF
; @expect-exit: 255

.entry main
.text
main:
    lui   r2, 0xFF00
    sra   r4, r2, 24
    call  #0x001
    nop
