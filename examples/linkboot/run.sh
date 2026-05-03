#!/bin/sh
# run.sh — multi-process linkboot demo.
#
# Spawns oriscbar + two simorisc CPUs:
#   pid 0: master.orx     — sends boot request when loader announces
#   pid 1: linkboot.orx   — generic loader, receives module, JRs in
#
# The loader's view of the master goes via --service: pid 0, idx 4
# (master allocates code=1, stack=2, data=3, service=4 — see
# tools/sim/simorisc::populate_self_service), R+S caps = 0x09.
#
# CPU 1 is the leader — oriscrun waits for the loaded module to exit
# before tearing the system down.

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

# Regenerate (cheap, deterministic) so the loader/master/demo bundle
# stays in lockstep with gen_linkboot.py.
python3 examples/linkboot/gen_linkboot.py >/dev/null

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

python3 tools/asm/asmorisc examples/linkboot/master.s   -o "$TMP/master.orx"
python3 tools/asm/asmorisc examples/linkboot/linkboot.s -o "$TMP/linkboot.orx"

# Master uses ,serve so its simorisc keeps the crossbar connection open
# even after its single SEND. Without that, master exits, the crossbar
# tears down its pid=0 connection, and any retry-announce from the
# loader (we have a finite-timeout retry loop in linkboot.s) gets
# dropped because there's no longer a destination.
exec python3 tools/oriscrun \
    --cpu pid=0:program="$TMP/master.orx",serve \
    --cpu "pid=1:program=$TMP/linkboot.orx,service=0=4@9" \
    --leader 1
