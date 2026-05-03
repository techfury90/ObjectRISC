; @description: CALL has NO delay slot. The instruction immediately after CALL must NOT
; execute before dispatch. We set R5=5 (count for ConsoleWrite), then put a
; bogus R5=99 immediately after the CALL. If CALL had a delay slot, the bogus
; assignment would happen first and ConsoleWrite would write 99 bytes (and trap).
; Since CALL has no delay slot, ConsoleWrite reads R5=5 and writes "Hello"; the
; bogus assignment runs only after the call returns.
; @expect-stdout: "Hello"
; @expect-exit: 0

.entry main
.text
main:
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 5
    call  #0x320           ; ConsoleWrite — should see R5=5
    addiu r5, r0, 99       ; runs AFTER the call returns; harmless
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "Hello, world!\n"
