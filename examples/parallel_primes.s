; parallel_primes.s — parameterized parallel π(N) demo, streamed to a terminal.
;
; Loaded onto K+1 CPUs (PID 0..K) plus one terminal device at PID 16.
; CPU 0 is the coordinator and also computes its own quarter-share;
; CPUs 1..K are workers. The coordinator partitions [2..N] into K+1
; equal ranges, dispatches K of them via SEND with a derived reply
; cap, computes its own range, polls K replies, and prints the total.
;
; Both N (upper bound) and K (number of workers) live as .word
; constants in the data section, patched per-run by the launcher
; script `examples/run_parallel_primes`. To support varying K without
; the OR-slot conflicts that would otherwise be inevitable (worker
; service refs land in O6..O(5+K), but the coord wants O9..O15 for
; its own state), the coord saves all of O6..O15 into an OBJSTORE
; backing object up front and then loads them back on demand via a
; jump table of OREFLD instructions — each table entry picks one of
; ten possible offsets into the OBJSTORE buffer.
;
; The dispatch table is an architectural showcase as well as a fix:
; OBJSTORE storage and OREFLD/OREFST are exactly the mechanism
; Volume III §5.4 added for situations like this — saving and
; restoring object references without violating the capability
; invariant.
;
; Run via:
;   examples/run_parallel_primes -N 2000 -w 3
;
; Conventions throughout (callee-preserved across helper jal's):
;   o5   = output buffer object ref       (coord only; we MapObject it)
;   o6   = OBJSTORE buffer of saved refs  (coord only; slots 0..9 = orig o6..o15)
;   o7   = terminal service ref           (coord only)
;   o8   = own service ref (queued)       (coord only; for terminal acks)
;   o9   = code object ref                (workers only)
;   o10  = reply object ref (queued)      (coord only; for worker replies)
;   o11  = reply send-cap                 (coord only; given to workers)
;   o12  = ack send-cap                   (coord only; given to terminal)
;   r24  = output buffer base VA          (coord only; for SB writes)
;   r25  = N (upper bound)                (coord only; loaded from .data)
;   r26  = K (worker count)               (coord only; loaded from .data)
;   r27  = range_size = N / (K+1)         (coord only)
;   r28  = running total of primes        (coord only)

.entry main

; ---- Constants ---------------------------------------------------------
; CODE_BASE is the VA at which the loader maps the boot code object
; (CONTRACT.md §2). We need the offset of prime_handler *within* the
; code object — InstallHandler wants that, not the absolute VA — so we
; subtract CODE_BASE from the label.
.set CODE_BASE, 0x10000

.text

;========================================================================
; main — branch on PROCID
;========================================================================
main:
    beq   r7, r0, coordinator
    nop
    j     worker
    nop

;========================================================================
; coordinator (PID 0)
;========================================================================
coordinator:
    ; ------- save o6..o15 to OBJSTORE before clobbering ----------------
    addiu r4, r0, 80              ; 10 slots × 8 bytes
    addiu r5, r0, 0x4202
    addiu r6, r0, 0x03            ; R+W (we do not need to share)
    call  #0x106                  ; ObjAllocStore
    bne   r2, r0, fatal
    nop
    ; o1 = OBJSTORE buf. Save originals before we overwrite anything.
    orefst o6, 0(o1)
    orefst o7, 8(o1)
    orefst o8, 16(o1)
    orefst o9, 24(o1)
    orefst o10, 32(o1)
    orefst o11, 40(o1)
    orefst o12, 48(o1)
    orefst o13, 56(o1)
    orefst o14, 64(o1)
    orefst o15, 72(o1)
    omov  o6, o1                  ; from now on, o6 = OBJSTORE buf

    ; ------- recover terminal (orig o5) and own service (orig o4) ------
    omov  o7, o5                  ; preserve terminal service
    omov  o8, o4                  ; preserve own service

    ; ------- allocate output buffer (256 B, R+W) and map it ------------
    addiu r4, r0, 256
    addiu r5, r0, 0x4200
    addiu r6, r0, 0x03            ; R+W
    call  #0x100                  ; ObjAlloc
    bne   r2, r0, fatal
    nop
    omov  o5, o1                  ; o5 = output buffer object

    omov  o1, o5
    addu  r4, r0, r0              ; va_hint = 0
    addu  r5, r0, r0              ; offset = 0
    addiu r6, r0, 0x03            ; prot = R+W
    addiu r7, r0, 256
    call  #0x110                  ; MapObject
    bne   r2, r0, fatal
    nop
    addu  r24, r3, r0             ; r24 = buffer VA

    ; ------- own-service receive queue + ack send-cap (for terminal) ---
    omov  o1, o8
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fatal
    nop
    omov  o1, o8
    addiu r4, r0, 0x08            ; mask = S
    call  #0x103                  ; ObjDerive
    bne   r2, r0, fatal
    nop
    omov  o12, o1                 ; ack send-cap

    ; ------- reply object + queue + send-cap (for workers) -------------
    addiu r4, r0, 4
    addiu r5, r0, 0x4201
    addiu r6, r0, 0x5B            ; R+W+S+V+C
    call  #0x100
    bne   r2, r0, fatal
    nop
    omov  o10, o1                 ; reply object

    ; Attach a queue deep enough for K replies. K is at most 10.
    omov  o1, o10
    addiu r4, r0, 10
    call  #0x203
    bne   r2, r0, fatal
    nop

    omov  o1, o10
    addiu r4, r0, 0x08            ; mask = S
    call  #0x103
    bne   r2, r0, fatal
    nop
    omov  o11, o1                 ; reply send-cap for workers

    ; ------- load N and K from .data, compute range_size ---------------
    la    r1, config_N
    lw    r25, 0(r1)              ; r25 = N
    la    r1, config_K
    lw    r26, 0(r1)              ; r26 = K (number of workers)

    ; range_size = N / (K + 1)
    addiu r1, r26, 1              ; K + 1
    divu  r25, r1
    mflo  r27                     ; r27 = range_size

    ; ------- print header: "Parallel pi(<N>) across <K+1> CPUs:\n" -----
    addiu r1, r26, 1              ; K + 1 = total CPU count
    addu  r4, r25, r0
    addu  r5, r1, r0
    jal   print_header
    nop

    ; ------- dispatch work to workers 1..K -----------------------------
    addiu r19, r0, 1              ; w = 1
dispatch_loop:
    sltu  r1, r26, r19            ; (K < w)?
    bne   r1, r0, dispatch_done
    nop

    ; lo = w * range_size + 1
    multu r19, r27
    mflo  r20
    addiu r20, r20, 1             ; r20 = lo

    ; hi = (w+1) * range_size  (clamp last worker to N)
    addiu r1, r19, 1
    multu r1, r27
    mflo  r21                     ; r21 = (w+1) * range_size
    sltu  r1, r19, r26            ; (w < K)?  i.e. not the last worker
    bne   r1, r0, send_to_worker
    nop
    addu  r21, r25, r0             ; last worker's hi clamps to N

send_to_worker:
    ; jal dispatch_to_worker(r4=w) → o1 = worker w's service ref
    addu  r4, r19, r0
    jal   dispatch_to_worker
    nop
    ; o1 now holds worker w's service ref. Build SEND.
    omov  o2, o11                 ; payload OR1 = reply send-cap
    onull o3
    onull o4
    addu  r4, r19, r0             ; worker id
    addu  r5, r20, r0             ; lo
    addu  r6, r21, r0             ; hi
    addiu r7, r0, 0
    send  o1

    addiu r19, r19, 1
    j     dispatch_loop
    nop

dispatch_done:

    ; ------- coord does its own range [2 .. range_size] ----------------
    call  #0x301                  ; ReadCycles
    addu  r16, r3, r0             ; start

    addiu r4, r0, 2
    addu  r5, r27, r0             ; hi = range_size
    jal   count_primes
    nop
    addu  r17, r2, r0             ; count

    call  #0x301
    subu  r18, r3, r16            ; elapsed

    addu  r28, r17, r0            ; total = our count

    addu  r4, r0, r0              ; cpu id = 0
    addu  r5, r17, r0
    addu  r6, r18, r0
    jal   print_result
    nop

    ; ------- poll reply queue K times ----------------------------------
    addu  r19, r26, r0            ; r19 = K (replies remaining)
poll_loop:
    omov  o1, o10
    addiu r4, r0, -1              ; infinite timeout
    call  #0x204                  ; ReceiveQueuePoll
    bne   r2, r0, fatal
    nop
    ; r3 = worker_id, r4 = count, r5 = elapsed
    addu  r20, r3, r0
    addu  r21, r4, r0
    addu  r22, r5, r0
    addu  r28, r28, r21

    addu  r4, r20, r0
    addu  r5, r21, r0
    addu  r6, r22, r0
    jal   print_result
    nop

    addiu r19, r19, -1
    bne   r19, r0, poll_loop
    nop

    ; ------- print total: "Total: pi(<N>) = <total>\n" -----------------
    addu  r4, r25, r0
    addu  r5, r28, r0
    jal   print_total
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

;========================================================================
; worker (PID 1..K) — install handler then TaskExit
;========================================================================
worker:
    omov  o9, o1                  ; o9 = code object
    omov  o1, o4                  ; target = own service
    omov  o2, o9
    li    r4, prime_handler - CODE_BASE   ; offset within code object
    call  #0x200                  ; InstallHandler
    bne   r2, r0, fatal
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

;========================================================================
; prime_handler — invoked by firmware on incoming SEND
;
; Entry: O3 = sender's O2 = reply send-cap (sender's O1 → our O2 is
;        the recipient ref, which we ignore).
;        R4 = worker_id, R5 = lo, R6 = hi.
;========================================================================
prime_handler:
    addiu sp, sp, -32
    sw    r31, 28(sp)
    sw    r16, 24(sp)             ; worker id
    sw    r17, 20(sp)             ; lo
    sw    r18, 16(sp)             ; hi
    sw    r19, 12(sp)             ; start cycles
    sw    r20, 8(sp)              ; count
    sw    r21, 4(sp)              ; elapsed

    omov  o9, o3                  ; preserve reply cap
    addu  r16, r4, r0
    addu  r17, r5, r0
    addu  r18, r6, r0

    call  #0x301
    addu  r19, r3, r0

    addu  r4, r17, r0
    addu  r5, r18, r0
    jal   count_primes
    nop
    addu  r20, r2, r0

    call  #0x301
    subu  r21, r3, r19

    omov  o1, o9                  ; reply send-cap
    onull o2
    onull o3
    onull o4
    addu  r4, r16, r0             ; worker id
    addu  r5, r20, r0             ; count
    addu  r6, r21, r0             ; elapsed
    addiu r7, r0, 0
    send  o1

    addiu r4, r0, 0
    call  #0x001
    nop

