; orbuf.s — growable BYTE buffer (see orbuf.h). The byte-collection sibling of
; orvec (which holds capability slots): orbuf holds raw bytes — a document
; Run's text, a serialisation scratch, and the backing byte-log that
; freeze-on-scroll-away serialises frozen elements into.
;
; Bare asm, like orvec.s / obj_or.s and for the same reason: the ops take
; `__or` params and orbuf_append loops/grows over them, which would drag in
; pcc-orisc's per-frame OBJSTORE prologue — and that miscompiles int
; locals/params in an `__or` function (see the pcc-or-frame-int-param note).
;
; An orbuf IS its backing store: a byte-typed ObjAlloc object. Capacity is
; intrinsic (OLEN); the caller tracks the used length. Bytes move in bulk via
; ObjFetchBytes (#0x108: O1=src, O2=dst, R4=src_off, R5=dst_off, R6=count),
; object-to-object — so append copies a span from a source object into the
; buffer, and read copies a span back out into a destination object. Every op
; inlines its primitive so there are no nested jal calls; the working set rides
; O5..O7 and R8..R13, which a firmware CALL preserves (it touches only O1..O4
; and R2..R6), so no stack frame is needed.
;
; Backing-store tag 0x4201 (document/widget range, distinct from orvec's
; 0x4200) and caps 0x53 (R|W|V|C) are inlined to match orbuf_new.

.text

; int orbuf_cap(void *__or buf)        buf=O1 -> R2 = capacity in bytes
orbuf_cap:
    olen r2, o1
    jr   r31
    nop

; void *__or orbuf_new(int cap)        cap=R4 -> O1 = buf (null on failure)
orbuf_new:
    slti r2, r4, 1           ; cap < 1 ?
    beqz r2, obn_ok
    nop
    addiu r4, r0, 1          ; clamp to at least 1 byte
obn_ok:
    addiu r5, r0, 0x4201     ; ORBUF_TAG
    addiu r6, r0, 0x53       ; R|W|V|C
    call #0x100              ; ObjAlloc -> O1, R2
    nop
    jr   r31
    nop

; void orbuf_free(void *__or buf)      buf=O1  (frees the buffer)
orbuf_free:
    call #0x101              ; ObjFree (reads O1)
    nop
    jr   r31
    nop

; void orbuf_read(void *__or buf, int off, void *__or dst, int dst_off, int n)
;   buf=O1, off=R4, dst=O2, dst_off=R5, n=R6. Copy n bytes from buf[off] into
;   dst[dst_off]. This maps 1:1 onto ObjFetchBytes (src=O1, dst=O2, R4=src_off,
;   R5=dst_off, R6=count) — no register juggling needed.
orbuf_read:
    call #0x108              ; ObjFetchBytes
    nop
    jr   r31
    nop

; void orbuf_write(void *__or buf, int off, void *__or src, int src_off, int n)
;   buf=O1, off=R4, src=O2, src_off=R5, n=R6. Copy n bytes from src[src_off]
;   into buf[off] — a random-offset WRITE (the inverse of orbuf_read), for
;   updating an existing span in place (e.g. a freeze locator entry). Here buf
;   is the DESTINATION, so ObjFetchBytes wants O1=src, O2=buf, R4=src_off,
;   R5=off; swap the object regs and the two offsets.
orbuf_write:
    omov o5, o1             ; save buf (dst)
    omov o1, o2             ; O1 = src
    omov o2, o5             ; O2 = buf (dst)
    addu r8, r4, r0         ; r8 = off (dst offset)
    addu r4, r5, r0         ; R4 = src_off
    addu r5, r8, r0         ; R5 = off
    call #0x108             ; ObjFetchBytes
    nop
    jr   r31
    nop

; void *__or orbuf_append(void *__or buf, int len, void *__or src,
;                         int src_off, int n)
;   buf=O1, len=R4, src=O2, src_off=R5, n=R6  ->  O1 = buf (maybe grown); null
;   on allocation failure (the original buf is left intact). Copies n bytes
;   from src[src_off] to buf[len], growing buf first (to max(2*cap, len+n)) if
;   it would overflow. The caller tracks the length: add n after a success.
orbuf_append:
    ; Stash all incoming args in registers a firmware CALL preserves.
    omov  o5, o1           ; o5 = buf
    omov  o6, o2           ; o6 = src
    addu  r8, r4, r0       ; r8 = len
    addu  r9, r5, r0       ; r9 = src_off
    addu  r10, r6, r0      ; r10 = n
    olen  r11, o5          ; r11 = cap
    addu  r12, r8, r10     ; r12 = need = len + n
    sltu  r2, r11, r12     ; cap < need ?
    beqz  r2, oba_append
    nop
    ; --- grow: newcap = 2*cap, or need if that is larger ---
    sll   r13, r11, 1      ; r13 = 2 * cap
    sltu  r2, r13, r12     ; 2*cap < need ?
    beqz  r2, oba_newcap
    nop
    addu  r13, r12, r0     ; newcap = need
oba_newcap:
    addu  r4, r13, r0      ; ObjAlloc(newcap, tag, caps)
    addiu r5, r0, 0x4201
    addiu r6, r0, 0x53
    call #0x100            ; -> O1 = bigger
    nop
    oisn  r2, o1           ; alloc failed?
    bnez  r2, oba_fail
    nop
    omov  o7, o1           ; o7 = bigger
    ; copy the used prefix: ObjFetchBytes(old buf -> bigger, 0, 0, len)
    omov  o1, o5
    omov  o2, o7
    addu  r4, r0, r0
    addu  r5, r0, r0
    addu  r6, r8, r0       ; count = len
    call #0x108
    nop
    omov  o1, o5           ; free the old buffer
    call #0x101
    nop
    omov  o5, o7           ; o5 = bigger is now the buffer
oba_append:
    ; append the span: ObjFetchBytes(src -> buf, src_off, len, n)
    omov  o1, o6           ; src
    omov  o2, o5           ; dst = buf
    addu  r4, r9, r0       ; src_off
    addu  r5, r8, r0       ; dst_off = len
    addu  r6, r10, r0      ; count = n
    call #0x108
    nop
    omov  o1, o5           ; return buf
    jr    r31
    nop
oba_fail:
    onull o1              ; return null (buf, in O5, is untouched)
    jr    r31
    nop
