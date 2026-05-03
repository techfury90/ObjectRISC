; @description: JR Rs jumps to address in Rs; load target via `la` pseudo
; @expect-exit: 11

.entry main
.text
main:
    la    r5, target
    jr    r5
    nop
    addiu r4, r0, 0        ; skipped
    call  #0x001
    nop
target:
    addiu r4, r0, 11
    call  #0x001
    nop
