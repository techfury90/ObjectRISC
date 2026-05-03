; @description: OISN on a live reference returns 0
; @expect-exit: 0

.entry main
.text
main:
    oisn  r4, o3
    call  #0x001
    nop

.data
    .string "x"
