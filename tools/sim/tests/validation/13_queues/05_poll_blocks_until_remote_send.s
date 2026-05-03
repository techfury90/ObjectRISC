; @description: Multi-CPU. CPU 0 attaches a queue and polls with infinite timeout — blocks. CPU 1 SENDs to it. CPU 0 unblocks and exits with the payload.
; @processors: 2
; @expect-exit: 0x55

.entry main
.text
main:
    bne   r7, r0, sender
    nop

receiver:                         ; CPU 0
    omov  o1, o4                  ; my service
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop

    omov  o1, o4
    addiu r4, r0, -1              ; timeout = 0xFFFFFFFF (infinite)
    call  #0x204                  ; blocks until CPU 1's SEND arrives
    bne   r2, r0, fail
    nop
    move  r4, r3                  ; payload R3 = SEND's R4
    call  #0x001
    nop

sender:                           ; CPU 1
    omov  o1, o5                  ; recipient = CPU 0's service
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0x55
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1
    addiu r4, r0, 0
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