;========================================================================
; dispatch_to_worker(r4 = w in 1..10) → o1 = worker w's service ref
;
; Loads the original o(5+w) value (which we OREFST'd into the OBJSTORE
; buffer in o6 at startup) into o1. The jump table indexes by w-1 with
; one (jr r31; orefld o1, OFFSET(o6)) entry per worker. The OREFLD
; runs in the jr's delay slot — the load completes before control
; returns to the caller.
;========================================================================
dispatch_to_worker:
    addiu r1, r4, -1              ; r1 = w - 1
    sll   r1, r1, 3               ; r1 = (w-1) * 8
    la    r2, dispatch_table
    addu  r1, r1, r2
    jr    r1
    nop

dispatch_table:
    ; w = 1 — original o6
    jr    r31
    orefld o1, 0(o6)
    ; w = 2 — original o7
    jr    r31
    orefld o1, 8(o6)
    ; w = 3 — original o8
    jr    r31
    orefld o1, 16(o6)
    ; w = 4 — original o9
    jr    r31
    orefld o1, 24(o6)
    ; w = 5 — original o10
    jr    r31
    orefld o1, 32(o6)
    ; w = 6 — original o11
    jr    r31
    orefld o1, 40(o6)
    ; w = 7 — original o12
    jr    r31
    orefld o1, 48(o6)
    ; w = 8 — original o13
    jr    r31
    orefld o1, 56(o6)
    ; w = 9 — original o14
    jr    r31
    orefld o1, 64(o6)
    ; w = 10 — original o15
    jr    r31
    orefld o1, 72(o6)

