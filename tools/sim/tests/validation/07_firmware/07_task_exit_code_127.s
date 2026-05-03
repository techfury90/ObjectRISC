; @description: TaskExit with R4 = 127
; @expect-exit: 127

.entry main
.text
main:
    addiu r4, r0, 127
    call  #0x001
    nop
