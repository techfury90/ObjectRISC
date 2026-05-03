; master.s — boot master that drives one linkboot CPU.
;
; Procedure:
;   1. Save our data ref (init_cpu put it in O3) — we need it after
;      the queue poll overlays O3 with the announce payload.
;   2. Attach a receive queue to our self-service.
;   3. Poll the queue infinitely. When the announce arrives:
;        O2 = loader's R+S self-ref
;        R3 = loader's PROCID
;   4. Build the boot SEND with code source = our data segment (the
;      module image lives at offset 0..31), data ref = same object
;      (the message lives at offset 32..39).
;   5. TaskExit. The loaded module on the receiver TaskExits with 0.

.entry main

.set MODULE_BYTES, 40

.text

main:
    ; Save our data ref before the queue poll overwrites O3.
    omov  o7, o3                       ; o7 = our data segment ref

    ; Attach a queue to our self-service.
    omov  o1, o4
    addiu r4, r0, 4
    call  #0x203                       ; ReceiveQueueAttach
    bne   r2, r0, fail
    nop

    ; Poll for the announce.
    omov  o1, o4
    addiu r4, r0, -1                   ; infinite
    call  #0x204                       ; ReceiveQueuePoll
    bne   r2, r0, fail
    nop
    ; Now: O2 = loader's R+S self-ref;
    ;      R3 = loader's PROCID (informational).
    omov  o6, o2                       ; o6 = loader's self-ref

    ; Build the boot SEND.
    omov  o1, o6                       ; recipient = loader
    omov  o2, o7                       ; code source = our data segment
    omov  o3, o7                       ; module's data ref = same object
    onull o4
    addiu r4, r0, MODULE_BYTES
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    addiu r4, r0, 0
    call  #0x001                       ; TaskExit
    nop

fail:
    addiu r4, r0, 1
    call  #0x001
    nop


.data
; Module image — the bytes the loader copies, MapObjects R|X, and JRs to.
; 32 B of code followed by 8 B of inline message; total 40 B = MODULE_BYTES.
; The module reads its message via O1 (= its own loaded code ref, set by
; the loader before JR). Hand-encoded per CONTRACT §5.
;
;   ; O1 is already the loaded code ref on entry; no setup needed.
;   nop
;   addiu r4, r0, 32          ; offset = 32 (past the module's own code)
;   addiu r5, r0, 8           ; count  = 8 ("Booted!\n")
;   call  #0x320              ; ConsoleWrite (reads from O1, locally)
;   addiu r4, r0, 0           ; exit code 0
;   call  #0x001              ; TaskExit
;   nop                       ; CALL has no delay slot, but gives a clean fall-through
;   nop                       ; pad to 32 bytes
    .word 0x00000000                   ; nop
    .word 0x24040020                   ; addiu r4, r0, 32
    .word 0x24050008                   ; addiu r5, r0, 8
    .word 0xF4000320                   ; call  #0x320
    .word 0x24040000                   ; addiu r4, r0, 0
    .word 0xF4000001                   ; call  #0x001
    .word 0x00000000                   ; nop
    .word 0x00000000                   ; nop (pad)

; The message lives at offset 32 of THIS .data segment. The master sends
; MODULE_BYTES (40) so both code and message get copied to the loader.
    .ascii "Booted!\n"
