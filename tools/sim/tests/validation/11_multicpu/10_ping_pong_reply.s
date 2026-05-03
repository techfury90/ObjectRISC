; @description: Ping-pong. CPU 0 SENDs to CPU 1 with a reply object in O2 (which becomes handler's O3). CPU 1's handler SENDs back. CPU 0's handler runs and prints. Tests bidirectional SEND.
; @processors: 2
; @expect-stdout: "PONG"
; @expect-exit: 0

.entry main

.text
main:
    bne   r7, r0, server
    nop

client:                    ; CPU 0
    ; Install our own reply handler on O4 (my service) so when CPU 1 sends back,
    ; we get the message.
    omov  o15, o3          ; preserve data ref in O15 for our handler
    omov  o14, o1          ; preserve code object
    omov  o1, o4           ; install on my service
    omov  o2, o14          ; handler code = my code
    la    r5, client_handler
    lui   r6, 0x0001
    subu  r4, r5, r6
    call  #0x200           ; InstallHandler
    ; Now SEND to CPU 1's service, passing my service ref as the reply cap
    omov  o1, o5           ; recipient = CPU 1's service
    omov  o2, o4           ; payload[0] = my service ref (reply cap) -> handler O3
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1
    addiu r4, r0, 0
    call  #0x001           ; main exits; reply handler waits

client_handler:            ; runs on CPU 0 when CPU 1 SENDs back
    omov  o1, o15          ; my data ref
    addiu r4, r0, 0        ; offset 0 = "PONG"
    addiu r5, r0, 4
    call  #0x320           ; ConsoleWrite "PONG"
    addiu r4, r0, 0
    call  #0x001
    nop

server:                    ; CPU 1
    omov  o9, o1
    omov  o1, o4
    omov  o2, o9
    la    r5, server_handler
    lui   r6, 0x0001
    subu  r4, r5, r6
    call  #0x200
    addiu r4, r0, 0
    call  #0x001

server_handler:            ; runs on CPU 1 when CPU 0 SENDs
    ; The reply cap arrived in O3 (per the convention: client's O2 -> handler O3).
    omov  o1, o3           ; recipient for our reply = the reply cap
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1               ; SEND back to CPU 0
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "PONG"
