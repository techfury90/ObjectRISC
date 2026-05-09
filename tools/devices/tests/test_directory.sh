#!/bin/sh
# test_directory.sh — Phase 45f: register / walk / list round-trip
# against a live oriscdir daemon.
#
# Builds a tiny test program that:
#   1. registers a leaf  (using its own O4 service ref as the value)
#   2. walks the leaf back; asserts kind=LEAF
#   3. mounts another path with the same ref and a prefix
#   4. walks /mountpoint/sub/leaf; asserts kind=MOUNT, remainder
#      is /myprefix/sub/leaf
#   5. lists the parent directory; asserts the registered names
#      appear
#   6. walks a non-existent path; asserts ENOENT
#
# Each failure exits with a distinct status code so the harness
# can pinpoint where things broke.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

if [ ! -f build/liborisc.ora ]; then
    make -s lib >/dev/null
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

cat > "$TMP/dirtest.c" <<'EOF'
#include "liborisc.h"

/* Helper: pcc's orisc backend gets confused by long main() bodies
 * (manifests as "adrput: illegal op 57"). Splitting into helpers
 * keeps each function small enough that the register allocator
 * stays happy. */

static int
do_register(void)
{
    int rc;
    asm volatile("orefld o1, 64(o12)");   /* O1 = saved self-service */
    rc = dir_register("/test/leaf");
    if (rc != 0) return 1;
    return 0;
}

static int
do_walk_leaf(void)
{
    int kind;
    char rem[64];
    int rc = dir_walk("/test/leaf", &kind, rem, 64);
    if (rc != 0) return 2;
    if (kind != DIR_KIND_LEAF) return 3;
    return 0;
}

static int
do_mount(void)
{
    int rc;
    asm volatile("orefld o1, 64(o12)");
    rc = dir_mount("/mnt", "/myprefix");
    if (rc != 0) return 4;
    return 0;
}

static int
do_walk_mount(void)
{
    int kind;
    char rem[64];
    int rc = dir_walk("/mnt/sub/leaf", &kind, rem, 64);
    /* expected remainder: "/myprefix/sub/leaf"
     *                      0    5    10   15
     *                      / m y p r e f i x / s u b / l e a f */
    if (rc != 18) return 5;
    if (kind != DIR_KIND_MOUNT) return 6;
    if (rem[0]  != '/') return 7;
    if (rem[1]  != 'm') return 7;
    if (rem[9]  != '/') return 8;
    if (rem[10] != 's') return 8;
    if (rem[13] != '/') return 9;
    if (rem[17] != 'f') return 9;
    return 0;
}

static int
do_list(void)
{
    char list_buf[64];
    int count = dir_list("/test", list_buf, 64);
    if (count != 1) return 10;
    if (list_buf[0] != 'l') return 11;
    return 0;
}

static int
do_enoent(void)
{
    int kind;
    char rem[64];
    int rc = dir_walk("/nope/here", &kind, rem, 64);
    if (rc != -2) return 12;
    return 0;
}

int
main(void)
{
    int rc;
    task_init();

    /* Publish BOOT_PARENT_SLOT (= directory mailbox in this
     * harness; oriscrun wired it into our boot O8) into DIR_SLOT
     * directly — the test's parent isn't a supervisor so dir.c's
     * lazy "ask my parent" path doesn't apply. */
    asm volatile(
        "orefld o1, 544(o12)\n"
        "orefst o1, 584(o12)"
        : : : "r1"
    );

    /* Save self-service ref (O4) into an unused OR-store slot
     * because dir_*() internally calls ReceiveQueuePoll, which
     * fills O1..O4 with reply payload — wiping O4 in the process.
     * Park into task-table slot 8 (offset 64, currently unused
     * since this test never task_spawns). */
    asm volatile("orefst o4, 64(o12)");

    /* Each helper reloads O1 = saved self-service via OREFLD before
     * any dir_register/mount call that needs a ref to publish. */
    rc = do_register();
    if (rc != 0) return rc;
    rc = do_walk_leaf();
    if (rc != 0) return rc;
    rc = do_mount();
    if (rc != 0) return rc;
    rc = do_walk_mount();
    if (rc != 0) return rc;
    rc = do_list();
    if (rc != 0) return rc;
    rc = do_enoent();
    if (rc != 0) return rc;

    return 0;
}
EOF

"$CPP"  -I tools/cc/arch/orisc -I tools/cc/lib "$TMP/dirtest.c" > "$TMP/dirtest.i"
"$CCOM" < "$TMP/dirtest.i" > "$TMP/dirtest.s"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/crt0.s        -o "$TMP/crt0.oro"
python3 tools/asm/asmorisc -r tools/cc/arch/orisc/console_io.s  -o "$TMP/cio.oro"
python3 tools/asm/asmorisc -r "$TMP/dirtest.s"                  -o "$TMP/dirtest.oro"
python3 tools/ld/orld -o "$TMP/dirtest.orx" \
    "$TMP/crt0.oro" "$TMP/cio.oro" "$TMP/dirtest.oro" \
    build/liborisc.ora

SOCK="$TMP/oriscbar.sock"

python3 tools/sim/oriscbar --socket "$SOCK" >/dev/null 2>&1 &
BAR=$!
for _ in $(seq 50); do [ -S "$SOCK" ] && break; sleep 0.05; done

python3 tools/devices/oriscdir \
    --socket "$SOCK" --pid 18 -v \
    --config tools/devices/oriscdir.default.conf \
    > "$TMP/dir.out" 2>&1 &
DIR=$!
for _ in $(seq 50); do
    grep -q "oriscdir READY" "$TMP/dir.out" 2>/dev/null && break
    sleep 0.05
done

# Wire the directory's mailbox sub-cap into O8 of the test CPU.
# The daemon's primary mailbox is at descriptor index 1, generation
# 1 (its first allocation). We synthesize "PID=1@9" for it.
set +e
python3 tools/sim/simorisc --connect "$SOCK" --pid 0 \
    --service "0=0@0" --service "0=0@0" --service "0=0@0" \
    --service "18=1@9" --service "0=0@0" --service "0=0@0" \
    "$TMP/dirtest.orx" >"$TMP/cpu0.out" 2>"$TMP/cpu0.err"
RC=$?
set -e

kill -KILL $DIR 2>/dev/null || true
kill -KILL $BAR 2>/dev/null || true
wait $DIR 2>/dev/null || true
wait $BAR 2>/dev/null || true

echo "--- oriscdir log ---"
cat "$TMP/dir.out"
echo "--- cpu0 stdout ---"
cat "$TMP/cpu0.out"
echo "--- cpu0 stderr ---"
cat "$TMP/cpu0.err"

if [ "$RC" -eq 0 ]; then
    echo "PASS"
else
    echo "FAIL (test exit code $RC — see comments in dirtest.c for the meaning)" >&2
    exit 1
fi
