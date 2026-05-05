; @description: ERET in user mode raises privileged-instruction
; @mode: user
; @expect-trap: privileged-instruction
; @expect-trap-pc: 0x10000

.entry main
.text
main:
    eret                    ; 0x10000 — user mode → trap
    call  #0x001
    nop
