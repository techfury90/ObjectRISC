; @description: CALL with the maximum 26-bit immediate (0x3FFFFFF) returns ENOSYS=4
; @expect-exit: 4

.entry main
.text
main:
    call  #0x3FFFFFF
    move  r4, r2
    call  #0x001
    nop
