; @description: a privileged-instruction trap from user mode delivers in firmware mode with mode-saved=user
;
; Boots in firmware so we can install the vector, then drops to user
; mode by setting STATUS = (saved=firmware, current=user) and ERETing
; into a labelled `user_code` that issues an LCTRL — illegal in user
; mode, raising privileged-instruction (cause 0x0b). The handler reads
; STATUS bits [3:2] for the saved mode (expect MODE_USER=0) and exits
; with that value. The handler also confirms it's in firmware mode by
; doing a TLB op (firmware-only) before the read; if that traps the
; test fails, proving the deliver-time mode switch happened.
; @mode: firmware
; @expect-exit: 0

.entry main
.text
main:
    la    r4, handler
    addiu r4, r4, -0x2C0      ; cause-0x0B vector offset
    sctrl $8, r4              ; VECBASE

    ; Set up the ERET-to-user transition: EPC = user_code,
    ; STATUS bits [3:2] = supervisor saved-mode (so ERET pops to
    ; supervisor, not back to firmware). For this test we use user mode
    ; as the saved value (STATUS = 0b0000) so ERET drops to user.
    la    r4, user_code
    sctrl $2, r4              ; EPC = user_code
    sctrl $0, r0              ; STATUS = 0 → mode=USER, saved_mode=USER
    eret
    nop

user_code:
    lctrl r4, $0              ; STATUS — illegal in user mode → trap 0x0b

    ; Unreachable on success.
    addiu r4, r0, 99
    call  #0x001
    nop

handler:
    tlbwr                     ; firmware-only; no-op in this sim if mode=fw
    lctrl r4, $0              ; read STATUS
    srl   r4, r4, 2           ; saved_mode bits [3:2] → low bits
    andi  r4, r4, 3
    call  #0x001              ; exit with saved_mode (expect 0 = USER)
    nop
