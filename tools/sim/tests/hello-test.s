; hello-test.s — reference assembly for the hand-encoded hello-test.orx.
;
; This is the EXACT program in CONTRACT.md Section 7. We do NOT depend on
; the assembler to produce it; build-hello-test.py emits the bytes
; directly from explicit instruction-word constants. This file is kept
; alongside as documentation.
;
; The host loader sets up the initial task with:
;   O1 = code object (this program), R+X
;   O2 = stack object, R+W
;   O3 = data object containing "Hello, world!\n", R
;   SP = top of stack region
;   PC = entry point (= "main")
;
; Program writes the contents of the data object to the console and
; exits cleanly.

.entry main

.text
main:
    omov  o1, o3            ; ConsoleWrite arg 1: source = data object
    addiu r4, r0, 0         ; offset = 0
    addiu r5, r0, 14        ; count = 14 (length of "Hello, world!\n")
    call  #0x320            ; ConsoleWrite

    addiu r4, r0, 0         ; exit code = 0
    call  #0x001            ; TaskExit
    nop                     ; (unreachable)

.data
hello_str:
    .string "Hello, world!\n"
