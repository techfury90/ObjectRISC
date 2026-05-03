; @description: SW then LW: store 0x42 to stack, load it back
; @expect-exit: 0x42

.entry main
.text
main:
    addiu r2, r0, 0x42
    sw    r2, -16(sp)      ; store at sp-16
    lw    r4, -16(sp)      ; load it back
    call  #0x001
    nop
