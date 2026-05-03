; 04_directives.s — exercise every directive.

.entry main

.text
main:
    nop

.data
str1:
    .string "hi"
str2:
    .asciz  "ok"
nums:
    .byte   1, 2, -1
    .half   0x1234, -1
    .word   0xCAFEBABE
    .align  3            ; cur is 16; already aligned to 8. No pad.
    .skip   2
    .align  2            ; cur is 18; pad two bytes to reach 20.
