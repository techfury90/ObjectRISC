; @description: InstallHandler succeeds on a local object (own service) and returns OK
; @processors: 2
; @expect-exit: 0

.entry main
.text
main:
    bne   r7, r0, server   ; only CPU 1 runs the install
    nop

cpu0_idle:                 ; CPU 0 just exits
    addiu r4, r0, 0
    call  #0x001
    nop

server:                    ; CPU 1
    omov  o9, o1           ; preserve code object
    omov  o1, o4           ; target = my service
    omov  o2, o9           ; handler code = my code object
    addiu r4, r0, 0        ; offset 0 within code object
    call  #0x200           ; InstallHandler
    move  r4, r2           ; r2 = OK on success
    call  #0x001
    nop
