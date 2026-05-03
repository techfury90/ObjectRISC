; 05_pseudo.s — exercise pseudo-instructions: nop, move, b, li (small/large), la.

.entry start

.text
start:
    move  r2, r3            ; addu r2, r3, r0
    li    r4, 5             ; addiu r4, r0, 5
    li    r5, 0xCAFEBABE    ; lui+ori (lo half negative under sign-ext)
    li    r6, 0x10000       ; lui+ori (lo half is zero)
    la    r7, target        ; lui+ori address materialization
target:
    nop
