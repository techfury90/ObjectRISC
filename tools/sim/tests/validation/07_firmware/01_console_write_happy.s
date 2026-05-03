; @description: ConsoleWrite happy path: write entire 14-byte data object
; @expect-stdout: "Hello, world!\n"
; @expect-exit: 0

.entry main
.text
main:
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 14
    call  #0x320           ; ConsoleWrite
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "Hello, world!\n"
