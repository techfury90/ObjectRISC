; @description: ObjFreeDeferred refuses to defer a TAG_TASK descriptor (returns EINVAL)
;
; Tasks have side state (cpu.tasks Python dict, scheduler queue
; entries) the deferral path can't safely outlast — the immediate
; ObjFree path evicts that state synchronously. Refuse rather
; than break the scheduler invariant.
; @expect-exit: 1

.entry main
.text
main:
    ; Save bootstrap code ref before ObjAlloc clobbers O1.
    omov  o9, o1

    ; Allocate a stack for the child.
    addiu r4, r0, 0x1000
    addiu r5, r0, 0x4101          ; TAG_STACK
    addiu r6, r0, 0x43
    call  #0x100
    bne   r2, r0, fail
    nop
    omov  o2, o1                  ; O2 = stack
    omov  o1, o9                  ; O1 = code (bootstrap's)

    ; TaskCreate(R4 = entry offset = child label, R5 = 0 init_r4)
    la    r4, child
    lui   r5, 1
    subu  r4, r4, r5              ; entry offset = child - CODE_VA
    addiu r5, r0, 0
    call  #0x000                  ; TaskCreate → O1 = task ref
    bne   r2, r0, fail
    nop

    ; Try ObjFreeDeferred on the task ref — must return EINVAL (1).
    addiu r4, r0, 0
    call  #0x107
    addiu r3, r0, 1               ; EINVAL
    bne   r2, r3, fail
    nop

    ; Pass — exit 1 (the expected EINVAL we just verified).
    addiu r4, r0, 1
    call  #0x001
    nop

child:
    addiu r4, r0, 0
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
