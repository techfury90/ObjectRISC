; console_io.s — minimal C-callable bridge to firmware ConsoleWrite.
;
; Provides one function:
;
;     int console_write(const char *buf, int count);
;
; Treats `buf` as a virtual address in the data section (mapped at
; 0x40000 by the loader per CONTRACT.md §2). Subtracts the data base
; to get the byte offset within the data object, then issues the
; firmware ConsoleWrite primitive (0x320) with O1 = the data
; section's object reference (preserved at O3 since boot — pcc-
; compiled C never touches the OR file in this iteration).
;
; This wrapper exists because the OR file isn't yet exposed to C
; (no `__or` qualifier in our pcc port, no OL/OS patterns in
; table.c). Once that lands, this file goes away — C will be able
; to call the firmware primitive directly via __builtin_orisc_call.

.text

console_write:
    ; r4 = source VA (in data section), r5 = byte count.
    ; Convert VA → offset within data: offset = r4 - 0x40000.
    li    r1, 0x40000
    subu  r4, r4, r1

    omov  o1, o3              ; data section reference (full caps)
    call  #0x320              ; ConsoleWrite — clobbers r2, r3
    nop                       ; (CALL has no delay slot, but pad
                              ;  for symmetry with adjacent calls)

    jr    r31                 ; return to caller; r2 holds status
    nop                       ; (jr delay slot)
