; @description: CALL #0x999 (not implemented) returns R2 = ENOSYS = 4
; @expect-exit: 4

.entry main
.text
main:
    call  #0x999
    move  r4, r2
    call  #0x001
    nop
