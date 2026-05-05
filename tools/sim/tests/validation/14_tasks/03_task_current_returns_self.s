; @description: TaskCurrent returns a stable ref to the calling task across two consecutive calls
;
; Two TaskCurrent calls back-to-back must return refs to the same
; descriptor with the same generation; oeq treats those as equal.
; @expect-exit: 1

.entry main
.text
main:
    call  #0x005                 ; TaskCurrent → O1
    omov  o5, o1                 ; stash first call's result

    call  #0x005                 ; TaskCurrent → O1 (second call)

    oeq   r4, o5, o1             ; r4 = 1 if both refs name the same task
    call  #0x001                 ; TaskExit(r4)
    nop
