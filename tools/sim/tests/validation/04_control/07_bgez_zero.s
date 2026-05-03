; @description: BGEZ taken on 0 (signed ge 0); exit 7
; @expect-exit: 7

.entry main
.text
main:
    bgez  r0, taken        ; r0 == 0 → ge 0 → taken
    nop
    addiu r4, r0, 0
    call  #0x001
    nop
taken:
    addiu r4, r0, 7
    call  #0x001
    nop
