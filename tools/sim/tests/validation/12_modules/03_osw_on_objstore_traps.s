; @description: OSW on an OBJSTORE object traps capability-violation (integer access not allowed)
; @expect-trap: capability-violation

.entry main
.text
main:
    addiu r4, r0, 8
    addiu r5, r0, 0
    addiu r6, r0, 0x43            ; R|W|C
    call  #0x106                  ; ObjAllocStore -> O1
    omov  o9, o1
    addiu r2, r0, 0x42
    osw   r2, 0(o9)               ; integer store on OBJSTORE → trap
    addiu r4, r0, 0
    call  #0x001
    nop
