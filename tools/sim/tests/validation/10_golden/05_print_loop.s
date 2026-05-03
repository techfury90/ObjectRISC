; @description: Loop printing "X" three times -> "XXX"
; @expect-stdout: "XXX"
; @expect-exit: 0

.entry main
.text
main:
    addiu r6, r0, 3        ; loop counter (callee-preserved would be ideal, but caller-saved for simplicity since CALL preserves)
loop:
    beq   r6, r0, done
    nop
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 1
    call  #0x320
    addiu r6, r6, -1
    j     loop
    nop
done:
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "X"
