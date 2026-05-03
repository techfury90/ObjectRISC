; @description: OEQ null,null does not trap and returns 1
; @expect-exit: 1

.entry main
.text
main:
    oeq   r4, o0, o0
    call  #0x001
    nop