;========================================================================
; count_primes(r4 = lo, r5 = hi) -> r2 = count
;========================================================================
count_primes:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    sw    r16, 8(sp)
    sw    r17, 4(sp)
    sw    r18, 0(sp)

    addu  r16, r4, r0
    addu  r17, r5, r0
    addu  r18, r0, r0

    addiu r1, r0, 2
    sltu  r2, r17, r1
    bne   r2, r0, cp_no_two
    nop
    sltu  r2, r1, r16
    bne   r2, r0, cp_no_two
    nop
    addiu r18, r18, 1
cp_no_two:
    addiu r1, r0, 3
    sltu  r2, r16, r1
    beq   r2, r0, cp_align_odd
    nop
    addiu r16, r0, 3
cp_align_odd:
    andi  r1, r16, 1
    bne   r1, r0, cp_loop
    nop
    addiu r16, r16, 1

cp_loop:
    sltu  r1, r17, r16
    bne   r1, r0, cp_done
    nop

    addu  r4, r16, r0
    jal   is_prime
    nop
    beq   r2, r0, cp_skip
    nop
    addiu r18, r18, 1
cp_skip:
    addiu r16, r16, 2
    j     cp_loop
    nop

cp_done:
    addu  r2, r18, r0
    lw    r18, 0(sp)
    lw    r17, 4(sp)
    lw    r16, 8(sp)
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16

