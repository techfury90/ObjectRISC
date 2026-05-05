; @description: timer-driven preemption — main spins on a flag, timer handler yields, child sets flag and exits, main resumes via the next preempt and exits with the flag value
;
; This is the architectural fix from Phase 35. Without it the
; handler's TaskYield would clobber main's saved trap state and
; ERET would resume into junk. With deferred yield: the handler
; calls TaskYield (sets yield_pending), then ERET sees the flag,
; saves main's clean pre-trap state to its task struct, and
; switches to the child. The child runs to TaskExit; on its exit
; the scheduler picks main; main resumes its loop, sees the flag,
; exits with that value.
;
; main and child share a 4-byte scratch object (parked in O7
; before TaskCreate so the child inherits it via OPR copy).
; @expect-exit: 0x37
; @max-cycles: 50000

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code

    ; Shared scratch (4 bytes, R+W+C). Child writes 0x37; main reads.
    addiu r4, r0, 4
    addiu r5, r0, 0x4102         ; TAG_DATA
    addiu r6, r0, 0x43           ; R|W|C
    call  #0x100
    omov  o7, o1                 ; O7 = scratch (inherited by child)
    osw   r0, 0(o7)              ; flag = 0

    ; Stack for child.
    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x43
    call  #0x100
    omov  o2, o1                 ; O2 = child stack

    ; TaskCreate(O1=code, O2=stack, R4=child entry, R5=0)
    omov  o1, o5
    la    r4, child
    lui   r9, 1
    subu  r4, r4, r9
    addiu r5, r0, 0
    call  #0x000
    call  #0x002                 ; TaskResume

    ; Install supervisor handler for external-interrupt (cause 0x01).
    addiu r4, r0, 1
    la    r5, timer_handler
    call  #0x520

    ; Arm timer: COMPARE = COUNT + 50.
    lctrl r8, $5
    addiu r8, r8, 50
    sctrl $6, r8

    ; STATUS: mode=supervisor, IE=1.
    addiu r9, r0, 0x11
    sctrl $0, r9

    ; Tight loop polling the flag. Without preemption main never
    ; yields and child never runs. With preemption the timer fires,
    ; handler yields, child runs+exits, scheduler returns to main,
    ; flag is 0x37 → exit.
spin:
    olw   r10, 0(o7)
    beqz  r10, spin
    nop

    move  r4, r10                ; r4 = flag value (= 0x37)
    call  #0x001
    nop

child:
    addiu r8, r0, 0x37
    osw   r8, 0(o7)              ; flag = 0x37
    addiu r4, r0, 0
    call  #0x001                 ; TaskExit
    nop

timer_handler:
    ; Re-arm timer for next quantum.
    lctrl r5, $5
    addiu r5, r5, 50
    sctrl $6, r5

    ; Re-enable IE in (saved) STATUS so it's restored on ERET.
    lctrl r6, $0
    ori   r6, r6, 0x10
    sctrl $0, r6

    ; Hand the rest of the quantum to the next runnable task. Sets
    ; yield_pending; honored by ERET below.
    call  #0x004                 ; TaskYield

    eret
    nop
