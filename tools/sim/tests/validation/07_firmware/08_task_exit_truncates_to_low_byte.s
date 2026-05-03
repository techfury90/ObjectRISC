; @description: TaskExit with R4 = 0x12345678 truncates to low byte 0x78 = 120
; @expect-exit: 120

.entry main
.text
main:
    lui   r4, 0x1234
    ori   r4, r4, 0x5678
    call  #0x001
    nop
