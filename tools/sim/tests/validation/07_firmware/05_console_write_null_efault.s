; @description: ConsoleWrite with O1=null returns EFAULT (8); we exit with R2 to verify
; @expect-exit: 8

.entry main
.text
main:
    onull o1
    addiu r4, r0, 0
    addiu r5, r0, 1
    call  #0x320           ; R2 should be 8 (EFAULT)
    move  r4, r2
    call  #0x001
    nop
