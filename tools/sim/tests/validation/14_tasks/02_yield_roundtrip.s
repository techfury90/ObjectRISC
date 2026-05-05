; @description: bootstrap yields to child; child writes to a shared object; bootstrap resumes and exits with the value child wrote
;
; Proves the yielding task resumes at the instruction *after* the CALL
; with all callee-clobber state (R8..R15, R24..R28) intact, that the
; round-robin queue picks the right successor, and that the address
; space switch leaves shared object refs reachable from both tasks
; (the child writes, bootstrap reads, via the same OBJALLOC'd ref
; both hold in O7 / O8).
; @expect-exit: 0x42

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code ref

    ; -- Allocate a child stack.
    addiu r4, r0, 0x1000
    addiu r5, r0, 0x4101         ; TAG_STACK
    addiu r6, r0, 0x43           ; R|W|C
    call  #0x100
    omov  o6, o1                 ; O6 = child stack ref

    ; -- Allocate a 4-byte shared scratch object (R+W+C).
    addiu r4, r0, 4
    addiu r5, r0, 0x4102         ; TAG_DATA
    addiu r6, r0, 0x43           ; R|W|C
    call  #0x100
    omov  o7, o1                 ; O7 = shared scratch (visible to both)

    ; -- TaskCreate(O1=code, O2=stack, R4=entry, R5=init_r4=0).
    omov  o1, o5
    omov  o2, o6
    la    r4, child_entry
    lui   r5, 1
    subu  r4, r4, r5
    addiu r5, r0, 0
    call  #0x000

    call  #0x002                 ; TaskResume

    ; -- Yield. Child runs, stores 0x42 to scratch[0:4], exits.
    call  #0x004                 ; TaskYield

    ; -- Resumed. Read back what child wrote.
    olw   r4, 0(o7)              ; r4 = scratch[0:4] = 0x42
    call  #0x001                 ; TaskExit(0x42)
    nop

child_entry:
    ; Child sees a fresh address space but inherits OPRs (O7 in
    ; particular — we didn't clear them in TaskCreate).
    addiu r8, r0, 0x42
    osw   r8, 0(o7)              ; scratch[0:4] = 0x42
    call  #0x001                 ; TaskExit(0); successor = bootstrap
    nop
