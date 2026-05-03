; console_io.s — minimal C-callable bridge to firmware ConsoleWrite.
;
; Provides one function:
;
;     int console_write(const char *buf, int count);
;
; Picks the right object reference based on `buf`'s VA range:
;   - VAs in [0x40000, 0x1f0000)   →  data section  (O3)
;   - VAs in [0x1f0000, 0x200000)  →  default stack (O2)
; The boundary assumes the default stack_size of 0x10000 from
; CONTRACT.md §2; programs with a non-default stack_size need a
; different bridge until the C compiler grows the `__or` qualifier
; and SEND/OL/OS patterns.
;
; Code section (O1) is also readable but excluded — there's no
; sensible reason to ConsoleWrite from a code object.
;
; Once `__or` lands, this whole file goes away — C will be able to
; carry around real object references and call ConsoleWrite (or any
; firmware primitive) directly via __builtin_orisc_call.

.set DATA_BASE,    0x40000
.set STACK_BOTTOM, 0x1f0000   ; 0x200000 - default stack_size (0x10000)

.text

console_write:
    ; r4 = source VA, r5 = byte count.
    ; Decide stack vs data by comparing against STACK_BOTTOM.
    li    r1, STACK_BOTTOM
    sltu  r2, r4, r1              ; r2 = (va < 0x1f0000) ? 1 : 0
    beqz  r2, cw_stack
    nop

cw_data:
    ; offset = va - 0x40000
    li    r1, DATA_BASE
    subu  r4, r4, r1
    omov  o1, o3                  ; data section reference (R cap)
    j     cw_call
    nop

cw_stack:
    ; offset = va - 0x1f0000
    li    r1, STACK_BOTTOM
    subu  r4, r4, r1
    omov  o1, o2                  ; stack object reference (R+W cap)

cw_call:
    call  #0x320                  ; ConsoleWrite — clobbers r2, r3
    nop

    jr    r31                     ; r2 holds status from ConsoleWrite
    nop                           ; (jr delay slot)


;========================================================================
; OR-aware ConsoleWrite wrapper. Lets C code with proper `__or` arg
; types call ConsoleWrite directly:
;
;     extern int orisc_console_write(void *__or src,
;                                    int offset, int count);
;
; pcc's caller-side `__or` calling convention puts the source ref
; in O1 and the int args in R4/R5 — exactly the firmware ABI. So
; this wrapper is just a passthrough: invoke #0x320 and return.
;
; Use this instead of `console_write` when you have an `__or`-typed
; reference (avoids the VA-range heuristic) and want direct firmware
; access from C without inline asm.
;========================================================================

orisc_console_write:
    call  #0x320                  ; ConsoleWrite
    nop
    jr    r31
    nop
