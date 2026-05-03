; @description: OLW at offset+width past the data object's length traps with bounds-violation
; @expect-trap: bounds-violation

.entry main
.text
main:
    olw   r4, 100(o3)      ; offset way past 5-byte object
    call  #0x001
    nop

.data
    .string "Hello"
