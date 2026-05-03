; 02_hello.s — copy of examples/hello.s for the test runner.

.entry main

.text
main:
    omov  o1, o3
    addiu r4, r0, 0
    addiu r5, r0, 14
    call  #0x320

    addiu r4, r0, 0
    call  #0x001
    nop

.data
hello_str:
    .string "Hello, world!\n"
