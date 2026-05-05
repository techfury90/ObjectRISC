; @description: CALL of a supervisor-only primitive from user mode returns EPERM.
;
; MapObject (#0x110) requires supervisor mode. From user mode the dispatcher
; returns ERR_EPERM=3 in R2 without invoking the primitive; R2 is then passed
; to TaskExit so the process exit code is 3.
; @mode: user
; @expect-exit: 3

.entry main
.text
main:
    addiu r4, r0, 0       ; ref placeholder (any — we never reach the primitive)
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    call  #0x110          ; MapObject — supervisor required → R2 = EPERM
    addu  r4, r0, r2      ; exit with the returned errno (expected: 3)
    call  #0x001
    nop
