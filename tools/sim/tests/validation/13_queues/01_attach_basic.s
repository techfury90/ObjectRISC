; @description: ReceiveQueueAttach succeeds on a local object with R+W+S+V+C caps
; @expect-exit: 0

.entry main
.text
main:
    addiu r4, r0, 64
    addiu r5, r0, 0
    addiu r6, r0, 0x5B            ; R|W|S|V|C
    addiu r7, r0, 0
    call  #0x100                  ; ObjAlloc -> O1
    bne   r2, r0, fail
    nop
    addiu r4, r0, 4               ; max_depth = 4
    call  #0x203                  ; ReceiveQueueAttach
    move  r4, r2                  ; should be 0 (OK)
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
