; @description: Phase 45d. CPU 1 creates a child task locally (exits 0x42), SENDs the child ref to CPU 0 via O5 (CPU 0's service). CPU 0's handler calls TaskWait on the (remote-home) child ref, which routes via TASK_WAIT_REQ. The child eventually exits; _wake_waiters drains its remote_waiters list, sending TASK_WAIT_RESP back to CPU 0. CPU 0's handler resumes with R3 = 0x42 and exits with that.
; @processors: 2
; @max-cycles: 50000
; @expect-exit: 0x42

.entry main
.text
main:
    bne   r7, r0, server
    nop

;========================================================================
; CPU 0 (client) — installs a handler on its own service that TaskWaits
; on the remote child task ref delivered via SEND. The handler exits
; the CPU with the awaited child's exit code in R4.
;========================================================================
client:
    omov  o15, o3                ; preserve data ref for the handler
    omov  o14, o1                ; preserve bootstrap code ref

    ; Install client_handler on my own service (O4, full caps).
    omov  o1, o4
    omov  o2, o14                ; handler code object = bootstrap code
    la    r5, client_handler
    lui   r6, 1
    subu  r4, r5, r6             ; offset into code object
    call  #0x200                 ; InstallHandler
    nop

    ; Bootstrap exits — the handler waits for the inbound SEND to fire.
    addiu r4, r0, 0
    call  #0x001
    nop

client_handler:                  ; runs on CPU 0 when CPU 1's SEND arrives.
    ; SEND payload convention: client's O2 → handler's O3 (Vol III §7.2).
    ; CPU 1 packed the child task ref into O2; we get it in O3 here.
    omov  o1, o3                 ; O1 = remote child task ref (home=1)
    call  #0x007                 ; TaskWait — routes via TASK_WAIT_REQ,
                                 ;   blocks until the child exits, then
                                 ;   R2 = ERR_OK, R3 = exit code.
    nop
    addu  r4, r3, r0             ; exit with the awaited exit code
    call  #0x001
    nop

;========================================================================
; CPU 1 (server) — TaskCreate a child that exits 0x42, TaskResume it,
; SEND the child ref to CPU 0's service (O5 here = CPU 0's service ref
; in 2-proc mode), then exit. The child runs and exits at some point;
; CPU 0's TaskWait will wake either via the "already exited" fast path
; or via the remote_waiters drain on _wake_waiters.
;========================================================================
server:
    omov  o6, o1                 ; preserve bootstrap code ref

    ; Allocate child stack (TAG_STACK 0x4101, R+W+V+C = 0x53).
    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x53
    call  #0x100                 ; ObjAlloc → O1
    nop
    omov  o2, o1                 ; child stack

    ; TaskCreate(O1=code, O2=stack, R4=entry_offset, R5=init_r4).
    omov  o1, o6
    la    r4, child_entry
    lui   r6, 1
    subu  r4, r4, r6
    addiu r5, r0, 0x42           ; child's R4 on entry → its TaskExit code
    call  #0x000                 ; TaskCreate → O1 = child task ref
    nop
    omov  o7, o1                 ; preserve child ref for resume + send

    ; TaskResume the child (O1 already holds the ref).
    call  #0x002                 ; TaskResume
    nop

    ; SEND to CPU 0's service (O5, R+S in 2-proc mode), with the child
    ; ref in O2 so the dispatched handler sees it in O3.
    omov  o1, o5                 ; recipient = CPU 0's service
    omov  o2, o7                 ; payload OR slot 0 = child task ref
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1
    addiu r4, r0, 0
    call  #0x001                 ; CPU 1's bootstrap exits
    nop

child_entry:
    ; TaskCreate placed our R4 = 0x42 into the new task; TaskExit
    ; uses the low byte of R4 as the exit code.
    call  #0x001
    nop
