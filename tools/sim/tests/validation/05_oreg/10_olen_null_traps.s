; @description: OLEN on null reference traps with null-dereference
; @expect-trap: null-dereference

.entry main
.text
main:
    olen  r4, o0
    call  #0x001
    nop
