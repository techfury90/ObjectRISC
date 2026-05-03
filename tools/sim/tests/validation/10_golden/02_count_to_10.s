; @description: Loop counting r4 from 0 up to 10 by addition; exit 10
; @expect-exit: 10

.entry main
.text
main:
    addiu r4, r0, 0        ; counter
    addiu r3, r0, 10       ; limit
loop:
    beq   r4, r3, done
    nop
    addiu r4, r4, 1
    j     loop
    nop
done:
    call  #0x001
    nop
