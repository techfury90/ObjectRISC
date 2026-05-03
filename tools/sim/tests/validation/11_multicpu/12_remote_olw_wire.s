; @description: Remote OLW now goes through real OBJ_READ_REQ/OBJ_READ_RESP packets. CPU 1 stores 0xCAFEBABE into its service object; CPU 0 reads the upper byte (0xCA = 202) via remote OLBU through O5. The CPU 0's load blocks until CPU 1's "memory controller" (autonomous request handler) services the request and sends a response back across the wire.
; @processors: 2
; @expect-exit: 202

.entry main
.text
main:
    bne   r7, r0, server
    nop

client:                           ; CPU 0
    omov  o9, o5                  ; remote service ref (R+S only — but R is what we need)
wait_loop:
    olbu  r4, 0(o9)               ; remote read via OBJ_READ_REQ; blocks until response
    beq   r4, r0, wait_loop       ; spin until CPU 1 has stored the marker
    nop
    call  #0x001                  ; exit r4
    nop

server:                           ; CPU 1 — store 0xCA at byte 0 of own service
    lui   r2, 0xCAFE
    ori   r2, r2, 0xBABE
    osw   r2, 0(o4)
    addiu r4, r0, 0
    call  #0x001
    nop