;========================================================================
; is_prime(r4 = n) -> r2 = (1 if prime else 0)
; Assumes n is odd and n >= 3.
;========================================================================
is_prime:
    addiu r5, r0, 3
ip_loop:
    multu r5, r5
    mflo  r6
    sltu  r1, r4, r6
    bne   r1, r0, ip_prime
    nop

    divu  r4, r5
    mfhi  r6
    beq   r6, r0, ip_not_prime
    nop

    addiu r5, r5, 2
    j     ip_loop
    nop

ip_prime:
    addiu r2, r0, 1
    jr    r31
    nop

ip_not_prime:
    addu  r2, r0, r0
    jr    r31
    nop

;========================================================================
; print_buf(r4 = length)
;   SENDs the first `length` bytes of the output buffer (o5) to the
;   terminal and waits for the ack.
;========================================================================
print_buf:
    addiu sp, sp, -8
    sw    r31, 4(sp)
    sw    r16, 0(sp)
    addu  r16, r4, r0

    omov  o1, o7                  ; terminal
    omov  o2, o5                  ; payload[0] = output buffer
    omov  o3, o12                 ; payload[1] = ack send-cap
    onull o4
    addiu r4, r0, 0
    addu  r5, r16, r0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    omov  o1, o8
    addiu r4, r0, -1
    call  #0x204
    bne   r2, r0, fatal
    nop

    lw    r16, 0(sp)
    lw    r31, 4(sp)
    jr    r31
    addiu sp, sp, 8

