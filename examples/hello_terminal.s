; hello_terminal.s — write "Hello, world!\n" to a remote graphics terminal
;
; Multi-process demo. Run via tools/oriscrun:
;
;     python3 tools/oriscrun \
;         --terminal pid=16 \
;         --cpu pid=0:program=examples/hello_terminal.orx,service=16=1@9
;
; The terminal expects a SEND with this layout:
;     payload OR slot 1 (sender's O2)  = ref to source buffer (R cap)
;     payload OR slot 2 (sender's O3)  = reply capability (S cap; optional)
;     payload int 0     (R4)           = byte offset within source
;     payload int 1     (R5)           = byte count
; The terminal then issues OBJ_READ_REQ packets back to the source's
; pid (us) to fetch and render the bytes. After rendering it SENDs a
; header-only ack through the reply cap, which unblocks our
; ReceiveQueuePoll. Then we TaskExit; the launcher tears down the rest.
;
; Wire trace (visible at simorisc --trace):
;     CPU0 -> term : SEND_DELIVER  (we ship: data ref, reply cap, off, len)
;     term -> CPU0 : OBJ_READ_REQ  (terminal pulls our bytes — we're blocked
;                                   on poll, but the autonomous memory
;                                   controller still services this)
;     CPU0 -> term : OBJ_READ_RESP (we return the bytes)
;     [terminal renders]
;     term -> CPU0 : SEND_DELIVER  (ack)
;     [we unblock and exit]

.entry main

.text
main:
    omov  o14, o4                 ; preserve own service for poll
    omov  o15, o3                 ; preserve data ref for SEND payload

    ; Attach a reply queue to my own service object
    omov  o1, o14
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop

    ; Derive a send-only reply cap from my service
    omov  o1, o14
    addiu r4, r0, 0x08            ; mask = S only
    call  #0x103                  ; ObjDerive
    bne   r2, r0, fail
    nop
    omov  o13, o1                 ; preserve reply cap

    ; SEND request to terminal
    omov  o1, o5                  ; recipient = terminal console
    omov  o2, o15                 ; payload[0]: data ref
    omov  o3, o13                 ; payload[1]: reply cap
    onull o4
    addiu r4, r0, 0               ; offset = 0
    addiu r5, r0, 14              ; "Hello, world!\n" length
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    ; Block until the terminal acks via our reply queue
    omov  o1, o14
    addiu r4, r0, -1              ; timeout = 0xFFFFFFFF (infinite)
    call  #0x204                  ; ReceiveQueuePoll
    bne   r2, r0, fail
    nop

    addiu r4, r0, 0
    call  #0x001                  ; TaskExit; launcher tears down the rest
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop

.data
    .string "Hello, world!\n"
