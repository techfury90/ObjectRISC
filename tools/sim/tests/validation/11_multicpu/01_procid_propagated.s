; @description: With --processors 2, each CPU sees its PID in R7. CPU 0 exits with R4=10, CPU 1 with R4=11. Sim returns CPU 0's exit code.
; @processors: 2
; @expect-exit: 10

.entry main
.text
main:
    addiu r4, r0, 10
    add   r4, r4, r7       ; r4 = 10 + procid
    call  #0x001
    nop
