; @description: OLW at offset 0 reads "Hell" as big-endian word 0x48656C6C; low byte 0x6C = 108
; @expect-exit: 108

.entry main
.text
main:
    olw   r4, 0(o3)
    andi  r4, r4, 0xFF
    call  #0x001
    nop

.data
    .string "Hell"
