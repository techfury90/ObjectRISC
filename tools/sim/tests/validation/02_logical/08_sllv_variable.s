; @description: SLLV uses low 5 bits of rs as shift count: 1 << 7 = 128
; @expect-exit: 128

.entry main
.text
main:
    addiu r2, r0, 1
    addiu r3, r0, 7
    sllv  r4, r2, r3
    call  #0x001
    nop
