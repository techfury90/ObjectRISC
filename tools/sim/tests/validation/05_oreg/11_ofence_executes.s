; @description: OFENCE assembles, executes, and falls through cleanly (no-op in this simulator)
; @expect-exit: 7

.entry main
.text
main:
    addiu r4, r0, 7
    ofence
    ofence
    ofence
    call  #0x001
    nop
