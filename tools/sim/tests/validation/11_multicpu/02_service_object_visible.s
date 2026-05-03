; @description: O4 (own service) and O5 (other CPU's service) are non-null in multi-CPU mode.
;   We exit with the sum: 0 + (oisn O4) + (oisn O5) -> should be 0 (both non-null).
; @processors: 2
; @expect-exit: 0

.entry main
.text
main:
    oisn  r2, o4           ; 0 if O4 is non-null
    oisn  r3, o5
    add   r4, r2, r3       ; 0 if both non-null
    call  #0x001
    nop
