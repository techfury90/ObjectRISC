; @description: JAL writes RA = PC+8 (the address after the delay slot) and jumps; we return via JR ra
; subroutine sets r5; main exits with r4 + r5 = 10
; @expect-exit: 10

.entry main
.text
main:
    addiu r4, r0, 7
    jal   subr
    nop
    add   r4, r4, r5       ; 7 + 3 = 10
    call  #0x001
    nop

subr:
    addiu r5, r0, 3
    jr    ra
    nop
