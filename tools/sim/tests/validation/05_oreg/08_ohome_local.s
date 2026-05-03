; @description: OHOME on any object on a single-CPU sim returns 0 (the sole processor)
; @expect-exit: 0

.entry main
.text
main:
    ohome r4, o3
    call  #0x001
    nop

.data
    .string "x"
