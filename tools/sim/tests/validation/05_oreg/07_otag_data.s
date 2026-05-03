; @description: OTAG on data object returns its type tag 0x4102 per CONTRACT §2 (low byte 0x02 = 2)
; @expect-exit: 2

.entry main
.text
main:
    otag  r4, o3
    andi  r4, r4, 0xFF
    call  #0x001
    nop

.data
    .string "x"
