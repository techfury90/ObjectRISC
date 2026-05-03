; @description: OCAP on data: per CONTRACT §2 the data object has caps R+C = 0x41 = 65
; @expect-exit: 65

.entry main
.text
main:
    ocap  r4, o3
    call  #0x001
    nop

.data
    .string "x"
