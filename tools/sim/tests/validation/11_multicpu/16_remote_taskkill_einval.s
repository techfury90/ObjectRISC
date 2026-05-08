; @description: Phase 45d. TaskKill on a remote ref pointing at a non-task descriptor routes via TASK_KILL_REQ → home returns ERR_EINVAL.
; @processors: 2
; @expect-exit: 1

.entry main
.text
main:
    bne   r7, r0, server
    nop

client:                    ; CPU 0
    omov  o1, o5           ; O5 = CPU 1's service ref (not a task)
    addiu r4, r0, 0        ; exit_code argument (irrelevant for EINVAL path)
    call  #0x00A           ; TaskKill — remote, blocks for response
    nop
    addu  r4, r2, r0       ; exit with status (expect ERR_EINVAL = 1)
    call  #0x001
    nop

server:                    ; CPU 1
    addiu r4, r0, 0
    call  #0x001
    nop
