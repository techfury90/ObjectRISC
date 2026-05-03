; parallel_primes.s — count π(N) across four CPUs, stream to a terminal.
;
; A multi-process Object RISC demo. Loaded onto four CPUs (PID 0..3) plus
; one terminal device at PID 16. Branches on R7 (PROCID):
;   PID 0  — coordinator + worker for [2..LIM/4]
;   PID k  — worker for the k'th quarter  (k = 1, 2, 3)
;
; Each worker installs a handler on its own service object and TaskExits
; so the handler runs on incoming SEND. The coordinator allocates a
; reply object with a depth-3 receive queue, derives a send-only cap on
; it, and SENDs each work request with that cap as a payload OR. Workers
; SEND back (worker_id, count, elapsed) through the cap.
;
; Range bounds:
;   coord:    [2 .. 500]
;   worker 1: [501 .. 1000]
;   worker 2: [1001 .. 1500]
;   worker 3: [1501 .. 2000]
;
; Run with:
;   python3 tools/oriscrun \
;       --terminal pid=16 \
;       --cpu pid=0:program=examples/parallel_primes.orx,service=16=1@9,service=1=4@9,service=2=4@9,service=3=4@9 \
;       --cpu pid=1:program=examples/parallel_primes.orx,serve \
;       --cpu pid=2:program=examples/parallel_primes.orx,serve \
;       --cpu pid=3:program=examples/parallel_primes.orx,serve
;
; Worker service objects sit at local index 4 (after code, stack, data
; allocated by init_cpu); the terminal's console hardcodes index 1.
;
; Conventions throughout (callee-preserved across helper jal's):
;   o9   = code object reference (saved at entry, preserved)
;   o11  = terminal service ref            (coord only; from --service slot)
;   o12  = own service ref (with queue)    (coord only; for terminal acks)
;   o13  = reply object ref (with queue)   (coord only; for worker replies)
;   o14  = send-only cap on o13            (coord only; given to workers)
;   o15  = send-only cap on o12            (coord only; given to terminal)
;   o5   = output buffer object ref        (coord only; we MapObject it)
;   r24  = output buffer base VA           (coord only; for SB writes)
;   r25  = running total of primes counted (coord only)

.entry main

.text

;========================================================================
; main — branch on PROCID
;========================================================================
main:
    omov  o9, o1                  ; o9 = code object (used by workers)
    beq   r7, r0, coordinator
    nop
    j     worker
    nop

;========================================================================
; coordinator (PID 0)
;========================================================================
coordinator:
    omov  o11, o5                 ; preserve terminal service
    omov  o12, o4                 ; preserve own service

    ; ------- allocate output buffer (256 B, R+W) and map it ------------
    addiu r4, r0, 256
    addiu r5, r0, 0x4200
    addiu r6, r0, 0x03            ; R+W
    call  #0x100                  ; ObjAlloc
    bne   r2, r0, fatal
    nop
    omov  o5, o1                  ; o5 = output buffer object

    omov  o1, o5
    addu  r4, r0, r0              ; va_hint = 0 (firmware picks)
    addu  r5, r0, r0              ; offset = 0
    addiu r6, r0, 0x03            ; prot = R+W
    addiu r7, r0, 256
    call  #0x110                  ; MapObject
    bne   r2, r0, fatal
    nop
    addu  r24, r3, r0             ; r24 = buffer VA

    ; ------- own-service receive queue + ack send-cap (for terminal) ---
    omov  o1, o12
    addiu r4, r0, 1
    call  #0x203                  ; ReceiveQueueAttach
    bne   r2, r0, fatal
    nop
    omov  o1, o12
    addiu r4, r0, 0x08            ; mask = S
    call  #0x103                  ; ObjDerive
    bne   r2, r0, fatal
    nop
    omov  o15, o1                 ; ack send-cap

    ; ------- reply object + queue + send-cap (for workers) -------------
    addiu r4, r0, 4
    addiu r5, r0, 0x4201
    addiu r6, r0, 0x5B            ; R+W+S+V+C
    call  #0x100
    bne   r2, r0, fatal
    nop
    omov  o13, o1

    omov  o1, o13
    addiu r4, r0, 3               ; depth = 3
    call  #0x203
    bne   r2, r0, fatal
    nop

    omov  o1, o13
    addiu r4, r0, 0x08            ; mask = S
    call  #0x103
    bne   r2, r0, fatal
    nop
    omov  o14, o1                 ; reply send-cap for workers

    ; ------- print header ---------------------------------------------
    la    r4, str_header
    addiu r5, r0, 33              ; "Parallel pi(2000) across 4 CPUs:\n"
    jal   print_lit
    nop

    ; ------- dispatch work to PID 1, 2, 3 ------------------------------
    omov  o1, o6                  ; PID 1 service
    omov  o2, o14                 ; payload OR1 = reply send-cap
    onull o3
    onull o4
    addiu r4, r0, 1               ; worker id
    addiu r5, r0, 501             ; lo
    addiu r6, r0, 1000            ; hi
    addiu r7, r0, 0
    send  o1

    omov  o1, o7                  ; PID 2 service
    omov  o2, o14
    onull o3
    onull o4
    addiu r4, r0, 2
    addiu r5, r0, 1001
    addiu r6, r0, 1500
    addiu r7, r0, 0
    send  o1

    omov  o1, o8                  ; PID 3 service
    omov  o2, o14
    onull o3
    onull o4
    addiu r4, r0, 3
    addiu r5, r0, 1501
    addiu r6, r0, 2000
    addiu r7, r0, 0
    send  o1

    ; ------- coordinator does its own range [2..500] -------------------
    call  #0x301                  ; ReadCycles
    addu  r16, r3, r0             ; start

    addiu r4, r0, 2
    addiu r5, r0, 500
    jal   count_primes
    nop
    addu  r17, r2, r0             ; count

    call  #0x301
    subu  r18, r3, r16            ; elapsed

    addu  r25, r17, r0            ; total = our count

    addu  r4, r0, r0              ; cpu id = 0
    addu  r5, r17, r0
    addu  r6, r18, r0
    jal   print_result
    nop

    ; ------- poll reply queue three times ------------------------------
    addiu r19, r0, 3
poll_loop:
    omov  o1, o13
    addiu r4, r0, -1              ; infinite timeout
    call  #0x204                  ; ReceiveQueuePoll
    bne   r2, r0, fatal
    nop
    ; r3 = worker_id, r4 = count, r5 = elapsed
    addu  r20, r3, r0
    addu  r21, r4, r0
    addu  r22, r5, r0
    addu  r25, r25, r21

    addu  r4, r20, r0
    addu  r5, r21, r0
    addu  r6, r22, r0
    jal   print_result
    nop

    addiu r19, r19, -1
    bne   r19, r0, poll_loop
    nop

    ; ------- print total -----------------------------------------------
    addu  r4, r25, r0
    jal   print_total
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

;========================================================================
; worker (PID 1, 2, 3) — install handler then TaskExit
;========================================================================
worker:
    omov  o1, o4                  ; target = own service
    omov  o2, o9                  ; code object
    la    r5, prime_handler
    lui   r6, 0x0001              ; 0x10000 = code base
    subu  r4, r5, r6              ; offset within code object
    call  #0x200                  ; InstallHandler
    bne   r2, r0, fatal
    nop

    addiu r4, r0, 0
    call  #0x001
    nop

;========================================================================
; prime_handler — invoked by firmware on incoming SEND
;
; Entry: O1 = self (recipient with full caps),
;        O2 = sender's O1 = recipient (= self with sender's caps; ignored),
;        O3 = sender's O2 = reply send-cap,
;        O4 = sender's O3 = null (unused).
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

    omov  o9, o3                  ; preserve reply cap (sender's O2 → our O3)
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
; count_primes(r4 = lo, r5 = hi) -> r2 = count
;
; Counts primes p with lo <= p <= hi by trial division.
;========================================================================
count_primes:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    sw    r16, 8(sp)              ; current candidate
    sw    r17, 4(sp)              ; hi
    sw    r18, 0(sp)              ; running count

    addu  r16, r4, r0
    addu  r17, r5, r0
    addu  r18, r0, r0

    ; Count 2 separately if [lo..hi] contains 2.
    addiu r1, r0, 2
    sltu  r2, r17, r1             ; r2 = (hi < 2)?
    bne   r2, r0, cp_no_two
    nop
    sltu  r2, r1, r16             ; r2 = (2 < lo)?  i.e. lo > 2
    bne   r2, r0, cp_no_two
    nop
    addiu r18, r18, 1             ; counted 2
cp_no_two:
    ; Advance r16 to first odd >= max(r16, 3).
    addiu r1, r0, 3
    sltu  r2, r16, r1             ; r2 = (r16 < 3)?
    beq   r2, r0, cp_align_odd
    nop
    addiu r16, r0, 3
cp_align_odd:
    andi  r1, r16, 1
    bne   r1, r0, cp_loop
    nop
    addiu r16, r16, 1

cp_loop:
    sltu  r1, r17, r16            ; (hi < n)?
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
;
; Assumes n is odd and n >= 3. Trial-divides by odd d while d*d <= n.
;========================================================================
is_prime:
    addiu r5, r0, 3
ip_loop:
    multu r5, r5
    mflo  r6                      ; d*d
    sltu  r1, r4, r6              ; (n < d*d)?
    bne   r1, r0, ip_prime
    nop

    divu  r4, r5
    mfhi  r6                      ; remainder
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
; print_lit(r4 = src VA, r5 = length)
;   SENDs the bytes at [src..src+length) on the data section directly
;   to the terminal — used for fully-static strings (header).
;
;   o3 still carries the data section ref at boot; we use it as the
;   SEND source. The terminal's OBJ_READ_REQ resolves through the
;   home CPU's descriptor table.
;========================================================================
print_lit:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    sw    r16, 8(sp)              ; offset within data
    sw    r17, 4(sp)              ; length

    ; Convert VA to offset within data section: offset = src - 0x40000.
    lui   r1, 0x0004
    subu  r16, r4, r1
    addu  r17, r5, r0

    omov  o1, o11                 ; terminal
    omov  o2, o3                  ; payload OR1 = data section ref
    omov  o3, o15                 ; payload OR2 = ack send-cap
    onull o4
    addu  r4, r16, r0             ; offset
    addu  r5, r17, r0             ; length
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    ; Wait for the terminal's ack via our own service queue.
    omov  o1, o12
    addiu r4, r0, -1
    call  #0x204
    bne   r2, r0, fatal
    nop

    lw    r17, 4(sp)
    lw    r16, 8(sp)
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16

;========================================================================
; print_buf(r4 = length)
;   SENDs the first `length` bytes of the output buffer (o5) to the
;   terminal and waits for the ack. Caller has already populated the
;   buffer at offsets [0..length).
;========================================================================
print_buf:
    addiu sp, sp, -8
    sw    r31, 4(sp)
    sw    r16, 0(sp)
    addu  r16, r4, r0

    omov  o1, o11
    omov  o2, o5                  ; payload[0] = output buffer
    omov  o3, o15                 ; payload[1] = ack send-cap
    onull o4
    addiu r4, r0, 0               ; offset in source
    addu  r5, r16, r0             ; length
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    omov  o1, o12
    addiu r4, r0, -1
    call  #0x204
    bne   r2, r0, fatal
    nop

    lw    r16, 0(sp)
    lw    r31, 4(sp)
    jr    r31
    addiu sp, sp, 8

;========================================================================
; print_result(r4 = cpu_id, r5 = count, r6 = elapsed)
;   Formats "CPU <id>: <count> primes in <elapsed> cycles\n" into the
;   output buffer and prints it.
;========================================================================
print_result:
    addiu sp, sp, -32
    sw    r31, 28(sp)
    sw    r16, 24(sp)             ; cpu id
    sw    r17, 20(sp)             ; count
    sw    r18, 16(sp)             ; elapsed
    sw    r19, 12(sp)             ; cursor offset

    addu  r16, r4, r0
    addu  r17, r5, r0
    addu  r18, r6, r0
    addu  r19, r0, r0             ; cursor = 0

    ; "CPU "
    la    r4, str_cpu
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, 4
    jal   emit_lit
    nop
    addu  r19, r19, r2

    ; <cpu id>
    addu  r4, r16, r0
    addu  r5, r24, r0
    addu  r5, r5, r19
    jal   itoa
    nop
    addu  r19, r19, r2

    ; ": "
    la    r4, str_colon
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, 2
    jal   emit_lit
    nop
    addu  r19, r19, r2

    ; <count>
    addu  r4, r17, r0
    addu  r5, r24, r0
    addu  r5, r5, r19
    jal   itoa
    nop
    addu  r19, r19, r2

    ; " primes in "
    la    r4, str_primesin
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, 11
    jal   emit_lit
    nop
    addu  r19, r19, r2

    ; <elapsed>
    addu  r4, r18, r0
    addu  r5, r24, r0
    addu  r5, r5, r19
    jal   itoa
    nop
    addu  r19, r19, r2

    ; " cycles\n"
    la    r4, str_cyclesnl
    addu  r5, r24, r0
    addu  r5, r5, r19
    addiu r6, r0, 8
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
; print_total(r4 = total)
;   Formats "\nTotal: pi(2000) = <total>\n" and prints it.
;========================================================================
print_total:
    addiu sp, sp, -16
    sw    r31, 12(sp)
    sw    r16, 8(sp)
    sw    r17, 4(sp)              ; cursor

    addu  r16, r4, r0
    addu  r17, r0, r0

    la    r4, str_totalpref
    addu  r5, r24, r0
    addu  r5, r5, r17
    addiu r6, r0, 18
    jal   emit_lit
    nop
    addu  r17, r17, r2

    addu  r4, r16, r0
    addu  r5, r24, r0
    addu  r5, r5, r17
    jal   itoa
    nop
    addu  r17, r17, r2

    la    r4, str_nl
    addu  r5, r24, r0
    addu  r5, r5, r17
    addiu r6, r0, 1
    jal   emit_lit
    nop
    addu  r17, r17, r2

    addu  r4, r17, r0
    jal   print_buf
    nop

    lw    r17, 4(sp)
    lw    r16, 8(sp)
    lw    r31, 12(sp)
    jr    r31
    addiu sp, sp, 16

;========================================================================
; emit_lit(r4 = src VA, r5 = dst VA, r6 = length) -> r2 = length
;   Copies `length` bytes from [src..src+length) into [dst..dst+length).
;   Used to splice fixed strings out of the data section into the
;   output buffer at a caller-chosen offset.
;========================================================================
emit_lit:
    addu  r2, r6, r0              ; return length
el_loop:
    beq   r6, r0, el_done
    nop
    lb    r1, 0(r4)
    sb    r1, 0(r5)
    addiu r4, r4, 1
    addiu r5, r5, 1
    j     el_loop
    addiu r6, r6, -1              ; (delay slot) decrement remaining

el_done:
    jr    r31
    nop

;========================================================================
; itoa(r4 = value, r5 = dst_va) -> r2 = bytes written
;========================================================================
itoa:
    addiu sp, sp, -32
    sw    r31, 28(sp)
    sw    r16, 24(sp)             ; remaining value
    sw    r17, 20(sp)             ; dst va
    sw    r18, 16(sp)             ; digit count
    ; sp+0..sp+15 holds digits in reverse order

    addu  r16, r4, r0
    addu  r17, r5, r0

    bne   r16, r0, it_loop
    nop
    ; Special case 0
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
    mfhi  r2                      ; digit
    addiu r2, r2, 0x30
    addu  r3, sp, r18
    sb    r2, 0(r3)
    addiu r18, r18, 1
    j     it_loop_body
    nop

it_emit:
    addu  r2, r18, r0             ; bytes to return
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
; fatal — write 99 to exit code and TaskExit
;========================================================================
fatal:
    addiu r4, r0, 99
    call  #0x001
    nop

;========================================================================
; .data — static strings
;========================================================================
.data
str_header:
    .string "Parallel pi(2000) across 4 CPUs:\n"
str_header_end:

str_cpu:
    .string "CPU "
str_cpu_end:

str_colon:
    .string ": "
str_colon_end:

str_primesin:
    .string " primes in "
str_primesin_end:

str_cyclesnl:
    .string " cycles\n"
str_cyclesnl_end:

str_totalpref:
    .string "Total: pi(2000) = "
str_totalpref_end:

str_nl:
    .string "\n"
str_nl_end:

;========================================================================
; Length constants — labels are absolute VAs, so subtract pairs to get
; lengths. The assembler treats `.set name, expr` as a symbol = expr,
; but our assembler doesn't; instead we hand-compute by counting bytes.
; (See the matching addiu r5, r0, str_*_len in code above.)
;========================================================================

; Hand-counted from above:
;   "Parallel pi(2000) across 4 CPUs:\n"  = 33
;   "CPU "                                 = 4
;   ": "                                   = 2
;   " primes in "                          = 11
;   " cycles\n"                            = 8
;   "Total: pi(2000) = "                   = 18
;   "\n"                                   = 1

