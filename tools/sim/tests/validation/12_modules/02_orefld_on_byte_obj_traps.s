; @description: OREFLD on a byte-typed object (here: O3, the data section) traps capability-violation
; @expect-trap: capability-violation

.entry main
.text
main:
    orefld o5, 0(o3)              ; O3 has objstore=False → trap
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .skip 8
