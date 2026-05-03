; @description: If 5 > 3, print "yes"; else "no". 5 > 3, so print "yes".
; @expect-stdout: "yes"
; @expect-exit: 0

.entry main
.text
main:
    addiu r2, r0, 5
    addiu r3, r0, 3
    slt   r5, r3, r2       ; r5 = 1 if 3 < 5
    bne   r5, r0, yes
    nop
    ; "no" branch: offset 3, length 2
    omov  o1, o3
    addiu r4, r0, 3
    addiu r5, r0, 2
    call  #0x320
    j     done
    nop
yes:
    ; "yes" branch: offset 0, length 3
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 3
    call  #0x320
    nop
done:
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "yesno"
