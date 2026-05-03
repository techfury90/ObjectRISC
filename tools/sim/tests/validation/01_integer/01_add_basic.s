; @description: ADD: 3 + 5 = 8 returned via exit code
; @expect-exit: 8

.entry main
.text
main:
    addiu r2, r0, 3
    addiu r3, r0, 5
    add   r4, r2, r3       ; r4 = 8
    call  #0x001            ; TaskExit
    nop
