; @description: ObjFree on a TAG_TASK descriptor reaps the task once it has exited; freeing a live task returns EBUSY
;
; Bootstrap creates a child, attempts ObjFree before the child has
; exited (expects EBUSY), TaskWaits, then ObjFrees again (expects OK).
; Exits with the second ObjFree's return code in R4.
; @expect-exit: 0

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code

    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x43
    call  #0x100
    omov  o2, o1

    omov  o1, o5
    la    r4, child_entry
    lui   r9, 1
    subu  r4, r4, r9
    addiu r5, r0, 0
    call  #0x000                 ; TaskCreate → O1 = child
    omov  o6, o1                 ; preserve child ref across ObjFree clobber

    call  #0x002                 ; TaskResume

    ; Premature ObjFree on a live task: should report EBUSY (= 5).
    omov  o1, o6
    call  #0x101                 ; ObjFree
    addiu r8, r0, 5              ; EBUSY
    bne   r2, r8, fail
    nop

    ; Wait for child to exit, then reap.
    omov  o1, o6
    call  #0x007                 ; TaskWait

    omov  o1, o6
    call  #0x101                 ; ObjFree → expect OK (= 0)
    bne   r2, r0, fail
    nop

    ; Verify the descriptor really is gone: a second free returns ESTALE.
    omov  o1, o6
    call  #0x101
    addiu r8, r0, 10             ; ESTALE
    bne   r2, r8, fail
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop

child_entry:
    call  #0x001                 ; child exits 0
    nop
