; crt0.s — Object RISC C runtime startup.
;
; Concatenated with C-compiled programs (output of orisc-unknown-none-ccom)
; to provide the entry point. Calls main, then TaskExits with main's
; return value as the exit code.
;
; Usage:
;   orisc-unknown-none-ccom < hello.c > hello.s
;   asmorisc tools/cc/arch/orisc/crt0.s hello.s -o hello.orx
;   simorisc hello.orx
;
; The C compiler does NOT emit .entry main — this file provides the
; .entry _start directive that the asmorisc loader uses to set the
; initial PC.
;
; Initial register state at _start (per CONTRACT.md §2):
;   O1 = code object, O2 = stack object, O3 = data object
;   R7 = PROCID, SP = top of stack, R31 (RA) = 0
; We don't use any of those here — main is a leaf-style call from
; this stub.

.entry _start

.text

_start:
    ; Reserve the 16-byte outgoing-arg-spill area that main expects
    ; at the top of its frame (Vol VII §2.2). We're the bottom of
    ; the call chain, so this is what makes us a well-behaved caller.
    addiu sp, sp, -16

    ; Call main. Object RISC's JAL puts return address in R31 (RA)
    ; and the architectural delay slot follows.
    jal   main
    nop                           ; (delay slot)

    ; main's return value is in R2 per the calling convention
    ; (Vol VII §2.1). Hand it to TaskExit (firmware primitive 0x001)
    ; as the exit code (low byte of R4 used).
    addu  r4, r2, r0
    call  #0x001                  ; TaskExit — does not return
    nop                           ; (unreachable, but keeps the
                                  ; trailing word aligned)
