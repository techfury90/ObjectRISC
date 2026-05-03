; @description: SEND payload (R4..R7) reaches the handler intact. Client sends 'X', 'Y', 'Z', '!' as R4..R7. Handler writes them to stdout via its service buffer.
; @processors: 2
; @expect-stdout: "XYZ!"
; @expect-exit: 0

.entry main
.text
main:
    bne   r7, r0, server
    nop

client:                    ; CPU 0
    omov  o1, o5           ; recipient = CPU 1's service
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0x58     ; 'X'
    addiu r5, r0, 0x59     ; 'Y'
    addiu r6, r0, 0x5A     ; 'Z'
    addiu r7, r0, 0x21     ; '!'
    send  o1
    addiu r4, r0, 0
    call  #0x001
    nop

server:                    ; CPU 1
    omov  o9, o1           ; preserve code
    omov  o1, o4           ; target = my service
    omov  o2, o9
    la    r5, server_handler
    lui   r6, 0x0001
    subu  r4, r5, r6
    call  #0x200           ; InstallHandler
    addiu r4, r0, 0
    call  #0x001

server_handler:
    ; Store payload bytes into self (O1, full caps) at offsets 0..3
    osb   r4, 0(o1)
    osb   r5, 1(o1)
    osb   r6, 2(o1)
    osb   r7, 3(o1)
    ; ConsoleWrite from self at offset 0 length 4
    addiu r4, r0, 0
    addiu r5, r0, 4
    call  #0x320
    addiu r4, r0, 0
    call  #0x001
    nop
