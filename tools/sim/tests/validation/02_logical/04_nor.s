; @description: NOR: ~(0 | 0) = 0xFFFFFFFF; low byte 0xFF = 255
; @expect-exit: 255

.entry main
.text
main:
    nor   r4, r0, r0
    call  #0x001
    nop
