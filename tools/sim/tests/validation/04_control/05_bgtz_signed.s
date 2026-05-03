; @description: BGTZ taken on 1 (signed gt 0); exit 5
; @expect-exit: 5

.entry main
.text
main:
    addiu r2, r0, 1
    bgtz  r2, taken
    nop
    addiu r4, r0, 0
    call  #0x001
    nop
taken:
    addiu r4, r0, 5
    call  #0x001
    nop
