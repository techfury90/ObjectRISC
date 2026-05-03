; @description: OSB then OLB through the stack object O2 (which has W cap)
; Store 0x33 at O2+0, load it back; exit 0x33 = 51
; @expect-exit: 51

.entry main
.text
main:
    addiu r2, r0, 0x33
    osb   r2, 0(o2)
    olbu  r4, 0(o2)
    call  #0x001
    nop
