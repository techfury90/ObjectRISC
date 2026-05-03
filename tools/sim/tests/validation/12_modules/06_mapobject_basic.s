; @description: ObjAlloc + MapObject + read via LW: store 0x42 at offset 0 of new obj, map it as R+W, read via the returned VA
; @expect-exit: 0x42

.entry main
.text
main:
    ; Allocate a 16-byte byte-typed object with R+W+C
    addiu r4, r0, 16
    addiu r5, r0, 0
    addiu r6, r0, 0x43            ; R|W|C
    addiu r7, r0, 0
    call  #0x100                  ; ObjAlloc -> O1
    bne   r2, r0, fail
    nop
    omov  o9, o1                  ; preserve

    ; Write 0x42 at offset 0
    addiu r2, r0, 0x42
    osw   r2, 0(o9)

    ; Map it: prot = R+W, length = 16, hint = 0 -> auto VA
    omov  o1, o9
    addiu r4, r0, 0               ; va_hint
    addiu r5, r0, 0               ; offset within object
    addiu r6, r0, 0x03            ; prot R|W
    addiu r7, r0, 16              ; length
    call  #0x110                  ; MapObject -> R3 = VA
    bne   r2, r0, fail
    nop

    ; Read the word via LW from the returned VA -> should be 0x42
    lw    r4, 0(r3)
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
