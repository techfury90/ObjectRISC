#!/bin/sh
# run_python_master.sh — multi-process linkboot demo with linkbootd
# (the Python-side boot server) standing in for the asm master.
#
# Architecture:
#
#     ┌──────────┐        ┌────────────┐        ┌──────────┐
#     │ linkbootd│◀──────▶│  oriscbar  │◀──────▶│ simorisc │
#     │ (pid 0)  │        │ (crossbar) │        │ (pid 1+) │
#     └──────────┘        └────────────┘        └──────────┘
#         │                                          │
#         └─── boot image ──── OLW ──── over wire ───┘
#
# linkbootd hosts a pre-built module .orx and synthesizes a
# (home=0, idx=0x100) reference for it. Loader CPUs announce to
# whatever lives in their O5; we install (home=0, idx=0x100, R|S)
# there via --service. linkbootd replies to each announce with a
# boot SEND carrying the image_ref + length + entry.
#
# Run with NCPUS environment variable to spawn more loaders:
#     NCPUS=4 examples/linkboot/run_python_master.sh

set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

NCPUS="${NCPUS:-1}"
TMP=$(mktemp -d)
# macOS mktemp doesn't expand X's mid-template — put the socket inside
# the per-run TMP dir to guarantee a fresh path every invocation.
SOCK="$TMP/oriscbar.sock"

cleanup() {
    [ -n "${BAR_PID:-}" ] && kill -TERM "$BAR_PID" 2>/dev/null || true
    [ -n "${LBD_PID:-}" ] && kill -TERM "$LBD_PID" 2>/dev/null || true
    rm -rf "$TMP" "$SOCK" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Regenerate loader/master/demo to stay in lockstep with gen_linkboot.py.
python3 examples/linkboot/gen_linkboot.py >/dev/null

# We need an .orx that the boot server can ship. The combined demo.s
# carries the module bytes in its .data segment (offsets 0..40), but
# the .orx text-section extraction would skip that. Build a tiny
# standalone module program — `module.orx` — and let linkbootd serve
# its text section.
cat >"$TMP/module.s" <<'EOF'
; Tiny boot-payload module: print "Booted!\n" and TaskExit.
; Reads the message from O1 (= its own loaded code ref, set by linkboot.s
; before JR), at offset 32 — the message immediately follows the code.

.entry main
.text
main:
    nop                          ; placeholder; O1 is already the code ref
    addiu r4, r0, 32             ; offset = 32 (past the module's own code)
    addiu r5, r0, 8              ; count  = 8 ("Booted!\n")
    call  #0x320                 ; ConsoleWrite (reads from O1, locally)
    addiu r4, r0, 0              ; exit code 0
    call  #0x001                 ; TaskExit
    nop
    nop                          ; pad to 32 bytes

    ; The message lives in .text right after the code so it gets
    ; bundled in linkbootd's image (.orx text section is what we serve).
    .ascii "Booted!\n"
EOF
python3 tools/asm/asmorisc "$TMP/module.s" -o "$TMP/module.orx"

# Build linkboot.s once.
python3 tools/asm/asmorisc examples/linkboot/linkboot.s -o "$TMP/linkboot.orx"

# 1) Crossbar.
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR_PID=$!
# Spin until the socket exists.
for _ in $(seq 50); do
    [ -S "$SOCK" ] && break
    sleep 0.05
done

# 2) Boot server.
python3 tools/devices/linkbootd \
    --socket "$SOCK" --pid 0 --image "$TMP/module.orx" -v \
    >"$TMP/lbd.out" 2>"$TMP/lbd.err" &
LBD_PID=$!
# Wait for "linkbootd READY" on stdout.
for _ in $(seq 50); do
    grep -q "linkbootd READY" "$TMP/lbd.out" 2>/dev/null && break
    sleep 0.05
done

# 3) Loader CPUs (one or more). Each gets pid=N, --service 0=0x100@0x09
# so its O5 points at linkbootd's hosted-image ref.
CPU_PIDS=""
for i in $(seq 1 "$NCPUS"); do
    python3 tools/sim/simorisc \
        --connect "$SOCK" --pid "$i" \
        --service "0=256@0x09" \
        "$TMP/linkboot.orx" \
        >"$TMP/cpu$i.out" 2>"$TMP/cpu$i.err" &
    CPU_PIDS="$CPU_PIDS $!"
done

# Wait for each loader explicitly. With `set -e` a non-zero exit from
# `wait` would kill the script before we print outputs, so swallow.
for p in $CPU_PIDS; do
    wait "$p" 2>/dev/null || true
done

# Print each CPU's stdout, one prefixed line per output line.
for i in $(seq 1 "$NCPUS"); do
    if [ -s "$TMP/cpu$i.out" ]; then
        sed "s/^/[cpu$i] /" "$TMP/cpu$i.out"
    fi
done

# When DEBUG=1, also surface each process's stderr (linkbootd is -v so
# its log lands on stderr; simorisc emits trap reports there too).
if [ "${DEBUG:-0}" = "1" ]; then
    echo "--- linkbootd stderr ---"
    cat "$TMP/lbd.err" 2>/dev/null || true
    for i in $(seq 1 "$NCPUS"); do
        echo "--- cpu$i stderr ---"
        cat "$TMP/cpu$i.err" 2>/dev/null || true
    done
fi
