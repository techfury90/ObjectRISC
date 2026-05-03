; @description: After CALL completes, execution resumes at PC_call + 4.
; We CALL ConsoleWrite, then set R4=42, then TaskExit. Exit code is 42.
; @expect-stdout: "x"
; @expect-exit: 42

.entry main
.text
main:
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 1
    call  #0x320
    addiu r4, r0, 42
    call  #0x001
    nop

.data
    .string "x"
