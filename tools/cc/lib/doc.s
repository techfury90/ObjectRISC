; doc.s — the native document model v1 (see doc.h): Block (a self-contained
; byte object) + Document (a 2-slot OR-header over a blocks orvec and a
; text-log orbuf). Bare asm, like orvec.s / orbuf.s and for the same reason
; (the ops take `__or` params, which would drag in pcc-orisc's OBJSTORE
; prologue and its int-miscompile — see the pcc-or-frame-int-param note).
;
; Every op here is a LEAF: it uses only firmware primitives (ObjAlloc #0x100,
; ObjAllocStore #0x106, ObjFetchBytes #0x108) and single OR-file instructions,
; never a jal, so no stack frame is needed. A firmware CALL touches only
; O1..O4 and R2..R6, so the working set rides O5..O7 and R8..R11.
;
; Block byte layout: [kind:4][style:4][text_len:4][rsv:4][text...] (header 16).
; Tags 0x4211 (Block) / 0x4210 (Document); caps 0x13 (R|W|V) for blocks,
; 0x53 (R|W|V|C) for the header + collections. Inlined to match doc.h/ortag.h.

.text

;========================================================================
; Block — a self-contained byte object
;========================================================================

; void *__or block_new(int kind, int style, void *__or src, int src_off,
;                      int text_len)
;   src=O1, kind=R4, style=R5, src_off=R6, text_len=R7  ->  O1 = block (or null)
block_new:
    omov  o5, o1           ; o5 = src        (survive the CALLs)
    addu  r8, r4, r0       ; r8 = kind
    addu  r9, r5, r0       ; r9 = style
    addu  r10, r6, r0      ; r10 = src_off
    addu  r11, r7, r0      ; r11 = text_len
    addiu r4, r11, 16      ; alloc size = BLOCK_HDR + text_len
    addiu r5, r0, 0x4211   ; TAG_BLOCK
    addiu r6, r0, 0x13     ; R|W|V
    call #0x100            ; ObjAlloc -> O1 = block
    nop
    oisn  r2, o1
    bnez  r2, bn_fail
    nop
    omov  o6, o1           ; o6 = block
    osw   r8, 0(o1)        ; header: kind
    osw   r9, 4(o1)        ; style
    osw   r11, 8(o1)       ; text_len
    osw   r0, 12(o1)       ; reserved = 0
    beqz  r11, bn_done     ; no text -> skip the copy
    nop
    ; copy text: ObjFetchBytes(src=o5, dst=block=o6, src_off=r10, dst_off=16, n=r11)
    omov  o1, o5
    omov  o2, o6
    addu  r4, r10, r0
    addiu r5, r0, 16
    addu  r6, r11, r0
    call #0x108            ; ObjFetchBytes
    nop
bn_done:
    omov  o1, o6           ; return block
    jr    r31
    nop
bn_fail:
    onull o1
    jr    r31
    nop

; int block_kind(void *__or b)        b=O1 -> R2
block_kind:
    olw  r2, 0(o1)
    nop
    jr   r31
    nop

; int block_style(void *__or b)       b=O1 -> R2
block_style:
    olw  r2, 4(o1)
    nop
    jr   r31
    nop

; int block_textlen(void *__or b)     b=O1 -> R2
block_textlen:
    olw  r2, 8(o1)
    nop
    jr   r31
    nop

; int block_bytelen(void *__or b)     b=O1 -> R2 = whole object size
block_bytelen:
    olen r2, o1
    jr   r31
    nop

; void block_text(void *__or b, void *__or dst, int dst_off)
;   b=O1, dst=O2, dst_off=R4. Copy block_textlen bytes from b[16] to dst[dst_off]
;   via ObjFetchBytes(src=b, dst=dst, src_off=16, dst_off, count=text_len).
block_text:
    olw   r6, 8(o1)        ; r6 = text_len (the count)
    nop
    addu  r5, r4, r0       ; r5 = dst_off
    addiu r4, r0, 16       ; r4 = src_off = BLOCK_HDR
    call #0x108            ; ObjFetchBytes (O1=b src, O2=dst already in place)
    nop
    jr    r31
    nop

;========================================================================
; Document — a 2-slot OR-header: [0]=blocks orvec, [1]=text-log orbuf
;========================================================================

; void *__or doc_new(int cap0)        cap0=R4 -> O1 = document (or null)
doc_new:
    addu  r8, r4, r0       ; r8 = cap0
    slti  r2, r8, 1        ; clamp cap0 >= 1
    beqz  r2, dn_cap_ok
    nop
    addiu r8, r0, 1
dn_cap_ok:
    ; header = ObjAllocStore(16, TAG_DOCUMENT, R|W|V|C)  -- 2 ref slots
    addiu r4, r0, 16
    addiu r5, r0, 0x4210
    addiu r6, r0, 0x53
    call #0x106
    nop
    oisn  r2, o1
    bnez  r2, dn_fail
    nop
    omov  o5, o1           ; o5 = header
    ; blocks = ObjAllocStore(cap0*8, TAG_ORVEC, R|W|V|C)
    sll   r4, r8, 3
    addiu r5, r0, 0x4200
    addiu r6, r0, 0x53
    call #0x106
    nop
    oisn  r2, o1
    bnez  r2, dn_fail
    nop
    omov  o6, o1           ; o6 = blocks orvec
    ; textlog = ObjAlloc(64, TAG_ORBUF, R|W|V|C)  -- default byte cap
    addiu r4, r0, 64
    addiu r5, r0, 0x4201
    addiu r6, r0, 0x53
    call #0x100
    nop
    oisn  r2, o1
    bnez  r2, dn_fail
    nop
    omov  o7, o1           ; o7 = textlog orbuf
    orefst o6, 0(o5)       ; header[0] = blocks
    orefst o7, 8(o5)       ; header[1] = textlog
    omov  o1, o5           ; return header
    jr    r31
    nop
dn_fail:
    onull o1
    jr    r31
    nop

; void *__or doc_blocks(void *__or doc)     doc=O1 -> O1 = blocks orvec
doc_blocks:
    orefld o1, 0(o1)
    nop
    jr    r31
    nop

; void *__or doc_textlog(void *__or doc)    doc=O1 -> O1 = text-log orbuf
doc_textlog:
    orefld o1, 8(o1)
    nop
    jr    r31
    nop

; void doc_set_blocks(void *__or doc, void *__or blocks)   doc=O1, blocks=O2
doc_set_blocks:
    orefst o2, 0(o1)       ; header[0] = blocks
    jr    r31
    nop

; void doc_set_textlog(void *__or doc, void *__or textlog)  doc=O1, textlog=O2
doc_set_textlog:
    orefst o2, 8(o1)       ; header[1] = textlog
    jr    r31
    nop
