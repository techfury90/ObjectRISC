; hello.s — Object RISC hello world
;
; The host loader sets up the initial task with:
;   O1 = code object (this program), R+X
;   O2 = stack object, R+W
;   O3 = data object containing "Hello, world!\n", R
;   SP = top of stack region
;   PC = entry point named by .entry directive
;
; The program writes the contents of the data object to the console
; and exits cleanly.

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
