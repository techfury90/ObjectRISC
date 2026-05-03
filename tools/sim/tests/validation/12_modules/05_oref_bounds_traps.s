; @description: OREFLD past the object's length traps bounds-violation
; @expect-trap: bounds-violation

.entry main
.text
main:
    addiu r4, r0, 8               ; one slot
    addiu r5, r0, 0
    addiu r6, r0, 0x43
    call  #0x106                  ; ObjAllocStore -> O1
    omov  o9, o1
    orefld o5, 8(o9)              ; offset 8 (would read 8..15, but length is 8)
    addiu r4, r0, 0
    call  #0x001
    nop
