; @description: Vol VII §4.3 canonical RPC pattern. Client (CPU 0) attaches a reply queue to its own service, derives a send-only reply cap, SENDs a request to the server (CPU 1), then ReceiveQueuePolls the reply queue. Server (CPU 1) installs a handler that SENDs the reply through the reply cap. Client unblocks and ConsoleWrites "Pong!".
; @processors: 2
; @expect-stdout: "Pong!"
; @expect-exit: 0

.entry main

.text
main:
    bne   r7, r0, server
    nop

client:                           ; CPU 0
    omov  o15, o3                 ; preserve data ref for ConsoleWrite later
    omov  o14, o4                 ; preserve my service (full-cap V) for polling

    ; Attach a reply queue to my own service object
    omov  o1, o4
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop

    ; Derive a send-only cap on my service for the server to reply through
    omov  o1, o14
    addiu r4, r0, 0x08            ; mask = S only
    call  #0x103                  ; ObjDerive
    bne   r2, r0, fail
    nop
    omov  o13, o1                 ; preserve reply cap

    ; SEND request to CPU 1's service, with reply cap in payload
    omov  o1, o5                  ; recipient = CPU 1's service
    omov  o2, o13                 ; payload[0] = reply cap (server sees as O3)
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    ; Poll the reply queue with infinite timeout — blocks until reply arrives
    omov  o1, o14                 ; my service (the queue object)
    addiu r4, r0, -1              ; timeout = 0xFFFFFFFF
    call  #0x204                  ; ReceiveQueuePoll
    bne   r2, r0, fail
    nop

    ; Reply arrived; ConsoleWrite "Pong!" from data section
    omov  o1, o15
    addiu r4, r0, 0
    addiu r5, r0, 5               ; "Pong!" = 5 bytes
    call  #0x320
    addiu r4, r0, 0
    call  #0x001
    nop

server:                           ; CPU 1
    omov  o9, o1                  ; preserve code obj
    omov  o1, o4                  ; install on my service
    omov  o2, o9
    la    r5, server_handler
    lui   r6, 0x0001
    subu  r4, r5, r6
    call  #0x200                  ; InstallHandler
    bne   r2, r0, fail
    nop
    addiu r4, r0, 0
    call  #0x001                  ; CPU 1 sleeps until SEND arrives

server_handler:                   ; runs on CPU 1 when client SENDs
    ; On entry: O3 = client's reply cap (from sender's O2)
    omov  o1, o3                  ; recipient = reply cap
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1                      ; reply (no payload — wakes client)
    addiu r4, r0, 0
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop

.data
    .string "Pong!"
