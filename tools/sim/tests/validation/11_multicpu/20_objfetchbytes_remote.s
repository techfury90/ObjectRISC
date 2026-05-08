; @description: Phase 45e. ObjFetchBytes across CPUs — CPU 1 writes a marker byte (0x77) into its own service object via OSB through O4, then SENDs an R+S sub-cap of that service to CPU 0. CPU 0's handler ObjFetchBytes 1 byte from the (remote-home) source into a freshly-allocated local destination, then OLBUs it back. Wire path: ObjFetchBytes builds an OBJ_READ_REQ for the byte, blocks on the response, copies the payload into the local destination, advances PC. Exits with the recovered byte.
; @processors: 2
; @max-cycles: 50000
; @expect-exit: 0x77

.entry main
.text
main:
    bne   r7, r0, server
    nop

;========================================================================
; CPU 0 (client) — installs a handler that ObjFetchBytes from the
; (remote) source ref delivered in O3 of the SEND.
;========================================================================
client:
    omov  o15, o3                ; save data ref (handler may need it)
    omov  o14, o1                ; save bootstrap code ref

    omov  o1, o4                 ; install handler on my own service
    omov  o2, o14
    la    r5, client_handler
    lui   r6, 1
    subu  r4, r5, r6
    call  #0x200                 ; InstallHandler
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

client_handler:
    omov  o5, o3                 ; preserve remote source ref

    ; Allocate a 16-byte TAG_DATA local destination, R|W caps.
    addiu r4, r0, 16
    addiu r5, r0, 0x4102
    addiu r6, r0, 0x53
    call  #0x100                 ; ObjAlloc → O1
    nop
    omov  o6, o1                 ; save dest ref

    ; ObjFetchBytes(O1=src=remote, O2=dst=local, R4=0, R5=0, R6=1).
    omov  o1, o5
    omov  o2, o6
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 1
    call  #0x108                 ; ObjFetchBytes — remote path
    nop
    bne   r2, r0, fail_status    ; expect ERR_OK
    nop

    olbu  r4, 0(o6)              ; pull the byte back
    call  #0x001
    nop

fail_status:
    addu  r4, r2, r0
    call  #0x001
    nop

;========================================================================
; CPU 1 (server) — write 0x77 into byte 0 of its service object, then
; SEND an R+S sub-cap of that service to CPU 0. The service object lives
; on CPU 1; the sub-cap lets CPU 0 issue OBJ_READ_REQ via ObjFetchBytes.
;========================================================================
server:
    addiu r2, r0, 0x77           ; the marker byte
    osb   r2, 0(o4)              ; store to my own service (full caps)

    ; ObjDerive O4 down to R-only sub-cap for sending.
    omov  o1, o4
    addiu r4, r0, 0x01           ; CAP_R only
    call  #0x103                 ; ObjDerive → O1 = sub-cap
    nop
    omov  o6, o1                 ; save the derived sub-cap

    ; SEND to CPU 0's service (O5 in 2-proc mode).
    omov  o1, o5                 ; recipient = CPU 0's service
    omov  o2, o6                 ; payload OR0 = remote source sub-cap
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    addiu r4, r0, 0
    call  #0x001
    nop
