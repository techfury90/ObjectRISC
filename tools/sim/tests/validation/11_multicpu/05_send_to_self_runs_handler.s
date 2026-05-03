; @description: Single CPU: ObjAlloc a service, InstallHandler, SEND to self, main exits, handler dispatches and exits 42
; @expect-exit: 42

.entry main
.text
main:
    omov  o14, o1          ; preserve code object

    ; Allocate a service object: 16 bytes, type 0xBEEF, caps R+W+S+V+C
    addiu r4, r0, 16
    addiu r5, r0, 0xBEEF
    addiu r6, r0, 0x5B     ; R|W|S|V|C = 0x01|0x02|0x08|0x10|0x40
    call  #0x100           ; ObjAlloc -> O1 = ref
    bne   r2, r0, fail
    nop

    omov  o9, o1           ; preserve service ref

    ; InstallHandler: target=O1 (service), handler=O2 (=O14, code), R4=offset
    omov  o2, o14
    la    r5, my_handler
    lui   r6, 0x0001       ; r6 = 0x00010000 (code base)
    subu  r4, r5, r6       ; r4 = offset within code object
    call  #0x200
    bne   r2, r0, fail
    nop

    ; SEND to self
    omov  o1, o9
    onull o2
    onull o3
    onull o4
    send  o1

    ; main TaskExits with code 0 first
    addiu r4, r0, 0
    call  #0x001
    nop

my_handler:
    addiu r4, r0, 42
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop
