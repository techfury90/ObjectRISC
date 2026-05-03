; @description: OLBU through a reference whose home is a different CPU routes the read across the crossbar. Both CPUs have a data section "Hello"; CPU 0 reads byte 0 of CPU 1's data via O5... but O5 is a service object, not data. So we test by having the client read a byte of the OTHER CPU's service buffer (which the server first wrote a known byte into).
; @processors: 2
; @expect-exit: 0x77

.entry main
.text
main:
    bne   r7, r0, server   ; r7 = procid
    nop

client:                    ; CPU 0 — reads remote byte after a delay
    ; Wait for server to write its byte, then read it via remote OLBU.
    ; Without proper sync we use a busy-wait: read until non-zero.
    omov  o9, o5           ; remote service ref (R+S only — but we need R for OLBU)
wait_loop:
    olbu  r4, 0(o9)        ; remote read: routes to CPU 1's descriptor table
    beq   r4, r0, wait_loop ; spin until server writes
    nop
    call  #0x001           ; exit r4 = the byte we read
    nop

server:                    ; CPU 1 — writes a known byte to its own service object
    addiu r2, r0, 0x77     ; the marker byte
    osb   r2, 0(o4)        ; store to my service (O4 has full caps)
    addiu r4, r0, 0
    call  #0x001
    nop
