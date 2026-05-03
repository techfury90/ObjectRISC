; @description: ONULL o5; then OISN says yes (1)
; @expect-exit: 1

.entry main
.text
main:
    omov  o5, o3           ; deliberately non-null first
    onull o5               ; clear it
    oisn  r4, o5
    call  #0x001
    nop

.data
    .string "x"
