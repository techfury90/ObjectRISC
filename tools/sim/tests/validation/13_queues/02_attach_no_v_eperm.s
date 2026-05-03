; @description: ReceiveQueueAttach without V cap on the target returns EPERM (3)
; @expect-exit: 3

.entry main
.text
main:
    addiu r4, r0, 64
    addiu r5, r0, 0
    addiu r6, r0, 0x09            ; R|S only — no V
    addiu r7, r0, 0
    call  #0x100
    bne   r2, r0, fail
    nop
    addiu r4, r0, 1
    call  #0x203                  ; -> R2 = EPERM
    move  r4, r2
    call  #0x001
    nop
fail:
    addiu r4, r0, 99
    call  #0x001
    nop
