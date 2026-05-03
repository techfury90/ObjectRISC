; @description: OSB through O3 (data object lacks W) traps with capability-violation
; @expect-trap: capability-violation

.entry main
.text
main:
    addiu r2, r0, 0x42
    osb   r2, 0(o3)        ; O3 only has R+C, no W
    call  #0x001
    nop

.data
    .string "x"
