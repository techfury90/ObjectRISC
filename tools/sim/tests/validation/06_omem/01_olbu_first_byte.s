; @description: OLBU at offset 0 of "Hello..." returns 'H' = 0x48 = 72
; @expect-exit: 72

.entry main
.text
main:
    olbu  r4, 0(o3)
    call  #0x001
    nop

.data
    .string "Hello"
