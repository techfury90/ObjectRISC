; 07_expressions.s — exercise .set constants, label arithmetic, and
; expression operands across instructions and data directives.

.set ANSWER, 42
.set TWO, 2
.set FOO, ANSWER + TWO          ; constants can refer to other constants

.entry main

.text
main:
    addiu r4, r0, ANSWER         ; .set used as immediate
    addiu r5, r0, FOO            ; chained .set
    addiu r6, r0, ANSWER - TWO   ; expression in immediate
    li    r8, two_words          ; li with label expression (forces lui+ori)
    li    r9, msg_end - msg      ; length via label arithmetic (small const)
    lw    r10, msg_off(r0)       ; .set in load offset
    nop

.data
msg:
    .byte 'h', 'i', '!'
msg_end:
    .word msg                    ; symbolic .word — pass-2 fixup
    .word msg_end - msg          ; label arithmetic in .word
    .word ANSWER                 ; constant in .word
two_words:
    .half 1, 2, 3, 4

.set msg_off, 4                  ; forward-declared constant works too
