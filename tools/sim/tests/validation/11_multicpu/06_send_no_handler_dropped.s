; @description: SEND to a service object with no handler installed: message dropped, no observable effect
; @processors: 2
; @expect-exit: 0
; @expect-stdout: ""

.entry main
.text
main:
    bne   r7, r0, idle     ; only CPU 0 sends; CPU 1 just exits without installing
    nop

cpu0:
    omov  o1, o5           ; recipient = CPU 1's service (no handler installed)
    onull o2
    onull o3
    onull o4
    send  o1
    addiu r4, r0, 0
    call  #0x001
    nop

idle:
    addiu r4, r0, 0
    call  #0x001
    nop
