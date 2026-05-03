; @description: Writes to R0 are silently discarded; R0 still reads as zero
; addu r0, r0, r4 (with r4=99) leaves r0 = 0; we then exit r0 → 0
; @expect-exit: 0

.entry main
.text
main:
    addiu r4, r0, 99
    addu  r0, r0, r4       ; would write 99 to r0, but r0 is hardwired
    move  r4, r0           ; reads back 0
    call  #0x001
    nop
