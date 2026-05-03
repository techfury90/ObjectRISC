; @description: Canonical two-CPU ping. CPU 0 SENDs to CPU 1's service; CPU 1's handler ConsoleWrites the data section.
; @processors: 2
; @expect-stdout: "Hello from CPU 1!\n"
; @expect-exit: 0

.entry main

.text
main:
    bne   r7, r0, server   ; r7 = my procid; if 1, server
    nop

client:                    ; CPU 0
    omov  o1, o5           ; recipient = CPU 1's service object
    onull o2
    onull o3
    onull o4
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1
    addiu r4, r0, 0
    call  #0x001
    nop

server:                    ; CPU 1
    omov  o15, o3          ; preserve data ref in O15 — handler will find it here
    omov  o9, o1           ; preserve code object
    omov  o1, o4           ; target = my service
    omov  o2, o9           ; handler in my code object
    la    r5, hello_handler
    lui   r6, 0x0001
    subu  r4, r5, r6       ; offset within code
    call  #0x200           ; InstallHandler
    addiu r4, r0, 0
    call  #0x001           ; main TaskExits; snapshot now includes O15=data

hello_handler:
    omov  o1, o15          ; data ref restored from main's pre-exit snapshot
    addiu r4, r0, 0
    addiu r5, r0, 18       ; "Hello from CPU 1!\n" = 18 bytes
    call  #0x320
    addiu r4, r0, 0
    call  #0x001
    nop

.data
    .string "Hello from CPU 1!\n"
