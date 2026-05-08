; @description: Phase 45d. TaskWait on a remote ref pointing at a non-task descriptor routes via TASK_WAIT_REQ → home returns ERR_EINVAL. Exercises the wire round-trip + home-side type check (same as TaskQuery, but through the wait path which can also block on a real task).
; @processors: 2
; @expect-exit: 1

.entry main
.text
main:
    bne   r7, r0, server
    nop

client:                    ; CPU 0
    omov  o1, o5           ; O5 = CPU 1's service ref (not a task)
    call  #0x007           ; TaskWait — remote, blocks for response
    nop
    addu  r4, r2, r0       ; exit with status (expect ERR_EINVAL = 1)
    call  #0x001
    nop

server:                    ; CPU 1
    addiu r4, r0, 0
    call  #0x001
    nop
