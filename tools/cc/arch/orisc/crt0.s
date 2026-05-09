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
;   R4 = init_r4 (TaskCreate caller's R5; Phase 51 carries
;        terminal_idx + 1 here so libc task_init can pick it up)
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

    ; Phase 51: stash R4 into a libc-known global BEFORE jal main.
    ; R4 = TaskCreate's init_r4 = parent's R5-at-TaskCreate-time, used
    ; by Phase 51's terminal_idx-propagation contract:
    ;   0   = "no terminal info" (legacy / top-level boot)
    ;   N+1 = "this child runs with terminal index N"
    ; libc task_init reads `_orisc_init_r4` and sets its
    ; my_terminal_idx accordingly. Saving here (before main runs)
    ; means main's body is free to clobber R4 — pcc's calling
    ; convention treats R4 as caller-saved scratch (Vol VII §2.1)
    ; and there's no other path to recover the original value.
    li    r1, _orisc_init_r4
    sw    r4, 0(r1)

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

.data

.global _orisc_init_r4
_orisc_init_r4:
    .word 0
