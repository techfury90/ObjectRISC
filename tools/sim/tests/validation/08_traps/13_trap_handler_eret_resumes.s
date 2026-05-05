; @description: trap handler advances EPC past the faulting instruction and ERETs; main resumes
;
; The handler reads EPC, adds 4 to skip the trapping `add`, writes it
; back, and issues ERET. Main resumes at the instruction after the trap
; and exits with 42 to prove control returned. ERET also restores the
; saved mode (still firmware here, since main was firmware too).
; @mode: firmware
; @expect-exit: 42

.entry main
.text
main:
    la    r4, handler
    addiu r4, r4, -0x240
    sctrl $8, r4              ; VECBASE

    lui   r5, 0x7FFF
    ori   r5, r5, 0xFFFF
    add   r6, r5, r5          ; trap → handler → ERET → next instruction

    addiu r4, r0, 42          ; resumed here via ERET
    call  #0x001
    nop

handler:
    lctrl r4, $2              ; EPC of the trapping `add`
    addiu r4, r4, 4           ; skip past it
    sctrl $2, r4
    eret
    nop
