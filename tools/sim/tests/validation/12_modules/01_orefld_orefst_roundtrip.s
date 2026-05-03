; @description: ObjAllocStore creates an OR-typed object; OREFST a ref to it, OREFLD it back, OEQ confirms equal
; @expect-exit: 1

.entry main
.text
main:
    ; Allocate an OR-typed object: 8 bytes (one OR slot), R+W+C
    addiu r4, r0, 8
    addiu r5, r0, 0
    addiu r6, r0, 0x43            ; R|W|C
    call  #0x102                  ; ObjAllocStore -> O1 = OR-storage ref
    bne   r2, r0, fail
    nop
    omov  o9, o1                  ; preserve OR-storage ref

    ; Store the boot code-object ref (O1 is now the storage; recover code from boot
    ; by snapshotting before we overwrote — we kept O14 below)
    ; Actually let's preserve a known ref before allocating: redo with order reversed.
    ; (We'll just store O3 — the data ref — since it's untouched by ObjAllocStore.)
    orefst o3, 0(o9)              ; store data ref into OR-storage at offset 0

    orefld o5, 0(o9)              ; load it back into O5

    oeq   r4, o5, o3              ; r4 = 1 if equal (incl. caps)
    call  #0x001
    nop

fail:
    addiu r4, r0, 99
    call  #0x001
    nop

.data
    .string "x"
