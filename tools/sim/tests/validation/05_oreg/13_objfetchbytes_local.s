; @description: Phase 45e. ObjFetchBytes (#0x108) local case — copy 4 bytes from the data ref O3 (where we've placed the marker "ABCD" at .data offset 0) into a freshly-allocated TAG_DATA destination, then read the first byte back via OLBU through the destination ref. Exits with that byte (0x41 = 'A').
; @expect-exit: 0x41

.entry main
.text
main:
    omov  o14, o3                ; preserve data ref (.data: "ABCD") for later

    ; Allocate a 16-byte TAG_DATA destination, R|W|V|C caps.
    addiu r4, r0, 16
    addiu r5, r0, 0x4102         ; TAG_DATA
    addiu r6, r0, 0x53           ; CAP_R|CAP_W|CAP_V|CAP_C
    call  #0x100                 ; ObjAlloc → O1
    nop
    omov  o15, o1                ; save the dest ref in O15

    ; ObjFetchBytes(O1=src, O2=dst, R4=src_off, R5=dst_off, R6=count).
    omov  o1, o14                ; source = data section
    omov  o2, o15                ; destination
    addiu r4, r0, 0              ; src_off = 0
    addiu r5, r0, 0              ; dst_off = 0
    addiu r6, r0, 4              ; count = 4
    call  #0x108                 ; ObjFetchBytes
    nop
    bne   r2, r0, fail_status    ; expect ERR_OK
    nop

    ; Read the first byte via OLBU through O15 (the destination).
    olbu  r4, 0(o15)
    call  #0x001                 ; exit with that byte (expect 'A' = 0x41)
    nop

fail_status:
    addu  r4, r2, r0             ; surface the error code on failure
    call  #0x001
    nop

.data
    .string "ABCD"
