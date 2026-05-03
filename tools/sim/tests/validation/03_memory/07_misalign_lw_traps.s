; @description: LW from an odd address traps with address-misaligned-d
; @expect-trap: address-misaligned-d

.entry main
.text
main:
    addiu r2, sp, -15      ; deliberately not 4-aligned
    lw    r4, 0(r2)
    call  #0x001
    nop
