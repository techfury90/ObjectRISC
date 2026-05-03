; @description: OLW through the null reference traps with null-dereference
; @expect-trap: null-dereference

.entry main
.text
main:
    olw   r4, 0(o0)
    call  #0x001
    nop
