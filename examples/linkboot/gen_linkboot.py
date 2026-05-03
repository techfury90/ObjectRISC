#!/usr/bin/env python3
"""Generate the linkboot loader and a self-contained two-CPU demo.

Outputs two files:

    linkboot.s — standalone .orx for the loader CPU. Suitable for
                 multi-process launches via oriscrun, where the master
                 lives in a separate process.
    demo.s     — combined master + loader, branching on PROCID, for
                 single-process `--processors 2` runs (validation tests,
                 quick smoke tests, embedded reuse).

The loader's copy stage is an unrolled `olw r2, OFF(o6); sw r2, OFF(r19)`
sequence, one stanza per word, because Object RISC's object-load
instructions take an immediate offset (no register-indexed OL form) and
MapObject refuses remote sources. To keep the output asm readable we
generate the unrolled section here.

MAX_WORDS sets the largest module the loader will accept; modules
beyond that are rejected with exit code 99. Bumping MAX_WORDS grows
the loader's text linearly (~5 instructions per word).
"""

from pathlib import Path

MAX_WORDS = 64               # ⇒ 256-byte module limit
HERE = Path(__file__).parent
REPO_ROOT = HERE.parent.parent
VALIDATION_DIR = REPO_ROOT / "tools" / "sim" / "tests" / "validation" / "11_multicpu"

# Module image is 8 instructions (32 B) of code immediately followed by
# the literal "Booted!\n" (8 B). Total 40 B copied; the module reads
# the message at offset 32 of its OWN loaded code object via O1 — which
# the loader leaves set to a local R|X|C ref. This keeps the demo
# self-contained (works in --processors and --connect modes alike;
# remote ConsoleWrite via direct descriptor access only works in
# single-process mode).
MODULE_BYTES = 40


# ---------------------------------------------------------------------------
# Loader (the bit that's identical in both standalone and combined builds).
# ---------------------------------------------------------------------------

