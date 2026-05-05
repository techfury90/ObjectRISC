; @description: bootstrap creates two children, yields; A then B run in FIFO order; bootstrap resumes last and reads back B's tag
;
; Queue evolution:
;   start                    runnable=[],          current=bootstrap
;   resume A                 runnable=[A]
;   resume B                 runnable=[A,B]
;   bootstrap yields         current=A,            runnable=[B,bootstrap]
;   A exits                  current=B,            runnable=[bootstrap]
;   B exits                  current=bootstrap,    runnable=[]
;   bootstrap reads scratch[2] = 0x12, exits 0x12 (no successor → TaskExitSignal)
; @expect-exit: 0x12

.entry main
.text
main:
    omov  o5, o1                 ; save bootstrap code

    ; shared scratch (inherited by children via TaskCreate's OPR copy)
    addiu r4, r0, 6
    addiu r5, r0, 0x4102         ; TAG_DATA
    addiu r6, r0, 0x43           ; R|W|C
    call  #0x100
    omov  o7, o1

    ; A's stack
    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x43
    call  #0x100
    omov  o6, o1                 ; tmp

    omov  o1, o5
    omov  o2, o6
    la    r4, child_a
    lui   r9, 1
    subu  r4, r4, r9
    addiu r5, r0, 0
    call  #0x000                 ; TaskCreate A
    call  #0x002                 ; TaskResume A

    ; B's stack
    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x43
    call  #0x100
    omov  o2, o1
    omov  o1, o5

    la    r4, child_b
    lui   r9, 1
    subu  r4, r4, r9
    addiu r5, r0, 0
    call  #0x000                 ; TaskCreate B
    call  #0x002                 ; TaskResume B

    call  #0x004                 ; yield → A runs first

    ; Resumed last. Both children have written their tags.
    olbu  r4, 2(o7)              ; scratch[2] = 0x12 (B's tag)
    call  #0x001                 ; exit 0x12 (last task → TaskExitSignal)
    nop

child_a:
    addiu r8, r0, 0x11
    osb   r8, 1(o7)              ; scratch[1] = 0x11
    addiu r4, r0, 0
    call  #0x001                 ; A exits → scheduler picks B
    nop

child_b:
    addiu r8, r0, 0x12
    osb   r8, 2(o7)              ; scratch[2] = 0x12
    addiu r4, r0, 0
    call  #0x001                 ; B exits → scheduler picks bootstrap
    nop
