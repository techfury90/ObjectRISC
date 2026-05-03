; @description: Iterative factorial of 5 = 120; exit code 120
; @expect-exit: 120

.entry main
.text
main:
    addiu r4, r0, 5        ; n = 5
    addiu r2, r0, 1        ; result = 1
loop:
    blez  r4, done
    nop
    mult  r2, r4
    mflo  r2               ; result *= n
    addiu r4, r4, -1
    j     loop
    nop
done:
    move  r4, r2
    call  #0x001
    nop
