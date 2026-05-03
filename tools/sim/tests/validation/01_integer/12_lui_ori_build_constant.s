; @description: LUI then ORI builds 0x12345678; exit code is the low byte (0x78 = 120)
; @expect-exit: 120

.entry main
.text
main:
    lui   r4, 0x1234
    ori   r4, r4, 0x5678   ; r4 = 0x12345678
    call  #0x001
    nop
