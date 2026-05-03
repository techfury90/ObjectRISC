; @description: The branch delay slot ALWAYS executes, even when the branch is taken.
; r4 starts at 5, branch taken, delay slot increments to 6, exit 6.
; @expect-exit: 6

.entry main
.text
main:
    addiu r4, r0, 5
    beq   r0, r0, target   ; always taken
    addiu r4, r4, 1        ; delay slot — runs even though branch taken; r4 = 6
    addiu r4, r4, 100      ; SKIPPED — past the branch target
target:
    call  #0x001            ; exit r4 = 6
    nop
