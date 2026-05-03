; @description: DIV 100 / 7 -> quot=14 in LO, rem=2 in HI; we add them and exit (16)
; @expect-exit: 16

.entry main
.text
main:
    addiu r2, r0, 100
    addiu r3, r0, 7
    div   r2, r3           ; LO=14, HI=2
    mflo  r5               ; r5 = 14
    mfhi  r6               ; r6 = 2
    add   r4, r5, r6       ; r4 = 16
    call  #0x001
    nop
