; obj_or.s — the `void *__or` capability-VALUE object API (see obj_or.h).
;
; Each op is a thin passthrough to a firmware primitive. This works
; because pcc's C `__or` calling convention already matches the firmware
; register ABI for these primitives:
;
;     __or args   -> O1, O2, O3        (caller-side, Vol VII §2.1)
;     int  args   -> R4, R5, R6, R7
;     __or return -> O1
;     int  return -> R2
;
; ObjAlloc/ObjDerive/ObjFree/ReceiveQueue*/SEND all read O1 (+ O2..) and
; R4.. and leave their result in O1 / R2 — so the wrappers are almost
; empty. Writing these in C would force the per-frame OBJSTORE prologue
; (because they take `__or` params), whose ObjAllocStore setup clobbers
; the incoming R4..R6 int params before homing them — corrupting any int
; argument forwarded to the firmware call. Bare asm avoids that; it mirrors
; console_io.s's orisc_console_write passthrough exactly.
;
; Register discipline: R2..R7 are caller-saved (macdefs.h), so these leaf
; wrappers clobber them freely. R8 is likewise caller-saved and is used as
; a scratch that survives a firmware CALL (which only writes R2..R6) where
; a value must outlive the call (objor_recv_cap's out-pointer).

.text

;========================================================================
; lifecycle
;========================================================================

; void *__or objor_alloc(int len, int tag, int caps)
;   len=R4, tag=R5, caps=R6  ->  O1 = ref, R2 = status  (ObjAlloc #0x100)
objor_alloc:
    call #0x100
    nop
    jr r31
    nop

; void *__or objor_alloc_store(int len, int tag, int caps)
;   len=R4, tag=R5, caps=R6  ->  O1 = ref, R2 = status  (ObjAllocStore #0x106)
objor_alloc_store:
    call #0x106
    nop
    jr r31
    nop

; void *__or objor_derive(void *__or src, int caps)
;   src=O1, caps=R4  ->  O1 = derived ref, R2 = status  (ObjDerive #0x103)
objor_derive:
    call #0x103
    nop
    jr r31
    nop

; int objor_free(void *__or o)
;   o=O1  ->  R2 = status  (ObjFree #0x101)
objor_free:
    call #0x101
    nop
    jr r31
    nop

; void objor_drop(void *__or o)
;   A capability value owns no table slot; "dropping" it is discarding the
;   C value. No firmware action — just return.
objor_drop:
    jr r31
    nop

;========================================================================
; inspection (single OR-file instruction, no firmware call)
;========================================================================

; int objor_isnull(void *__or o)   ; o=O1 -> R2
objor_isnull:
    oisn r2, o1
    jr r31
    nop

; int objor_eq(void *__or a, void *__or b)   ; a=O1, b=O2 -> R2
objor_eq:
    oeq r2, o1, o2
    jr r31
    nop

; int objor_len(void *__or o)   ; o=O1 -> R2
objor_len:
    olen r2, o1
    jr r31
    nop

; int objor_tag(void *__or o)   ; o=O1 -> R2
objor_tag:
    otag r2, o1
    jr r31
    nop

; int objor_caps(void *__or o)   ; o=O1 -> R2
objor_caps:
    ocap r2, o1
    jr r31
    nop

;========================================================================
; byte access at offset 0
;========================================================================

; int objor_loadw(void *__or o)   ; o=O1 -> R2 = o[0]  (needs CAP_R)
objor_loadw:
    olw r2, 0(o1)
    nop
    jr r31
    nop

; void objor_storew(void *__or o, int v)   ; o=O1, v=R4  (needs CAP_W)
objor_storew:
    osw r4, 0(o1)
    jr r31
    nop

;========================================================================
; messaging
;========================================================================

; int objor_queue_attach(void *__or svc, int depth)
;   svc=O1, depth=R4  ->  R2 = status  (ReceiveQueueAttach #0x203)
objor_queue_attach:
    call #0x203
    nop
    jr r31
    nop

; int objor_send(void *__or recip, int a0, int a1, int a2, int a3)
;   recip=O1; a0..a3 = R4..R7 (already in place). Null the OR-payload
;   slots O2..O4 so no stale caps ride along, then SEND. Return 0.
objor_send:
    onull o2
    onull o3
    onull o4
    send o1
    addu r2, r0, r0
    jr r31
    nop

; int objor_send_cap(void *__or recip, void *__or p2,
;                    int a0, int a1, int a2, int a3)
;   recip=O1, p2=O2 (the OR payload, by the __or arg convention);
;   a0..a3 = R4..R7. Null O3/O4, SEND, return 0.
objor_send_cap:
    onull o3
    onull o4
    send o1
    addu r2, r0, r0
    jr r31
    nop

; void *__or objor_recv_cap(void *__or q, int *out_word)
;   q=O1, out_word=R4. Block on q's queue (ReceiveQueuePoll #0x204,
;   timeout=-1). The reply lands: R2=status, R3..R6=int payload, O1..O4=OR
;   payload (the handed-over cap rides O2). Move O2 -> O1 (the __or return
;   reg) and store R3 to *out_word. R8 holds the out-pointer across the
;   CALL (the poll writes R2..R6 but not R8).
objor_recv_cap:
    addu r8, r4, r0          ; save out_word ptr (survives the CALL)
    addiu r4, r0, -1         ; timeout = -1 (block forever)
    call #0x204
    nop
    beqz r8, orc_ret         ; skip the store when out_word == NULL
    nop
    sw r3, 0(r8)             ; *out_word = reply status word (R3)
orc_ret:
    omov o1, o2              ; return the reply's O2 capability
    jr r31
    nop

;========================================================================
; inherited-capability adoption (object-register <-> value bridge)
;
; The value API can mint caps (alloc/derive) and receive them (recv_cap),
; but a program's FIRST caps arrive in object registers the loader/parent
; set up (boot services, or a cap a parent parks for a child it spawns).
; These two ops bridge such an O-register cap into / out of the value world.
; They are the same one-instruction OMOV the handle API's obj_adopt_oN /
; obj_park_oN use; O7 is the conventional spawn-handoff register (TaskCreate
; copies O1..O15 to the child — see examples/cc/multitask/concurrent.c).
;========================================================================

; void objor_stash_o7(void *__or o)   ; o=O1 -> O7 (park for a spawned child)
objor_stash_o7:
    omov o7, o1
    jr r31
    nop

; void *__or objor_adopt_o7(void)   ; O7 -> O1 (adopt the parked/inherited cap)
objor_adopt_o7:
    omov o1, o7
    jr r31
    nop

; void objor_stash_o9(void *__or o)   ; o=O1 -> O9 (park for an orx_spawn'd child)
; O9 is callee-saved and unused by the spawn call tree (orx/vfs/task/dir), so a
; cap parked here survives orx_spawn to the child's TaskCreate O1..O15 copy —
; the object-console cross-process result-sink handoff (a launcher hands a
; command program its sink cap without an ORX_SLOT_CHILD injection slot).
objor_stash_o9:
    omov o9, o1
    jr r31
    nop

; void *__or objor_adopt_o9(void)   ; O9 -> O1 (adopt the inherited sink cap)
objor_adopt_o9:
    omov o1, o9
    jr r31
    nop

;========================================================================
; indexed OREF-array access (register-indexed OREFLD/OREFST)
;
; An ObjAllocStore object is an array of contiguous 8-byte OREF slots.
; orefldx/orefstx index it at a RUNTIME element index (the CPU scales the
; index by 8, the slot size), so these are O(1) — no per-slot switch ladder
; like task.c's task_slot. The firmware bounds-checks the scaled offset
; against the object's length exactly as the immediate OREFLD/OREFST do, so
; an out-of-range index TRAPS rather than reading or forging an OREF from
; neighbouring storage. The base must be OR-typed (objor_alloc_store) and
; carry R (get) / W (set). These are the raw slot accessors the growable
; orvec is built on.
;========================================================================

; void *__or objor_vget(void *__or vec, int i)
;   vec=O1, i=R4  ->  O1 = vec[i]   (OREFLD indexed, needs CAP_R). The nop is
;   the load-delay slot (mirrors objor_loadw).
objor_vget:
    orefldx o1, r4(o1)
    nop
    jr r31
    nop

; void objor_vset(void *__or vec, void *__or ref, int i)
;   vec=O1, ref=O2, i=R4  ->  vec[i] = ref   (OREFST indexed, needs CAP_W)
objor_vset:
    orefstx o2, r4(o1)
    jr r31
    nop
