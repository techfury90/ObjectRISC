; @description: SCTRL in user mode raises privileged-instruction
; @mode: user
; @expect-trap: privileged-instruction
; @expect-trap-pc: 0x10000

.entry main
.text
main:
    sctrl $0, r0           ; 0x10000 — write STATUS; user mode → trap
    call  #0x001
    nop
