; @description: blocked-on-queue task yields the CPU — main parks itself blocked on an empty queue with an infinite timeout, scheduler switches to the runnable child, child SENDs to the queue and exits, main unblocks and exits with the payload value
;
; Phase 36 architectural fix: without blocked-task preemption, a
; current_task that goes BLOCKED on IPC would hold the CPU forever
; even with another task runnable — single-CPU configurations could
; deadlock just by having two tasks where one waits on the other.
; With blocked-task preemption: the scheduler notices the current
; task can't unblock right now, save_cpu_to_task checkpoints its
; blocked_on into the Task struct, load_task_to_cpu paints the next
; runnable, and the blocked task is rescheduled later when its
; condition is met (handled by _wake_blocked_tasks).
;
; main and child share a service object with an attached queue. The
; service ref is parked in O7 so child inherits it via OPR copy.
; @expect-exit: 0x55
; @max-cycles: 50000

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code

    ; Allocate a service object; cap mask 0x5B = S|V|R|W|Q.
    addiu r4, r0, 64
    addiu r5, r0, 0              ; TAG_SERVICE
    addiu r6, r0, 0x5B
    addiu r7, r0, 0
    call  #0x100
    bne   r2, r0, fail
    nop
    omov  o7, o1                 ; O7 = service (inherited by child)

    ; Attach a 1-deep receive queue.
    omov  o1, o7
    addiu r4, r0, 1
    call  #0x203                 ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop

    ; Allocate a stack for child.
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
    call  #0x002                 ; TaskResume
    nop

    ; Block on the queue with infinite timeout. Without blocked-task
    ; preemption this would hang the CPU forever (child never gets
    ; to run); with it, the scheduler switches to child, child SENDs,
    ; main is rescheduled, _wake_blocked_tasks promotes us back to
    ; RUNNABLE, _try_unblock delivers the message, exit with R3.
    omov  o1, o7
    addiu r4, r0, -1             ; timeout = 0xFFFFFFFF
    call  #0x204                 ; ReceiveQueuePoll
    bne   r2, r0, fail
    nop

    move  r4, r3                 ; R3 = first int payload word (= 0x55)
    call  #0x001                 ; TaskExit
    nop

child:
    omov  o1, o7
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0x55
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1
    addiu r4, r0, 0
    call  #0x001                 ; TaskExit
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
