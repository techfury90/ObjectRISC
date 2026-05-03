; @description: OLH at offset 0 reads 'H' 'e' as big-endian halfword 0x4865; low byte 0x65 = 101
; @expect-exit: 101

.entry main
.text
main:
    olh   r4, 0(o3)
    andi  r4, r4, 0xFF
    call  #0x001
    nop

.data
    .string "He"
