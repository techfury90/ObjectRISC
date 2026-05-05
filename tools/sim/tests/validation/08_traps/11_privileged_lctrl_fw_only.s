; @description: LCTRL of a firmware-only control register (VECBASE) traps in supervisor mode
; @mode: supervisor
; @expect-trap: privileged-instruction
; @expect-trap-pc: 0x10000

.entry main
.text
main:
    lctrl r2, $8           ; 0x10000 — VECBASE is fw-only → trap
    call  #0x001
    nop
