; preempt_handler.s — generic timer-interrupt trap handler.
;
; Phase 36 wires this into supervisor programs (the shell) that
; want their CPU stay responsive even when a child task is
; CPU-bound. Installed via task_install_preempt_timer (libc),
; which calls Vol VI #0x520 InstallTrapHandler with this label
; as the supervisor handler for cause 0x01 (external-interrupt),
; arms COMPARE = COUNT + quantum, and enables STATUS.IE.
;
; The handler itself:
;   1. Re-arm COMPARE = COUNT + 5000 cycles for the next preempt.
;   2. Re-enable STATUS.IE (deliver_trap auto-cleared it).
;   3. Call TaskYield. Phase 35's deferred-yield idiom kicks in:
;      because cpu.in_trap_handler is set, primitive_TaskYield
;      sets cpu.yield_pending = True and returns immediately
;      instead of trying an immediate context switch (which
;      would clobber the interrupted task's saved trap state).
;   4. ERET. The ERET path notices yield_pending and switches
;      to the next runnable task before resuming user mode.
;
; Quantum hard-coded to 5000 cycles for now — long enough that
; the per-tick overhead is negligible, short enough that an
; interactive shell stays snappy. Wiring this into a libc
; configuration knob would let callers tune it.

.text

.global preempt_timer_handler
preempt_timer_handler:
    ; Re-arm: COMPARE = COUNT + 5000.
    lctrl r5, $5
    addiu r5, r5, 5000
    sctrl $6, r5
    ; Re-enable STATUS.IE (bit 4) — deliver_trap auto-cleared it
    ; for cause 0x01 to keep the handler from re-firing on the
    ; very next instruction.
    lctrl r6, $0
    ori   r6, r6, 0x10
    sctrl $0, r6
    ; Defer-yield: TaskYield from a trap-handler context flips
    ; cpu.yield_pending and returns, which the ERET below honors.
    call  #0x004
    nop
    eret
    nop