def loader_body(label_prefix: str = "") -> str:
    """Emit the loader logic. `label_prefix` distinguishes labels in the
    combined demo (where it's "L_") from the standalone loader (where it's "")."""
    p = label_prefix
    parts = [f"""\
{p}main:
    ; Save PROCID (R7) — primitives clobber R4..R7.
    addu  r16, r7, r0

    ; Derive an R+S self-ref to give the master.
    omov  o1, o4
    addiu r4, r0, 0x09                 ; R|S
    call  #0x103                       ; ObjDerive
    bne   r2, r0, {p}fail
    nop
    omov  o6, o1                       ; o6 = derived ref for master

    ; ReceiveQueueAttach on self-service so we can poll for the boot SEND.
    ; Done BEFORE the first announce so that even an instantaneous master
    ; reply lands in our queue rather than getting dropped.
    omov  o1, o4
    addiu r4, r0, 4                    ; queue depth
    call  #0x203                       ; ReceiveQueueAttach
    bne   r2, r0, {p}fail
    nop

{p}announce_loop:
    ; Announce: SEND to the master in O5.
    omov  o1, o5                       ; recipient
    omov  o2, o6                       ; payload OR slot 1 = derived self-ref
    onull o3
    addu  r4, r16, r0                  ; PROCID
    addiu r5, r0, 0                    ; protocol version
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    ; Poll for the boot SEND with a finite timeout. If the master isn't
    ; connected to the crossbar yet (a real race in multi-process mode
    ; — both CPUs start asynchronously) the SEND gets dropped silently
    ; by the crossbar; we re-announce on ETIMEOUT until the master shows.
    omov  o1, o4
    lui   r4, 0x0001                   ; ~65 k cycles between retries
    call  #0x204                       ; ReceiveQueuePoll
    beq   r2, r0, {p}got_boot          ; ERR_OK = boot received
    nop
    addiu r1, r0, 7                    ; ERR_ETIMEOUT
    beq   r2, r1, {p}announce_loop     ; re-announce on timeout
    nop
    j     {p}fail                      ; any other error is fatal
    nop

{p}got_boot:
    ; Queue dispatch (Vol VI §6): R3..R6 ← sender R4..R7;
    ; O1..O4 ← sender O1..O4 verbatim.
    ;   O2 = code ref (sender's O2)
    ;   O3 = data ref (sender's O3)
    ;   R3 = length (sender's R4)
    ;   R4 = entry offset (sender's R5)
    addu  r17, r3, r0                  ; r17 = length
    addu  r18, r4, r0                  ; r18 = entry offset
    omov  o6, o2                       ; o6 = code ref
    omov  o7, o3                       ; o7 = data ref (may be null)

    ; Length sanity-check.
    addiu r1, r0, MAX_BYTES
    sltu  r2, r1, r17                  ; r2 = (MAX_BYTES < length)
    bne   r2, r0, {p}fail              ; reject oversize
    nop
    beq   r17, r0, {p}fail             ; reject zero-length
    nop

    ; ObjAlloc a writable local code object.
    addu  r4, r17, r0
    addiu r5, r0, TAG_CODE
    addiu r6, r0, 0x47                 ; R|W|X|C
    addiu r7, r0, 0
    call  #0x100                       ; ObjAlloc → O1 = new ref
    bne   r2, r0, {p}fail
    nop
    omov  o9, o1                       ; o9 = writable local code

    ; MapObject the writable local code as R+W (so we can sw to it).
    omov  o1, o9
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0x03                 ; R|W
    addu  r7, r17, r0
    call  #0x110                       ; → R3 = dst_va
    bne   r2, r0, {p}fail
    nop
    addu  r19, r3, r0                  ; r19 = dst_va

    ; Words-to-copy = length / 4. (length is checked ≤ MAX_BYTES.)
    srl   r20, r17, 2                  ; r20 = num_words

    ; ----- Unrolled copy with early-exit -------------------------------
    ; r1 counts words copied; on each iteration we OLW one word from
    ; the remote source O6 at fixed offset, sw it to dst_va + offset,
    ; and beq-out when r1 == num_words. Each stanza is 5 instructions.
    addu  r1, r0, r0                   ; words copied
"""]

    for i in range(MAX_WORDS):
        off = i * 4
        parts.append(f"""\
    olw   r2, {off}(o6)
    sw    r2, {off}(r19)
    addiu r1, r1, 1
    beq   r1, r20, {p}copy_done
    nop
""")

    parts.append(f"""\
{p}copy_done:

    ; ObjDerive to drop W; keep R|X|C.
    omov  o1, o9
    addiu r4, r0, 0x45                 ; R|X|C
    call  #0x103                       ; ObjDerive
    bne   r2, r0, {p}fail
    nop
    omov  o9, o1

    ; MapObject as R+X — gives us the executable VA.
    omov  o1, o9
    addiu r4, r0, 0
    addiu r5, r0, 0
    addiu r6, r0, 0x05                 ; R|X
    addu  r7, r17, r0
    call  #0x110                       ; → R3 = exec_va
    bne   r2, r0, {p}fail
    nop
    addu  r3, r3, r18                  ; add entry offset

    ; Final OR setup for the loaded module:
    ;   O1 = loaded code ref (local R|X|C — module can ConsoleWrite from it)
    ;   O3 = data ref the master passed (or null if it didn't pass one).
    ; O1 is already o9 from the last MapObject argument; restore it
    ; explicitly in case future edits reorder things.
    omov  o1, o9
    omov  o3, o7

    ; JR into the module. No return.
    jr    r3
    nop

{p}fail:
    addiu r4, r0, 99
    call  #0x001                       ; TaskExit
    nop
""")
    return "".join(parts)


# ---------------------------------------------------------------------------
# Master (small, identical in both builds — but in the combined build it's
# placed in the same .text and reached via a PROCID branch).
# ---------------------------------------------------------------------------

