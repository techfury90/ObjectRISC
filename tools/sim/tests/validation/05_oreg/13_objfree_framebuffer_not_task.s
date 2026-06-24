; @description: ObjFree and ObjFreeDeferred treat a TAG_FRAMEBUFFER as a plain object, not a task
;
; Regression for a type-tag collision: TAG_FRAMEBUFFER and TAG_TASK
; were both 0x4104. The free paths branch on `type_tag == TAG_TASK`,
; so a framebuffer was misclassified as a task. The immediate ObjFree
; path survived by luck (no Python Task struct keyed at that index),
; but ObjFreeDeferred unconditionally rejected a "task" with EINVAL —
; so a framebuffer could not be freed through the drain-window path.
; With TAG_FRAMEBUFFER moved to a distinct value both frees return OK.
; @expect-exit: 0

.entry main
.text
main:
    ; --- Framebuffer A: immediate ObjFree (#0x101) must return OK ---
    addiu r4, r0, 16              ; width
    addiu r5, r0, 16              ; height
    addiu r6, r0, 0x53            ; caps R|W|V|C (V needed to free)
    addiu r7, r0, 1              ; FB_FLAG_OFFSCREEN — no host display worker
    call  #0x10A                 ; ObjAllocFramebuffer → O1 = ref, R2 = status
    bne   r2, r0, fail
    nop

    call  #0x101                 ; ObjFree(O1) → R2 = status
    bne   r2, r0, fail           ; must be OK, not mis-routed through task reap
    nop

    ; --- Framebuffer B: deferred ObjFree (#0x107) must return OK ---
    ; This is the path that broke under the collision: a TAG_TASK
    ; descriptor is refused with EINVAL, and a framebuffer used to
    ; share that tag.
    addiu r4, r0, 16             ; width
    addiu r5, r0, 16             ; height
    addiu r6, r0, 0x53           ; caps R|W|V|C
    addiu r7, r0, 1              ; FB_FLAG_OFFSCREEN
    call  #0x10A                 ; ObjAllocFramebuffer → O1 = ref, R2 = status
    bne   r2, r0, fail
    nop

    addiu r4, r0, 0              ; drain delay = 0 ms
    call  #0x107                 ; ObjFreeDeferred(O1) → R2 = status
    bne   r2, r0, fail           ; must be OK; EINVAL would mean still misclassified
    nop

    ; Both frees accepted — pass.
    addiu r4, r0, 0
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
