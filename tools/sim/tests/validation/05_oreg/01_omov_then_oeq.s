; @description: OMOV o5,o3 then OEQ on the two should return 1 (same ref)
; @expect-exit: 1

.entry main
.text
main:
    omov  o5, o3
    oeq   r4, o5, o3
    call  #0x001
    nop

.data
    .string "needs data so O3 is non-null"
