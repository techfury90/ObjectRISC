; 03_branches.s — forward and backward branches and jumps.

.entry start
.text
start:
    addiu r4, r0, 0
loop:
    addiu r4, r4, 1
    bne   r4, r5, loop      ; backward branch
    nop                     ; delay slot
    b     done              ; forward branch (pseudo: beq r0, r0, done)
    nop
    addiu r4, r0, 99        ; skipped
done:
    nop
