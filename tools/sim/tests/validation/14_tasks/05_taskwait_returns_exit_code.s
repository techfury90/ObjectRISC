; @description: bootstrap creates a child, TaskWaits on it; child exits 0x37; bootstrap resumes with R3=0x37 and exits with that
;
; Proves the BLOCKED → RUNNABLE wakeup path: child's TaskExit walks
; its waiters list, sets each waiter's R3 to its exit code, queues
; the waiter back, and the scheduler picks it up. The waiter's PC
; was bumped past the TaskWait CALL before save, so it resumes at
; the next instruction.
; @expect-exit: 0x37

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code

    ; Allocate child stack.
    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x43
    call  #0x100
    omov  o2, o1

    omov  o1, o5
    la    r4, child_entry
    lui   r9, 1
    subu  r4, r4, r9
    addiu r5, r0, 0x37           ; child's R4 on entry
    call  #0x000                 ; TaskCreate → O1 = child
    call  #0x002                 ; TaskResume

    ; TaskWait blocks until child exits; on resume R3 = child's code.
    call  #0x007                 ; TaskWait(O1)
    move  r4, r3
    call  #0x001                 ; exit with the awaited code
    nop

child_entry:
    ; init_r4 = 0x37 placed by TaskCreate.
    call  #0x001
    nop
