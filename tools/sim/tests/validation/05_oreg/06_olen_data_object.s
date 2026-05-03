; @description: OLEN on the 14-byte data object returns 14
; @expect-exit: 14

.entry main
.text
main:
    olen  r4, o3
    call  #0x001
    nop

.data
    .string "Hello, world!\n"   ; exactly 14 bytes
