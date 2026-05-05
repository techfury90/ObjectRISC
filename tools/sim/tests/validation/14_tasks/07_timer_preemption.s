; @description: timer interrupt fires repeatedly; supervisor handler increments a counter, re-arms, ERETs; main loop exits once the counter reaches 5
;
; The handler proves end-to-end timer-driven preemption: the bootstrap
; spends all its cycles in a tight olw/loop reading a counter, never
; voluntarily yielding. The only way the counter advances is the
; timer-interrupt handler firing, incrementing it, re-arming COMPARE,
; re-enabling IE, and ERETing back. After five fires the loop exits.
;
; Cycle budget per fire: ~50 cycles between COMPARE arms, plus the
; handler itself (~10 instructions). Five fires comfortably fits in
; the suite's default 100k cycle cap.
; @expect-exit: 5

.entry main
.text
main:
    ; -- shared 4-byte scratch (handler will increment a tick count).
    addiu r4, r0, 4
    addiu r5, r0, 0x4102         ; TAG_DATA
    addiu r6, r0, 0x43           ; R|W|C
    call  #0x100
    omov  o7, o1                 ; O7 = scratch

    ; Initialize scratch[0:4] = 0.
    osw   r0, 0(o7)

    ; Install handler for external-interrupt (cause 0x01).
    addiu r4, r0, 1
    la    r5, timer_handler
    call  #0x520

    ; Arm timer: COMPARE = COUNT + 50 (ish).
    lctrl r8, $5                 ; r8 = COUNT
    addiu r8, r8, 50
    sctrl $6, r8                 ; COMPARE = COUNT + 50

    ; STATUS: enable IE, keep mode = supervisor.
    ;   bits [1:0] = mode = 1 (supervisor), bit 4 = IE = 1
    addiu r9, r0, 0x11
    sctrl $0, r9

    ; Tight loop until scratch[0:4] >= 5.
loop:
    olw   r10, 0(o7)
    addiu r11, r0, 5
    slt   r12, r10, r11          ; r12 = 1 if r10 < 5
    bne   r12, r0, loop
    nop

    ; r10 >= 5; exit with that count.
    move  r4, r10
    call  #0x001
    nop

timer_handler:
    ; Bump scratch[0:4] by 1.
    olw   r4, 0(o7)
    addiu r4, r4, 1
    osw   r4, 0(o7)

    ; Re-arm COMPARE = COUNT + 50 for the next quantum.
    lctrl r5, $5
    addiu r5, r5, 50
    sctrl $6, r5

    ; Re-enable IE (deliver_trap auto-cleared it).
    lctrl r6, $0                 ; STATUS
    ori   r6, r6, 0x10           ; set IE bit
    sctrl $0, r6

    eret
    nop
