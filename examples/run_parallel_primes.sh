#!/bin/sh
# run_parallel_primes.sh — assemble and run the parallel-prime-counting demo.
#
# Spawns oriscbar (the crossbar), oriscterm (a tkinter terminal at pid=16),
# and four simorisc CPU processes (pids 0..3). CPU 0 is the coordinator;
# CPUs 1..3 are workers that install a SEND handler and idle in --serve
# mode until the coordinator dispatches work to them. Each worker counts
# primes in its quarter of [2..2000], samples ReadCycles before and after,
# and SENDs (count, elapsed_cycles) back through a reply cap. The
# coordinator streams formatted result lines to the terminal as each reply
# arrives, then prints the total.
#
# Worker service-object indices are 4 (after init_cpu allocates code,
# stack, and data). The terminal's console object is hardcoded at index 1.

set -eu
cd "$(dirname "$0")/.."

python3 tools/asm/asmorisc examples/parallel_primes.s -o /tmp/parallel_primes.orx

exec python3 tools/oriscrun \
    --terminal pid=16 \
    --cpu "pid=0:program=/tmp/parallel_primes.orx,service=16=1@9,service=1=4@9,service=2=4@9,service=3=4@9" \
    --cpu "pid=1:program=/tmp/parallel_primes.orx,serve" \
    --cpu "pid=2:program=/tmp/parallel_primes.orx,serve" \
    --cpu "pid=3:program=/tmp/parallel_primes.orx,serve" \
    --linger 3 \
    --leader-timeout 60
