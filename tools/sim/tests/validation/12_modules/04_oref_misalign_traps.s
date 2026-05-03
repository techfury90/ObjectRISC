; @description: OREFLD with non-8-aligned offset traps address-misaligned-d
; @expect-trap: address-misaligned-d

.entry main
.text
main:
    addiu r4, r0, 16
    addiu r5, r0, 0
    addiu r6, r0, 0x43
    call  #0x106                  ; ObjAllocStore -> O1
    omov  o9, o1
    orefld o5, 4(o9)              ; offset 4 — not 8-aligned
    addiu r4, r0, 0
    call  #0x001
    nop
