; @description: OLW at offset 2 from a 5-byte object: 2 + 4 = 6 > 5, traps
; @expect-trap: bounds-violation

.entry main
.text
main:
    olw   r4, 2(o3)        ; would read bytes 2,3,4,5 — but only 0..4 exist
    call  #0x001
    nop

.data
    .string "Hello"
