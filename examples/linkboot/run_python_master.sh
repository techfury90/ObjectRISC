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
#         └── chunked boot image ── OLW ── via bus ──┘
#
# linkbootd preloads a module .orx and serves it 256 bytes at a time
# to whichever loader CPU(s) announce. Loaders run chunkboot.s
# (the chunked-protocol loader); they ObjAlloc a writable code
# object, copy chunks into it, then CALL InstallProgram (firmware
# primitive #0x111) to map the result at CODE_VA and jump in.
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

# Regenerate the chunked-boot loader (what spare-CPU slots run).
python3 examples/linkboot/gen_chunkboot.py >/dev/null

# A tiny standalone module program — `module.orx` — that linkbootd
# preloads and serves. The module reads its message via O1 (= its
# own loaded code ref, set by InstallProgram) at offset 32 — the
# "Booted!\n" bytes immediately follow the code in .text.
cat >"$TMP/module.s" <<'EOF'
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

# Build chunkboot.s once.
python3 tools/asm/asmorisc examples/linkboot/chunkboot.s -o "$TMP/chunkboot.orx"

# 1) Crossbar.
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR_PID=$!
# Spin until the socket exists.
for _ in $(seq 50); do
    [ -S "$SOCK" ] && break
    sleep 0.05
done

# 2) Boot server. With --image preload + the loader-pids list,
# linkbootd queues module.orx and serves it (chunked) to the first
# announcing loader, with no completion notification.
LOADER_PIDS=""
for i in $(seq 1 "$NCPUS"); do
    [ -n "$LOADER_PIDS" ] && LOADER_PIDS="$LOADER_PIDS,"
    LOADER_PIDS="${LOADER_PIDS}$i"
done
python3 tools/devices/linkbootd \
    --socket "$SOCK" --pid 0 \
    --loader-pids "$LOADER_PIDS" \
    --image "$TMP/module.orx" -v \
    >"$TMP/lbd.out" 2>"$TMP/lbd.err" &
LBD_PID=$!
# Wait for "linkbootd READY" on stdout.
for _ in $(seq 50); do
    grep -q "linkbootd READY" "$TMP/lbd.out" 2>/dev/null && break
    sleep 0.05
done

# 3) Loader CPUs (one or more). Each gets --service 0=1@0x09 so its
# O5 points at linkbootd's service ref. (The chunked loader uses O5
# as its master and reserves O6..O11 as slots passed through to the
# guest after a pre-jump shift; for this demo the guest doesn't use
# any device refs, so we leave those slots empty.)
CPU_PIDS=""
for i in $(seq 1 "$NCPUS"); do
    python3 tools/sim/simorisc \
        --connect "$SOCK" --pid "$i" \
        --service "0=1@0x09" \
        "$TMP/chunkboot.orx" \
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
