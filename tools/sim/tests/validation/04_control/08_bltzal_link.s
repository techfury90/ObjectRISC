; @description: BLTZAL writes RA regardless of branch outcome; here condition false (1 < 0 is false), but RA still set to PC+8
; If RA is non-zero we exit with low byte of RA; otherwise exit 0
; @expect-exit: 12

.entry main
.text
main:
    addiu r2, r0, 1
    bltzal r2, never_taken    ; 1 < 0 is false, but RA still gets PC+8
    nop                       ; delay slot at 0x10008
    ; PC+8 from the BLTZAL = 0x1000C; low byte = 0x0C = 12
    andi  r4, r31, 0xFF
    call  #0x001
    nop
never_taken:
    addiu r4, r0, 0
    call  #0x001
    nop
