; @description: OEQ on O1 (code) vs O3 (data): different objects -> 0
; @expect-exit: 0

.entry main
.text
main:
    oeq   r4, o1, o3
    call  #0x001
    nop

.data
    .string "x"
