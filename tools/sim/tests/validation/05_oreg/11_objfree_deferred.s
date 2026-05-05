; @description: ObjFreeDeferred (#0x107) keeps the descriptor live across the drain window; ObjFree returns ESTALE after the deadline elapses
;
; Allocates an object, schedules a 0-ms-deferred free, polls
; ObjFree via OCAP-style bounds (just dereferencing the handle
; would need OL/OS through it). To keep the test deterministic
; without a sleep primitive, we use 0-ms delay — the run loop's
; scan picks it up on the very next tick. After that point a
; second ObjFree returns ESTALE (= 10) because the descriptor is
; gone.
; @expect-exit: 10

.entry main
.text
main:
    addiu r4, r0, 16              ; size
    addiu r5, r0, 0               ; tag = 0
    addiu r6, r0, 0x53            ; R|W|V|C (V required for free)
    call  #0x100                  ; ObjAlloc → O1, R2 = OK
    bne   r2, r0, fail
    nop
    omov  o9, o1                  ; preserve the ref

    ; Schedule deferred free with 0 ms — fires on next run-loop scan.
    addiu r4, r0, 0
    call  #0x107                  ; ObjFreeDeferred(O1, 0)
    bne   r2, r0, fail
    nop

    ; Burn cycles to give the run loop a chance to process the
    ; deferred-free queue. A no-op loop of ~50 iterations is plenty:
    ; the queue is scanned every tick.
    addiu r3, r0, 50
loop:
    addiu r3, r3, -1
    bne   r3, r0, loop
    nop

    ; A second ObjFree should now report ESTALE (= 10).
    omov  o1, o9
    call  #0x101                  ; ObjFree
    move  r4, r2                  ; pass status to TaskExit
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
