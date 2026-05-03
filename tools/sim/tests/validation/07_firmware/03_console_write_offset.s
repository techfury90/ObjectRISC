; @description: ConsoleWrite of bytes 7..11 -> "world"
; @expect-stdout: "world"
; @expect-exit: 0

.entry main
.text
main:
    omov  o1, o3
    addiu r4, r0, 7        ; offset = 7 ('w')
    addiu r5, r0, 5        ; count = 5
    call  #0x320
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "Hello, world!\n"
