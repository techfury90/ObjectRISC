; @description: JALR rd, rs sets rd = PC+8 then jumps to rs; subroutine returns via JR ra (rd in this case)
; @expect-exit: 12

.entry main
.text
main:
    la    r5, subr
    jalr  r6, r5           ; r6 = PC+8, jump to subr
    nop
    addiu r4, r0, 12
    call  #0x001
    nop

subr:
    jr    r6
    nop
