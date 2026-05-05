; @description: arithmetic-overflow delivers to firmware vector; handler reads CAUSE and exits with 9
;
; Bias VECBASE so the cause-9 vector slot lands directly on `handler`
; (avoids laying out a full 16-vector table for a single-cause test).
; @mode: firmware
; @expect-exit: 9

.entry main
.text
main:
    la    r4, handler
    addiu r4, r4, -0x240
    sctrl $8, r4              ; VECBASE = handler - 0x240

    lui   r5, 0x7FFF
    ori   r5, r5, 0xFFFF      ; r5 = INT_MAX
    add   r6, r5, r5          ; INT_MAX+INT_MAX → arithmetic-overflow → trap

    addiu r4, r0, 1           ; trap dispatched? if we reach here, fail
    call  #0x001
    nop

handler:
    lctrl r4, $1              ; CAUSE
    call  #0x001              ; exit with cause as code (expect 9)
    nop
