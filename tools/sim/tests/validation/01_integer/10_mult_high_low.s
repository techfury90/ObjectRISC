; @description: MULT 0x10000 * 0x10000 -> HI=1, LO=0; MFHI returns 1
; @expect-exit: 1

.entry main
.text
main:
    lui   r2, 1            ; r2 = 0x00010000
    move  r3, r2
    mult  r2, r3           ; product = 0x100000000 → HI=1, LO=0
    mfhi  r4
    call  #0x001
    nop
