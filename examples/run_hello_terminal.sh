#!/bin/sh
# run_hello_terminal.sh — assemble and run the hello-world-on-terminal demo.
#
# Spawns oriscbar (the crossbar process), oriscterm (a tkinter window
# presenting itself as a console device at pid=16), and a single
# simorisc CPU that SENDs "Hello, world!\n" to the terminal across the
# crossbar via the canonical RPC pattern (reply cap + receive queue).
#
# The terminal stays on screen for ~3 seconds after the CPU exits so
# you can see the result before everything tears down.

set -eu
cd "$(dirname "$0")/.."

python3 tools/asm/asmorisc examples/hello_terminal.s -o /tmp/hello_terminal.orx

exec python3 tools/oriscrun \
    --terminal pid=16 \
    --cpu "pid=0:program=/tmp/hello_terminal.orx,service=16=1@9" \
    --linger 3 \
    --leader-timeout 15
