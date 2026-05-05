; @description: InstallTrapHandler routes arithmetic-overflow to a supervisor handler that runs in supervisor mode and exits with the cause code
;
; Vol VI #0x520 lets supervisor code register handlers without
; firmware-mode VECBASE access. The handler runs in supervisor mode
; in the trapping task's address space; it can call user/sv-callable
; primitives like TaskExit. To prove it really is in supervisor mode
; (not firmware), the handler reads STATUS and asserts the low bits
; equal MODE_SUPERVISOR (= 1) before exiting.
; @expect-exit: 9

.entry main
.text
main:
    ; InstallTrapHandler(R4=cause=0x09, R5=va=handler).
    addiu r4, r0, 9
    la    r5, handler
    call  #0x520

    ; Trigger arithmetic overflow.
    lui   r6, 0x7FFF
    ori   r6, r6, 0xFFFF
    add   r7, r6, r6             ; INT_MAX + INT_MAX → trap

    addiu r4, r0, 1              ; unreachable on success
    call  #0x001
    nop

handler:
    ; Confirm we're in supervisor mode (STATUS bits [1:0] == 1).
    lctrl r8, $0
    andi  r8, r8, 3
    addiu r9, r0, 1
    bne   r8, r9, mode_wrong
    nop

    ; Exit with the cause code (R4 = CAUSE).
    lctrl r4, $1
    call  #0x001
    nop

mode_wrong:
    addiu r4, r0, 99
    call  #0x001
    nop
