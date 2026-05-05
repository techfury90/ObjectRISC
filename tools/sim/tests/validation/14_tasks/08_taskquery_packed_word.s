; @description: TaskQuery (#0x008) returns the packed state word for an EXITED child — state in low byte, processor in next, exit code in upper 16 bits
;
; Bootstrap creates a child that immediately exits with R4 = 0x42.
; TaskExit on the child (last task → TaskExitSignal? no — bootstrap
; is still around). Bootstrap then queries the child and asserts the
; packed word: state = TASK_STATE_EXITED (5), processor = pid (0),
; exit code = 0x42. Packed: 5 | (0 << 8) | (0x42 << 16) = 0x420005.
; @expect-exit: 0x05

.entry main
.text
main:
    omov  o5, o1                  ; save bootstrap code

    ; Allocate stack for child
    addiu r4, r0, 0x800
    addiu r5, r0, 0x4101
    addiu r6, r0, 0x43
    call  #0x100
    omov  o2, o1

    ; TaskCreate(O1=code, O2=stack, R4=child entry offset, R5=0x42)
    omov  o1, o5
    la    r4, child
    lui   r6, 1
    subu  r4, r4, r6
    addiu r5, r0, 0x42
    call  #0x000                  ; → O1 = task ref
    omov  o6, o1                  ; save task ref
    call  #0x002                  ; TaskResume

    ; Yield so child runs to completion (it just exits 0x42).
    call  #0x004

    ; Query the (now EXITED) child.
    omov  o1, o6
    call  #0x008                  ; TaskQuery → R3 = packed
    move  r4, r3

    ; Assert state byte (low 8 bits) = TASK_STATE_EXITED = 5.
    andi  r5, r4, 0xff
    addiu r6, r0, 5
    bne   r5, r6, fail
    nop

    ; Assert exit code (bits 23:16) = 0x42.
    srl   r5, r4, 16
    andi  r5, r5, 0xff
    addiu r6, r0, 0x42
    bne   r5, r6, fail
    nop

    ; Pass — exit with state byte (= 5) so we can verify via @expect-exit.
    andi  r4, r4, 0xff
    call  #0x001
    nop

child:
    ; init_r4 = 0x42 from TaskCreate; just exit.
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