def master_body(label_prefix: str = "") -> str:
    p = label_prefix
    return f"""\
{p}main:
    ; Save our data ref before the queue poll overwrites O3.
    omov  o7, o3                       ; o7 = our data segment ref

    ; Attach a queue to our self-service.
    omov  o1, o4
    addiu r4, r0, 4
    call  #0x203                       ; ReceiveQueueAttach
    bne   r2, r0, {p}fail
    nop

    ; Poll for the announce.
    omov  o1, o4
    addiu r4, r0, -1                   ; infinite
    call  #0x204                       ; ReceiveQueuePoll
    bne   r2, r0, {p}fail
    nop
    ; Now: O2 = loader's R+S self-ref;
    ;      R3 = loader's PROCID (informational).
    omov  o6, o2                       ; o6 = loader's self-ref

    ; Build the boot SEND.
    omov  o1, o6                       ; recipient = loader
    omov  o2, o7                       ; code source = our data segment
    omov  o3, o7                       ; module's data ref = same object
    onull o4
    addiu r4, r0, MODULE_BYTES
    addiu r5, r0, 0
    addiu r6, r0, 0
    addiu r7, r0, 0
    send  o1

    addiu r4, r0, 0
    call  #0x001                       ; TaskExit
    nop

{p}fail:
    addiu r4, r0, 1
    call  #0x001
    nop
"""


# ---------------------------------------------------------------------------
# Module bytes (hand-encoded; appears at offset 0 of master's .data).
# ---------------------------------------------------------------------------

MODULE_BYTES_DATA = """\
; Module image — the bytes the loader copies, MapObjects R|X, and JRs to.
; 32 B of code followed by 8 B of inline message; total 40 B = MODULE_BYTES.
; The module reads its message via O1 (= its own loaded code ref, set by
; the loader before JR). Hand-encoded per CONTRACT §5.
;
;   ; O1 is already the loaded code ref on entry; no setup needed.
;   nop
;   addiu r4, r0, 32          ; offset = 32 (past the module's own code)
;   addiu r5, r0, 8           ; count  = 8 ("Booted!\\n")
;   call  #0x320              ; ConsoleWrite (reads from O1, locally)
;   addiu r4, r0, 0           ; exit code 0
;   call  #0x001              ; TaskExit
;   nop                       ; CALL has no delay slot, but gives a clean fall-through
;   nop                       ; pad to 32 bytes
    .word 0x00000000                   ; nop
    .word 0x24040020                   ; addiu r4, r0, 32
    .word 0x24050008                   ; addiu r5, r0, 8
    .word 0xF4000320                   ; call  #0x320
    .word 0x24040000                   ; addiu r4, r0, 0
    .word 0xF4000001                   ; call  #0x001
    .word 0x00000000                   ; nop
    .word 0x00000000                   ; nop (pad)

; The message lives at offset 32 of THIS .data segment. The master sends
; MODULE_BYTES (40) so both code and message get copied to the loader.
    .ascii "Booted!\\n"
"""


# ---------------------------------------------------------------------------
# Top-level emitters
# ---------------------------------------------------------------------------

LOADER_HEADER = f"""\
; linkboot.s — generic link-boot loader.  GENERATED by gen_linkboot.py.
;
; A self-contained .orx that any extra CPU can be booted with. The
; loader announces itself to a "boot master" service at startup, then
; waits for the master to SEND it a code reference; on receipt, it
; copies the code into a fresh local code object, maps it executable,
; and JRs into the loaded module.
;
; The loader's own .orx carries no application code — only the boot
; logic. Useful when you want to spin up extra CPUs whose actual
; programs are decided at runtime (e.g. dynamic worker pools, demand-
; loaded service handlers).
;
; Conventions
; -----------
; Master location: by simorisc convention, when CPU N boots in an
; M-CPU configuration the lowest-PID other CPU's service ref lands
; in O5. The loader announces to whatever ref is in O5.
;
; Announce SEND payload (loader → master):
;     O2 = derived self-service (R+S only — enough for master to SEND)
;     R4 = loader's PROCID
;     R5 = 0  (announcement protocol version)
;
; Boot SEND payload (master → loader, queue-dispatched):
;     O2 = code reference (any byte-typed object with R cap)
;     O3 = optional data reference (becomes the loaded module's O3)
;     R4 = byte length of the module image (must be ≤ {MAX_WORDS * 4})
;     R5 = entry offset within the loaded image (0 = start)
;     R6, R7 = reserved (zero)
;
; On entry to the loaded module:
;     PC = mapped_va + R5
;     O3 = data ref (or whatever sender's O3 was; null if none)
;     O4 = self-service ref (preserved from boot)
;     O5 = master service (preserved)
;     O6, O7, O9 = clobbered (loader scratch)
;     other ORs = preserved if untouched at boot

.entry main

.set TAG_CODE,  0x4100
.set MAX_BYTES, {MAX_WORDS * 4}

.text

; ---------------------------------------------------------------------------
; main — announce, attach queue, poll, copy, derive, map, JR.
; Single straight-line task; no handler dispatch.
; ---------------------------------------------------------------------------
"""


