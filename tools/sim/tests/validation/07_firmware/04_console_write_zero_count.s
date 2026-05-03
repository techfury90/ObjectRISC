; @description: ConsoleWrite with count=0: no output, status OK, exit 0
; @expect-stdout: ""
; @expect-exit: 0

.entry main
.text
main:
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 0
    call  #0x320
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "ignored"
