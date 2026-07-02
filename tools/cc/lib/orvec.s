; orvec.s — growable capability-slot array (see orvec.h). Bare asm, like
; obj_or.s and for the same reason: these ops take `__or` params, and
; orvec_push loops over them, which drags in pcc-orisc's per-frame OBJSTORE
; prologue — and that prologue miscompiles here (a caller's `&local` resolves
; to the OBJSTORE-frame size rather than a stack address, and the C copy
; loop's index overruns the source store). Hand asm sidesteps the OBJSTORE
; frame entirely.
;
; An orvec IS its backing store: an ObjAllocStore object of 8-byte reference
; slots. Capacity is intrinsic (OLEN / 8); the caller tracks the length. Every
; op inlines its object primitives (OLEN, ObjAllocStore #0x106, orefldx/orefstx,
; ObjFree #0x101) so there are NO nested jal calls — only firmware CALLs, which
; touch only O1..O4 and R2..R6. The working set therefore rides O5..O7 and
; R8..R11, which a firmware CALL preserves, so no stack frame is needed.
;
; Backing-store type tag 0x4200 (the document/widget object-tag range) and
; caps 0x53 (R|W|V|C) are inlined below to match orvec_new.

.text

; int orvec_cap(void *__or store)      store=O1 -> R2 = slot count
orvec_cap:
    olen r2, o1
    srl  r2, r2, 3            ; bytes / 8 (one 8-byte slot each)
    jr   r31
    nop

; void *__or orvec_new(int cap)        cap=R4 -> O1 = store (null on failure)
orvec_new:
    slti r2, r4, 1           ; cap < 1 ?
    beqz r2, orvn_ok
    nop
    addiu r4, r0, 1          ; clamp to at least 1 slot
orvn_ok:
    sll  r4, r4, 3           ; bytes = cap * 8
    addiu r5, r0, 0x4200     ; ORVEC_TAG
    addiu r6, r0, 0x53       ; R|W|V|C
    call #0x106              ; ObjAllocStore -> O1, R2
    nop
    jr   r31
    nop

; void orvec_free(void *__or store)    store=O1  (frees the store, NOT elements)
orvec_free:
    call #0x101              ; ObjFree (reads O1)
    nop
    jr   r31
    nop

; void *__or orvec_push(void *__or store, void *__or elem, int len)
;   store=O1, elem=O2, len=R4  ->  O1 = store (or the grown store); null on
;   allocation failure (the original store is left intact). Places elem at
;   index `len`, first growing to twice the capacity if len == cap.
orvec_push:
    olen  r8, o1            ; r8 = byte length of store
    srl   r8, r8, 3         ; r8 = cap (slots)
    sltu  r9, r4, r8        ; r9 = (len < cap) ?
    bnez  r9, orvp_place    ; room already -> place directly
    nop
    ; --- grow: bigger = ObjAllocStore(cap * 2 slots) ---
    omov  o5, o1           ; save store  (O5 survives the firmware CALL)
    omov  o6, o2           ; save elem
    addu  r10, r4, r0      ; save len    (R10 survives the CALL)
    sll   r4, r8, 4        ; bytes = cap * 2 * 8 = cap * 16
    addiu r5, r0, 0x4200
    addiu r6, r0, 0x53
    call #0x106            ; ObjAllocStore -> O1 = bigger
    nop
    oisn  r2, o1           ; bigger null (alloc failed)?
    bnez  r2, orvp_fail
    nop
    omov  o7, o1          ; o7 = bigger
    addu  r9, r0, r0      ; i = 0
orvp_copy:
    sltu  r11, r9, r10    ; i < len ?
    beqz  r11, orvp_copied
    nop
    orefldx o1, r9(o5)    ; o1 = store[i]
    nop
    orefstx o1, r9(o7)    ; bigger[i] = o1
    addiu r9, r9, 1       ; i++
    j     orvp_copy
    nop
orvp_copied:
    omov  o1, o5         ; free the old store
    call #0x101          ; ObjFree
    nop
    omov  o1, o7         ; result / place-target = bigger
    omov  o2, o6         ; restore elem
    addu  r4, r10, r0    ; restore len as the placement index
orvp_place:
    orefstx o2, r4(o1)   ; store[len] = elem
    jr    r31
    nop
orvp_fail:
    onull o1             ; return null (store, held in O5, is untouched)
    jr    r31
    nop
