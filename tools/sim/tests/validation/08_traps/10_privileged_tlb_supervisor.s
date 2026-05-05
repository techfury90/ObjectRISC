; @description: TLBWR in supervisor mode raises privileged-instruction (firmware-only)
; @mode: supervisor
; @expect-trap: privileged-instruction
; @expect-trap-pc: 0x10000

.entry main
.text
main:
    tlbwr                   ; 0x10000 — supervisor mode insufficient → trap
    call  #0x001
    nop
