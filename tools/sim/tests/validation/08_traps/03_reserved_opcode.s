; @description: Major opcode 0x10 (reserved for FP) traps with reserved-instruction
; @expect-trap: reserved-instruction

.entry main
.text
main:
    .word 0x40000000        ; opcode 0x10 (FP coprocessor) — reserved this revision
    call  #0x001
    nop
