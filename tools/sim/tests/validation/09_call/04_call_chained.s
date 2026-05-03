; @description: Two CALLs in sequence both run; ConsoleWrite then TaskExit.
; Validates that PC advances cleanly past each.
; @expect-stdout: "Hi"
; @expect-exit: 0

.entry main
.text
main:
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 2
    call  #0x320           ; ConsoleWrite: writes "Hi"
    addiu r4, r0, 0
    call  #0x001           ; TaskExit
    nop

.data
    .string "Hi"
