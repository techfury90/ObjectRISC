; @description: CALL #0 (no primitive defined at 0) returns ENOSYS
; @expect-exit: 4

.entry main
.text
main:
    call  #0
    move  r4, r2
    call  #0x001
    nop