DEMO_HEADER = f"""\
; demo.s — combined linkboot loader + master, single-orx.
; GENERATED by gen_linkboot.py.
;
; Branches on PROCID (R7) at entry: CPU 0 falls into the master, CPU
; 1+ falls into the loader. Designed for `simorisc --processors 2`
; runs (validation tests, smoke tests). For multi-process launches
; via oriscrun see linkboot.s + master.s individually.

.entry start

.set TAG_CODE,    0x4100
.set MAX_BYTES,   {MAX_WORDS * 4}
.set MODULE_BYTES, {MODULE_BYTES}

.text

start:
    bne   r7, r0, L_main               ; PROCID != 0 → loader
    nop
    j     M_main                       ; PROCID == 0 → master
    nop

; ---- Master (CPU 0) -------------------------------------------------------
"""


def standalone_loader() -> str:
    return LOADER_HEADER + loader_body(label_prefix="")


def standalone_master() -> str:
    return f"""\
; master.s — boot master that drives one linkboot CPU.
;
; Procedure:
;   1. Save our data ref (init_cpu put it in O3) — we need it after
;      the queue poll overlays O3 with the announce payload.
;   2. Attach a receive queue to our self-service.
;   3. Poll the queue infinitely. When the announce arrives:
;        O2 = loader's R+S self-ref
;        R3 = loader's PROCID
;   4. Build the boot SEND with code source = our data segment (the
;      module image lives at offset 0..31), data ref = same object
;      (the message lives at offset 32..39).
;   5. TaskExit. The loaded module on the receiver TaskExits with 0.

.entry main

.set MODULE_BYTES, {MODULE_BYTES}

.text

{master_body(label_prefix='')}

.data
{MODULE_BYTES_DATA}"""


def combined_demo(header: str = DEMO_HEADER) -> str:
    return (header
            + master_body(label_prefix="M_")
            + "\n; ---- Loader (CPU 1+) ------------------------------------------------------\n"
            + loader_body(label_prefix="L_")
            + "\n.data\n"
            + MODULE_BYTES_DATA)


VALIDATION_HEADER = f"""\
; @description: Generic link-boot loader. CPU 0 (master) waits for CPU 1's announce SEND, then SENDs back a 40-byte module image (8 instructions of code + an inline "Booted!\\n" message). CPU 1 runs the generated linkboot loader: derives a R+S self-ref, attaches a receive queue, announces itself, polls for the boot request, ObjAllocs a writable code object, copies the module bytes from the master via OLW (unrolled with early-exit), derives R|X|C, MapObjects executable, and JRs in. The loaded module ConsoleWrites the message via its own loaded code ref (in O1) and TaskExits.
; @processors: 2
; @expect-stdout: "Booted!\\n"
; @expect-exit: 0

; This file is GENERATED by examples/linkboot/gen_linkboot.py. Edit
; that script to change the loader; then re-run it to regenerate.

.entry start

.set TAG_CODE,    0x4100
.set MAX_BYTES,   {MAX_WORDS * 4}
.set MODULE_BYTES, {MODULE_BYTES}

.text

start:
    bne   r7, r0, L_main               ; PROCID != 0 → loader
    nop
    j     M_main                       ; PROCID == 0 → master
    nop

; ---- Master (CPU 0) -------------------------------------------------------
"""


def main() -> None:
    files = {
        HERE / "linkboot.s":                       standalone_loader(),
        HERE / "master.s":                         standalone_master(),
        HERE / "demo.s":                           combined_demo(),
        VALIDATION_DIR / "13_linkboot_loader.s":   combined_demo(VALIDATION_HEADER),
    }
    for path, content in files.items():
        path.write_text(content)
        print(f"wrote {path} ({len(content)} bytes)")


if __name__ == "__main__":
    main()
