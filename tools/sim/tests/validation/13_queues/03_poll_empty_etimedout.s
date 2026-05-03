; @description: ReceiveQueuePoll on empty queue with timeout=0 returns ETIMEOUT (7) immediately
; @expect-exit: 7

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
    omov  o9, o1
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop
    omov  o1, o9
    addiu r4, r0, 0               ; timeout = 0 — return immediately
    call  #0x204                  ; ReceiveQueuePoll
    move  r4, r2                  ; should be ETIMEOUT = 7
    call  #0x001
    nop
fail:
    addiu r4, r0, 99
    call  #0x001
    nop
