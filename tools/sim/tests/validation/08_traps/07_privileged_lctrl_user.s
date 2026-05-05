; @description: LCTRL in user mode raises privileged-instruction (Vol II §13)
; @mode: user
; @expect-trap: privileged-instruction
; @expect-trap-pc: 0x10000

.entry main
.text
main:
    lctrl r2, $0           ; 0x10000 — read STATUS; user mode → trap
    call  #0x001
    nop
