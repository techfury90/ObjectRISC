; @description: Loadable handler module. Main allocates a service object, allocates a code object, copies handler bytes into it, derives R+X-only, MapObjects it, InstallHandler points at it, SENDs to the service, and the handler dispatched from the loadable module ConsoleWrites.
; @expect-stdout: "From module!"
; @expect-exit: 0

.entry main

.text
main:
    omov  o15, o3                 ; preserve data ref for handler dispatch

    ; Allocate a service object (since we're single-CPU, no boot one exists)
    addiu r4, r0, 64
    addiu r5, r0, 0x4103          ; TAG_SERVICE
    addiu r6, r0, 0x5B            ; R|W|S|V|C
    addiu r7, r0, 0
    call  #0x100                  ; ObjAlloc -> O1 = service ref
    bne   r2, r0, fail
    nop
    omov  o8, o1                  ; preserve service ref

    ; Allocate the loadable code object: 28 bytes, R|W|X|C
    addiu r4, r0, 28
    addiu r5, r0, 0x4100          ; TAG_CODE
    addiu r6, r0, 0x47            ; R|W|X|C
    addiu r7, r0, 0
    call  #0x100
    bne   r2, r0, fail
    nop
    omov  o9, o1                  ; preserve writeable code obj

    ; Copy 28 bytes (7 words) from O3 offset 12..39 to O9 offset 0..27.
    ; (Data layout: bytes 0..11 = "From module!", bytes 12..39 = handler code.)
    olw   r10, 12(o3)
    osw   r10, 0(o9)
    olw   r10, 16(o3)
    osw   r10, 4(o9)
    olw   r10, 20(o3)
    osw   r10, 8(o9)
    olw   r10, 24(o3)
    osw   r10, 12(o9)
    olw   r10, 28(o3)
    osw   r10, 16(o9)
    olw   r10, 32(o3)
    osw   r10, 20(o9)
    olw   r10, 36(o3)
    osw   r10, 24(o9)

    ; ObjDerive to drop W (mask = R|X|C)
    omov  o1, o9
    addiu r4, r0, 0x45
    call  #0x103
    bne   r2, r0, fail
    nop
    omov  o10, o1                 ; preserve derived ref

    ; MapObject it as R+X
    omov  o1, o10
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0x05            ; R|X
    addiu r7, r0, 28
    call  #0x110
    bne   r2, r0, fail
    nop

    ; InstallHandler on the service object, pointing at the loaded module
    omov  o1, o8                  ; target = service
    omov  o2, o10                 ; handler = loaded module
    addiu r4, r0, 0
    call  #0x200
    bne   r2, r0, fail
    nop

    ; SEND to the service
    omov  o1, o8
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    addiu r4, r0, 0
    call  #0x001                  ; main exits; handler dispatches from module

fail:
    addiu r4, r0, 99
    call  #0x001
    nop

.data
    .string "From module!"        ; offset 0..11

    ; Handler code bytes (7 instructions = 28 bytes), hand-encoded per
    ; CONTRACT.md Section 5. Main copies these into a fresh code object
    ; and the handler runs in that loadable module's mapped VA range.
    .word 0xC07C0000   ; omov o1, o15            (data ref preserved by snapshot)
    .word 0x24040000   ; addiu r4, r0, 0         (offset = 0)
    .word 0x2405000C   ; addiu r5, r0, 12        (count = 12)
    .word 0xF4000320   ; call #0x320             (ConsoleWrite)
    .word 0x24040000   ; addiu r4, r0, 0         (exit code)
    .word 0xF4000001   ; call #0x001             (TaskExit)
    .word 0x00000000   ; nop
