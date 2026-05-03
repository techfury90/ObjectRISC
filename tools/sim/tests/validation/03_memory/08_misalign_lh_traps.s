; @description: LH from an odd address traps with address-misaligned-d
; @expect-trap: address-misaligned-d

.entry main
.text
main:
    addiu r2, sp, -15      ; odd address
    lh    r4, 0(r2)
    call  #0x001
    nop
