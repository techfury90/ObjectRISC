; @description: bootstrap creates a child task, resumes it, exits; child runs and exits last with 42
;
; Scheduler chain: bootstrap TaskExit finds child runnable, context-
; switches to it; child TaskExit has no successor and bubbles up
; TaskExitSignal(42) which becomes the CPU's exit code.
; @expect-exit: 42

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code ref

    ; ObjAlloc(R4=size, R5=tag, R6=caps) → O1 = stack ref.
    addiu r4, r0, 0x1000         ; 4 KiB stack
    addiu r5, r0, 0x4101         ; TAG_STACK
    addiu r6, r0, 0x43           ; R | W | C
    call  #0x100

    omov  o2, o1                 ; O2 = stack
    omov  o1, o5                 ; O1 = code (restored)

    ; entry offset = child_entry - CODE_VA
    la    r4, child_entry
    lui   r5, 1                  ; r5 = 0x10000
    subu  r4, r4, r5

    addiu r5, r0, 42             ; child's initial R4

    call  #0x000                 ; TaskCreate → O1 = child task ref
    call  #0x002                 ; TaskResume(O1)

    addiu r4, r0, 0
    call  #0x001                 ; bootstrap exits → switches to child
    nop

child_entry:
    ; init_r4 = 42 was placed in R4 by TaskCreate.
    call  #0x001                 ; TaskExit(42), last task → CPU exits 42
    nop
