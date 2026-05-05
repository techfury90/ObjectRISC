; @description: CALL of an unallocated primitive (#0xFFF in the task range) returns ENOSYS
; @expect-exit: 4

.entry main
.text
main:
    call  #0xFFF              ; unallocated; #0x000 is now TaskCreate
    move  r4, r2
    call  #0x001
    nop