;========================================================================
; print_header(r4 = N, r5 = total CPU count)
;   Streams "Parallel pi(<N>) across <count> CPUs:\n" into the buffer
;   piece by piece, then prints it.
;========================================================================
print_header:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    sw    r16, 8(sp)              ; N
    sw    r17, 4(sp)              ; CPU count
    sw    r18, 0(sp)              ; cursor

    addu  r16, r4, r0
    addu  r17, r5, r0
    addu  r18, r0, r0

    ; "Parallel pi("
    la    r4, str_parallel
    addu  r5, r24, r0
    addu  r5, r5, r18
    addiu r6, r0, str_parallel_end - str_parallel
    jal   emit_lit
    nop
    addu  r18, r18, r2

    ; <N>
    addu  r4, r16, r0
    addu  r5, r24, r0
    addu  r5, r5, r18
    jal   itoa
    nop
    addu  r18, r18, r2

    ; ") across "
    la    r4, str_across
    addu  r5, r24, r0
    addu  r5, r5, r18
    addiu r6, r0, str_across_end - str_across
    jal   emit_lit
    nop
    addu  r18, r18, r2

    ; <CPU count>
    addu  r4, r17, r0
    addu  r5, r24, r0
    addu  r5, r5, r18
    jal   itoa
    nop
    addu  r18, r18, r2

    ; " CPUs:\n"
    la    r4, str_cpus_nl
    addu  r5, r24, r0
    addu  r5, r5, r18
    addiu r6, r0, str_cpus_nl_end - str_cpus_nl
    jal   emit_lit
    nop
    addu  r18, r18, r2

    addu  r4, r18, r0
    jal   print_buf
    nop

    lw    r18, 0(sp)
    lw    r17, 4(sp)
    lw    r16, 8(sp)
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16

;========================================================================
; print_result(r4 = cpu_id, r5 = count, r6 = elapsed)
;   "CPU <id>: <count> primes in <elapsed> cycles\n"
;========================================================================
print_result:
    addiu sp, sp, -32
    sw    r31, 28(sp)
    sw    r16, 24(sp)
    sw    r17, 20(sp)
    sw    r18, 16(sp)
    sw    r19, 12(sp)             ; cursor

    addu  r16, r4, r0
    addu  r17, r5, r0
    addu  r18, r6, r0
    addu  r19, r0, r0

    la    r4, str_cpu
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, str_cpu_end - str_cpu
    jal   emit_lit
    nop
    addu  r19, r19, r2

    addu  r4, r16, r0
    addu  r5, r24, r0
    addu  r5, r5, r19
    jal   itoa
    nop
    addu  r19, r19, r2

    la    r4, str_colon
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, str_colon_end - str_colon
    jal   emit_lit
    nop
    addu  r19, r19, r2

    addu  r4, r17, r0
    addu  r5, r24, r0
    addu  r5, r5, r19
    jal   itoa
    nop
    addu  r19, r19, r2

    la    r4, str_primesin
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, str_primesin_end - str_primesin
    jal   emit_lit
    nop
    addu  r19, r19, r2

    addu  r4, r18, r0
    addu  r5, r24, r0
    addu  r5, r5, r19
    jal   itoa
    nop
    addu  r19, r19, r2

    la    r4, str_cyclesnl
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, str_cyclesnl_end - str_cyclesnl
    jal   emit_lit
    nop
    addu  r19, r19, r2

    addu  r4, r19, r0
    jal   print_buf
    nop

    lw    r19, 12(sp)
    lw    r18, 16(sp)
    lw    r17, 20(sp)
    lw    r16, 24(sp)
    lw    r31, 28(sp)
    jr    r31
    addiu sp, sp, 32

