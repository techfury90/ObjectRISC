; @description: Phase 45d. CPU 1 creates a long-running child (busy-loop), TaskResumes it, SENDs the child ref to CPU 0. CPU 0's handler TaskKills the (remote-home) child with exit_code=0x99 — routes via TASK_KILL_REQ → home transitions the task to EXITED. CPU 0 then TaskQuerys the child to confirm: state==EXITED (5) and exit_code==0x99. Exits with the recovered exit_code.
; @processors: 2
; @max-cycles: 100000
; @expect-exit: 0x99

.entry main
.text
main:
    bne   r7, r0, server
    nop

client:
    omov  o15, o3
    omov  o14, o1

    omov  o1, o4
    omov  o2, o14
    la    r5, client_handler
    lui   r6, 1
    subu  r4, r5, r6
    call  #0x200
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

client_handler:
    omov  o5, o3                 ; preserve child ref

    ; Kill the remote child with exit_code=0x99.
    omov  o1, o5
    addiu r4, r0, 0x99
    call  #0x00A                 ; TaskKill — remote
    nop
    bne   r2, r0, kill_failed    ; R2 should be ERR_OK
    nop

    ; Query to confirm EXITED with our exit_code.
poll_after_kill:
    omov  o1, o5
    call  #0x008                 ; TaskQuery — remote
    nop
    addu  r6, r3, r0
    andi  r4, r3, 0xFF
    addiu r5, r0, 5              ; TASK_STATE_EXITED
    bne   r4, r5, poll_after_kill
    nop

    srl   r4, r6, 16
    andi  r4, r4, 0xFFFF         ; recovered exit_code
    call  #0x001
    nop

kill_failed:
    addu  r4, r2, r0             ; surface the kill status as exit code on failure
    call  #0x001
    nop

server:
    omov  o6, o1

    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x53
    call  #0x100
    nop
    omov  o2, o1

    omov  o1, o6
    la    r4, child_entry
    lui   r6, 1
    subu  r4, r4, r6
    addiu r5, r0, 0
    call  #0x000                 ; TaskCreate
    nop
    omov  o7, o1

    call  #0x002                 ; TaskResume
    nop

    omov  o1, o5                 ; SEND to CPU 0
    omov  o2, o7                 ; child ref
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    ; Server itself sleeps in TaskYield forever — the child needs the
    ; CPU to be alive so the home-side _process_requests can answer
    ; TASK_KILL_REQ from CPU 0.
server_loop:
    call  #0x004                 ; TaskYield
    nop
    j     server_loop
    nop

child_entry:
    ; Busy-loop until killed externally. Just spin.
spin:
    j     spin
    nop
