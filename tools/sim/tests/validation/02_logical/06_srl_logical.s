; @description: SRL: 0xFF000000 >> 24 = 0x000000FF (logical, no sign-extend)
; @expect-exit: 255

.entry main
.text
main:
    lui   r2, 0xFF00
    srl   r4, r2, 24
    call  #0x001
    nop
