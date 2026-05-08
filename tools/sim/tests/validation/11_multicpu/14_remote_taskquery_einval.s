; @description: Phase 45d. TaskQuery on a remote ref pointing at a non-task descriptor (the other CPU's service object) routes via TASK_QUERY_REQ → home returns ERR_EINVAL because the descriptor exists but isn't TAG_TASK. Exercises the wire round-trip + home-side type check.
; @processors: 2
; @expect-exit: 1

.entry main
.text
main:
    bne   r7, r0, server   ; r7 = procid; CPU 1 just exits
    nop

client:                    ; CPU 0
    omov  o1, o5           ; O5 = CPU 1's service ref (R+S, home=1)
    call  #0x008           ; TaskQuery — remote, blocks for response
    nop
    addu  r4, r2, r0       ; exit with status (expect ERR_EINVAL = 1)
    call  #0x001
    nop

server:                    ; CPU 1 — just sit here so CPU 0 can route
    addiu r4, r0, 0
    call  #0x001
    nop
