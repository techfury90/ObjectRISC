; 06_otype.s — every O-type form, plus send and call.

.entry start

.text
start:
    omov   o2, o3
    onull  o4
    oeq    r10, o5, o6
    oisn   r11, o7
    olen   r12, o8
    otag   r13, o9
    ohome  r14, o10
    ocap   r15, o11
    olw    r4, 8(o2)
    osw    r5, -4(o3)
    send   o1
    call   #0x42
