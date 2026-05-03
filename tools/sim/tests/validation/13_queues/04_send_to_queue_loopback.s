; @description: Single CPU loopback. Attach a queue, SEND to self with R4 payload, poll, exit with payload (which arrives as R3 per Vol VI Section 6).
; @expect-exit: 0x42

.entry main
.text
main:
    addiu r4, r0, 64
    addiu r5, r0, 0
    addiu r6, r0, 0x5B
    addiu r7, r0, 0
    call  #0x100
    bne   r2, r0, fail
    nop
    omov  o9, o1                  ; preserve queue ref

    omov  o1, o9
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop

    ; SEND to self — message goes straight to the queue (no handler dispatch).
    omov  o1, o9
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0x42
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    ; Poll immediately — message should be there.
    omov  o1, o9
    addiu r4, r0, 0               ; timeout=0 (no need to wait — already in queue)
    call  #0x204                  ; ReceiveQueuePoll
    bne   r2, r0, fail
    nop
    move  r4, r3                  ; first int payload word arrives in R3
    call  #0x001
    nop
fail:
    addiu r4, r0, 99
    call  #0x001
    nop
