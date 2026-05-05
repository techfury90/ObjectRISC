; @description: TaskKill (#0x00A) — main spawns a child that would loop forever, kills it with code 0x42, then TaskWaits on it to read the installed exit code, exits with that code
;
; Without TaskKill the child would never terminate and the test
; would hit @max-cycles. With it: TaskKill flips the child's
; state to EXITED with R4=0x42, drops it from the runnable
; queue, and wakes any waiters. main's subsequent TaskWait
; returns immediately with R3=0x42.
;
; Same boot-OR pattern as the other 14_tasks tests: O5 saves
; the bootstrap code ref before TaskCreate clobbers O1.
; @expect-exit: 0x42
; @max-cycles: 50000

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code ref

    ; Stack for child.
    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101         ; TAG_STACK
    addiu r6, r0, 0x43
    call  #0x100
    bne   r2, r0, fail
    nop
    omov  o2, o1                 ; O2 = child stack

    ; TaskCreate(O1=code, O2=stack, R4=child entry, R5=0).
    omov  o1, o5
    la    r4, child
    lui   r9, 1
    subu  r4, r4, r9
    addiu r5, r0, 0
    call  #0x000
    bne   r2, r0, fail
    nop
    omov  o6, o1                 ; O6 = child task ref

    ; Resume the child so it's RUNNABLE (we don't yield to it; we
    ; just want it on the queue so TaskKill has to remove it).
    omov  o1, o6
    call  #0x002                 ; TaskResume
    bne   r2, r0, fail
    nop

    ; Kill the child with exit code 0x42.
    omov  o1, o6
    addiu r4, r0, 0x42
    call  #0x00A                 ; TaskKill
    bne   r2, r0, fail
    nop

    ; Killing again should be idempotent (returns OK).
    omov  o1, o6
    addiu r4, r0, 0xFF           ; ignored — already EXITED
    call  #0x00A
    bne   r2, r0, fail
    nop

    ; TaskWait should return immediately with R3 = the kill code.
    omov  o1, o6
    call  #0x007
    bne   r2, r0, fail
    nop
    move  r4, r3                 ; r4 = exit code (0x42)
    call  #0x001                 ; TaskExit
    nop

child:
    ; Spin forever — only TaskKill can stop us.
spin:
    j     spin
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
