; @description: Link-boot. CPU 0 (loader) sends its data ref to CPU 1; the receiver's bootloader handler reads the module bytes across the crossbar, allocates a local code object, derives R+X+C, MapObjects it, restores O3 to CPU 1's local data, and JRs into the loaded module. The module ConsoleWrites "Booted!\n" and TaskExits.
; @processors: 2
; @expect-stdout: "Booted!\n"
; @expect-exit: 0

.entry main

.text
main:
    bne   r7, r0, boot_main      ; r7 = procid; if 1, receiver
    nop
    ; fall through to loader_main on CPU 0

loader_main:                     ; CPU 0 — sends the module reference
    omov  o1, o5                 ; recipient = CPU 1's service object
    omov  o2, o3                 ; payload[0] = my data ref (with module embedded)
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1
    addiu r4, r0, 0
    call  #0x001                 ; loader exits cleanly
    nop

boot_main:                       ; CPU 1 — install boot_handler then sleep
    omov  o15, o3                ; preserve CPU 1's local data ref for the module
    omov  o9, o1                 ; preserve code object
    omov  o1, o4                 ; install on my service
    omov  o2, o9                 ; handler in my code object
    la    r5, boot_handler
    lui   r6, 0x0001
    subu  r4, r5, r6             ; offset within code
    call  #0x200                 ; InstallHandler
    bne   r2, r0, boot_fail
    nop
    addiu r4, r0, 0
    call  #0x001                 ; main exits; CPU 1 sleeps until SEND arrives

boot_fail:
    addiu r4, r0, 99
    call  #0x001
    nop

boot_handler:                    ; runs on CPU 1 when CPU 0's SEND arrives
    ; Per the dispatch convention:
    ;   O1 = self (CPU 1's service, full caps; firmware-installed)
    ;   O3 = sender's O2 = CPU 0's data ref (with module embedded at offset 8)
    ;   O15 = CPU 1's local data (preserved from main's TaskExit snapshot)
    omov  o9, o3                 ; preserve remote source ref

    ; Allocate a local 28-byte code object
    addiu r4, r0, 28
    addiu r5, r0, 0x4100         ; TAG_CODE
    addiu r6, r0, 0x47           ; R|W|X|C
    addiu r7, r0, 0
    call  #0x100                 ; ObjAlloc
    bne   r2, r0, h_fail
    nop
    omov  o10, o1                ; preserve writeable code obj

    ; Copy 7 words (28 bytes) from REMOTE O9 offset 8..35 to LOCAL O10 offset 0..27.
    ; Each OLW dispatches across the crossbar to CPU 0; each OSW writes locally.
    olw   r11, 8(o9)
    osw   r11, 0(o10)
    olw   r11, 12(o9)
    osw   r11, 4(o10)
    olw   r11, 16(o9)
    osw   r11, 8(o10)
    olw   r11, 20(o9)
    osw   r11, 12(o10)
    olw   r11, 24(o9)
    osw   r11, 16(o10)
    olw   r11, 28(o9)
    osw   r11, 20(o10)
    olw   r11, 32(o9)
    osw   r11, 24(o10)

    ; Drop the W cap
    omov  o1, o10
    addiu r4, r0, 0x45           ; R|X|C
    call  #0x103                 ; ObjDerive
    bne   r2, r0, h_fail
    nop
    omov  o11, o1                ; derived ref

    ; MapObject as R+X
    omov  o1, o11
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0x05           ; R|X
    addiu r7, r0, 28
    call  #0x110                 ; -> R3 = mapped VA
    bne   r2, r0, h_fail
    nop

    ; Restore O3 to CPU 1's local data so the loaded module can use it
    omov  o3, o15

    ; JR into the loaded module — control transfers, no return.
    jr    r3
    nop                          ; delay slot

h_fail:
    addiu r4, r0, 99
    call  #0x001
    nop

.data
    .string "Booted!\n"          ; offset 0..7 — "B-o-o-t-e-d-!-\n" = 8 bytes

    ; Module code (7 instructions = 28 bytes), hand-encoded per CONTRACT
    ; Section 5. Loader sends this region across the crossbar via the data
    ; ref in O3; receiver reads each word remotely with OLW, copies into
    ; a fresh local code object, MapObjects R+X, and JRs in.
    ;
    ; Module body:
    ;   omov  o1, o3            ; data ref (set by bootloader before JR)
    ;   addiu r4, r0, 0         ; ConsoleWrite offset = 0
    ;   addiu r5, r0, 8         ; ConsoleWrite count = 8
    ;   call  #0x320            ; ConsoleWrite
    ;   addiu r4, r0, 0         ; exit code
    ;   call  #0x001            ; TaskExit
    ;   nop
    .word 0xC04C0000
    .word 0x24040000
    .word 0x24050008
    .word 0xF4000320
    .word 0x24040000
    .word 0xF4000001
    .word 0x00000000
