; @description: ADDU silently wraps INT_MAX + 0x42 to 0x80000041 (low byte 0x41 = 65)
; @expect-exit: 65

.entry main
.text
main:
    lui   r2, 0x7fff
    ori   r2, r2, 0xffff   ; r2 = 0x7FFFFFFF
    addiu r3, r0, 0x42
    addu  r4, r2, r3       ; wraps to 0x80000041; no trap
    call  #0x001
    nop
