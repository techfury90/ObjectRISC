; @description: Phase 45d. CPU 1 creates a child task that exits 0x55, SENDs the child ref to CPU 0. CPU 0's handler TaskQuerys the (remote-home) child ref repeatedly until state == TASK_STATE_EXITED (5), then exits with the exit code from the upper 16 bits of the packed state word.
; @processors: 2
; @max-cycles: 50000
; @expect-exit: 0x55

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
    call  #0x200                 ; InstallHandler
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

client_handler:
    omov  o5, o3                 ; preserve child ref in O5 (we need O1 fresh each iteration)
poll_loop:
    omov  o1, o5
    call  #0x008                 ; TaskQuery — remote
    nop
    ; R3 packed state word: low 8 bits = state, next 8 = proc, upper 16 = exit code.
    addu  r6, r3, r0             ; preserve full packed word
    andi  r4, r3, 0xFF           ; r4 = state
    addiu r5, r0, 5              ; TASK_STATE_EXITED
    bne   r4, r5, poll_loop
    nop
    ; State is EXITED; extract exit code from upper 16 bits.
    srl   r4, r6, 16
    andi  r4, r4, 0xFFFF
    call  #0x001                 ; exit with the recovered code
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
    addiu r5, r0, 0x55
    call  #0x000                 ; TaskCreate → O1 = child
    nop
    omov  o7, o1

    call  #0x002                 ; TaskResume
    nop

    omov  o1, o5                 ; SEND to CPU 0's service
    omov  o2, o7                 ; payload OR0 = child ref
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    addiu r4, r0, 0
    call  #0x001
    nop

child_entry:
    call  #0x001
    nop
