#!/bin/sh
# test_hostfs.sh — end-to-end test of hostfsd + the host_io C library.
#
# Builds a tiny C program that opens a known file under hostfsd's
# jail, reads its contents, prints them, and exits. Asserts on the
# CPU's stdout.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f tools/cc/lib/liborisc.ora ]; then
    bash tools/cc/lib/build.sh >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# Make a fixture: a tiny file at a known path with deterministic content.
mkdir -p "$TMP/jail"
printf 'one\ntwo\nthree\n' > "$TMP/jail/test.txt"

# A C program that just opens, reads up to 256 bytes, prints, closes.
# Same shape as host_cat.c but with the path as a literal here so the
# test stands alone.
cat >"$TMP/test_prog.c" <<'CEOF'
#include "liborisc.h"

const char path_str[] = "test.txt";

int
main(void)
{
    register void *__or o2_stack       __asm__("o2");
    register void *__or o3_data        __asm__("o3");
    register void *__or o4_self        __asm__("o4");
    register void *__or o11_stack_save __asm__("o11");
    register void *__or o14_self_save  __asm__("o14");
    register void *__or o15_data_save  __asm__("o15");
    int fd, n, i;
    char buf[256];

    o11_stack_save = o2_stack;
    o14_self_save  = o4_self;
    o15_data_save  = o3_data;

    if (hf_init() != 0) { print_str("INIT-FAIL\n"); return 1; }

    fd = hf_open(path_str, HF_O_RDONLY);
    if (fd < 0) { print_str("OPEN-FAIL "); print_int(fd); print_str("\n"); return 2; }

    print_str("FD=");
    print_int(fd);
    print_str("\n");

    n = hf_read(fd, buf, sizeof(buf));
    if (n <= 0) { print_str("READ-FAIL "); print_int(n); print_str("\n"); return 3; }

    print_str("READ N=");
    print_int(n);
    print_str(":\n");
    for (i = 0; i < n; i++) print_char(buf[i]);

    hf_close(fd);
    print_str("END\n");
    return 0;
}
CEOF

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
"$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" \
    -I tools/cc/arch/orisc -I tools/cc/lib "$TMP/test_prog.c" > "$TMP/p.i"
"$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" < "$TMP/p.i" > "$TMP/p.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/console_io.oro"
python3 tools/asm/asmorisc -r "$TMP/p.s"                        -o "$TMP/p.oro"
python3 tools/ld/orld -o "$TMP/test.orx" \
    "$TMP/crt0.oro" "$TMP/console_io.oro" "$TMP/p.oro" \
    tools/cc/lib/liborisc.ora

SOCK="$TMP/oriscbar.sock"
python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/hostfsd \
    --socket "$SOCK" --pid 17 --root "$TMP/jail" -v \
    > "$TMP/hf.out" 2>&1 &
HF=$!
for _ in $(seq 50); do
    grep -q "hostfsd READY" "$TMP/hf.out" 2>/dev/null && break
    sleep 0.05
done

python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "0=0@0" --service "0=0@0" --service "17=1@9" \
    "$TMP/test.orx" >"$TMP/cpu.out" 2>"$TMP/cpu.err" &
CPU=$!

wait $CPU 2>/dev/null || true
kill -TERM $HF 2>/dev/null || true
wait $HF 2>/dev/null || true
kill -TERM $BAR 2>/dev/null || true
wait $BAR 2>/dev/null || true

echo "--- cpu0 stdout ---"
cat "$TMP/cpu.out"
echo "--- hostfsd log ---"
cat "$TMP/hf.out"

# Assertions.
grep -q "FD=0"    "$TMP/cpu.out" || { echo "FAIL: missing FD=0"   >&2; exit 1; }
grep -q "READ N=14" "$TMP/cpu.out" \
    || { echo "FAIL: expected READ N=14 (one\\ntwo\\nthree\\n is 14 bytes)" >&2; exit 1; }
grep -q "^one$"   "$TMP/cpu.out" || { echo "FAIL: missing 'one' line" >&2; exit 1; }
grep -q "^two$"   "$TMP/cpu.out" || { echo "FAIL: missing 'two' line" >&2; exit 1; }
grep -q "^three$" "$TMP/cpu.out" || { echo "FAIL: missing 'three' line" >&2; exit 1; }
grep -q "^END$"   "$TMP/cpu.out" || { echo "FAIL: missing END marker" >&2; exit 1; }

echo "PASS"
