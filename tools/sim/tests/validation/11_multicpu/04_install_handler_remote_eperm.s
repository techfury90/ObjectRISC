; @description: Trying to install a handler on a remote service object fails with EPERM. Remote refs in O5 carry only R+S, so the V cap requirement fails before the home check (EREMOTE) would. This is the correct security outcome — no remote installs without V.
; @processors: 2
; @expect-exit: 3

.entry main
.text
main:
    omov  o9, o1           ; preserve code object
    omov  o1, o5           ; target = OTHER cpu's service object
    omov  o2, o9           ; handler code (just to satisfy the args)
    addiu r4, r0, 0
    call  #0x200           ; -> R2 = EPERM (no V cap on O5)
    move  r4, r2
    call  #0x001
    nop
