; @description: ReadCycles returns OK and a strictly-increasing count
; @expect-exit: 0

.entry main
.text
main:
    call  #0x301                  ; first sample
    bne   r2, r0, fail            ; status must be OK
    nop
    addu  r16, r3, r0             ; save first cycle reading

    ; Spend a handful of cycles
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0

    call  #0x301                  ; second sample
    bne   r2, r0, fail
    nop

    sltu  r17, r16, r3            ; r17 = (first < second) ? 1 : 0
    beq   r17, r0, fail           ; bail if not strictly increasing
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

fail:
    addiu r4, r0, 1
    call  #0x001
    nop
