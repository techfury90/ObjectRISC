; @description: SLL: 5 << 4 = 80
; @expect-exit: 80

.entry main
.text
main:
    addiu r2, r0, 5
    sll   r4, r2, 4
    call  #0x001
    nop