;========================================================================
; print_total(r4 = N, r5 = total)
;   "Total: pi(<N>) = <total>\n"
;========================================================================
print_total:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    sw    r16, 8(sp)              ; N
    sw    r17, 4(sp)              ; total
    sw    r18, 0(sp)              ; cursor

    addu  r16, r4, r0
    addu  r17, r5, r0
    addu  r18, r0, r0

    la    r4, str_total_pre
    addu  r5, r24, r0
    addu  r5, r5, r18
    addiu r6, r0, str_total_pre_end - str_total_pre
    jal   emit_lit
    nop
    addu  r18, r18, r2

    addu  r4, r16, r0
    addu  r5, r24, r0
    addu  r5, r5, r18
    jal   itoa
    nop
    addu  r18, r18, r2

    la    r4, str_eq
    addu  r5, r24, r0
    addu  r5, r5, r18
    addiu r6, r0, str_eq_end - str_eq
    jal   emit_lit
    nop
    addu  r18, r18, r2

    addu  r4, r17, r0
    addu  r5, r24, r0
    addu  r5, r5, r18
    jal   itoa
    nop
    addu  r18, r18, r2

    la    r4, str_nl
    addu  r5, r24, r0
    addu  r5, r5, r18
    addiu r6, r0, str_nl_end - str_nl
    jal   emit_lit
    nop
    addu  r18, r18, r2

    addu  r4, r18, r0
    jal   print_buf
    nop

    lw    r18, 0(sp)
    lw    r17, 4(sp)
    lw    r16, 8(sp)
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16

;========================================================================
; emit_lit(r4 = src VA, r5 = dst VA, r6 = length) -> r2 = length
;========================================================================
emit_lit:
    addu  r2, r6, r0
el_loop:
    beq   r6, r0, el_done
    nop
    lb    r1, 0(r4)
    sb    r1, 0(r5)
    addiu r4, r4, 1
    addiu r5, r5, 1
    j     el_loop
    addiu r6, r6, -1

el_done:
    jr    r31
    nop

;========================================================================
; itoa(r4 = value, r5 = dst_va) -> r2 = bytes written
;========================================================================
itoa:
    addiu sp, sp, -32
    sw    r31, 28(sp)
    sw    r16, 24(sp)
    sw    r17, 20(sp)
    sw    r18, 16(sp)
    ; sp+0..sp+15 = reverse-order digits

    addu  r16, r4, r0
    addu  r17, r5, r0

    bne   r16, r0, it_loop
    nop
    addiu r1, r0, 0x30
    sb    r1, 0(r17)
    addiu r2, r0, 1
    j     it_done
    nop

it_loop:
    addu  r18, r0, r0
it_loop_body:
    beq   r16, r0, it_emit
    nop
    addiu r1, r0, 10
    divu  r16, r1
    mflo  r16
    mfhi  r2
    addiu r2, r2, 0x30
    addu  r3, sp, r18
    sb    r2, 0(r3)
    addiu r18, r18, 1
    j     it_loop_body
    nop

it_emit:
    addu  r2, r18, r0
it_emit_loop:
    beq   r18, r0, it_done
    nop
    addiu r18, r18, -1
    addu  r3, sp, r18
    lb    r1, 0(r3)
    sb    r1, 0(r17)
    addiu r17, r17, 1
    j     it_emit_loop
    nop

it_done:
    lw    r18, 16(sp)
    lw    r17, 20(sp)
    lw    r16, 24(sp)
    lw    r31, 28(sp)
    jr    r31
    addiu sp, sp, 32

;========================================================================
; fatal — exit with code 99
;========================================================================
fatal:
    addiu r4, r0, 99
    call  #0x001
    nop

;========================================================================
; .data — runtime config and string fragments
;========================================================================
.data

; --- Config: patched by the launcher script -----------------------------
config_N:    .word 2000           ; upper bound for prime counting
config_K:    .word 3              ; number of worker CPUs

; --- String fragments. Length of each is `<label>_end - <label>` and is
;     used directly at the call site via the assembler's label arithmetic.

str_parallel:    .string "Parallel pi("
str_parallel_end:
str_across:      .string ") across "
str_across_end:
str_cpus_nl:     .string " CPUs:\n"
str_cpus_nl_end:
str_cpu:         .string "CPU "
str_cpu_end:
str_colon:       .string ": "
str_colon_end:
str_primesin:    .string " primes in "
str_primesin_end:
str_cyclesnl:    .string " cycles\n"
str_cyclesnl_end:
str_total_pre:   .string "Total: pi("
str_total_pre_end:
str_eq:          .string ") = "
str_eq_end:
str_nl:          .string "\n"
str_nl_end:
