; @description: ConsoleWrite with count > length returns EINVAL (1)
; @expect-exit: 1

.entry main
.text
main:
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 100      ; way past 5-byte object
    call  #0x320
    move  r4, r2           ; R2 = EINVAL = 1
    call  #0x001
    nop

.data
    .string "Hello"
