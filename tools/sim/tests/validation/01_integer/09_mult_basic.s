; @description: MULT 7 * 11 = 77 in LO; MFLO retrieves it
; @expect-exit: 77

.entry main
.text
main:
    addiu r2, r0, 7
    addiu r3, r0, 11
    mult  r2, r3
    mflo  r4
    call  #0x001
    nop
