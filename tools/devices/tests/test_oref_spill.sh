#!/bin/sh
# test_oref_spill.sh — regression for `__or` autos/params as per-frame
# OBJSTORE-memory variables (toolchain Phase 2).
#
# Two checks:
#  1. RUNTIME: examples/cc/oref_spill.c homes an `__or` parameter in the
#     per-frame OBJSTORE and reloads it across a clobbering call; main()
#     returns 42 iff the capability survived (prologue ObjAllocStore +
#     O12-anchor chain, OREFLD/OREFST homing, epilogue ObjFree).
#  2. COMPILE-REJECT: storing a capability *call result* directly into an
#     `__or` local is a known v1 limitation and must fail loudly (a clean
#     uerror, not broken codegen), so callers get a clear workaround.

set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT"

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    echo "SKIP: pcc not built at $PCC_BUILD (run tools/cc/build.sh)" >&2
    exit 0
fi

# --- 1. runtime: capability param survives a clobbering call ---
rc=0
examples/cc/run_c.sh examples/cc/oref_spill.c >/dev/null 2>&1 || rc=$?
if [ "$rc" != "42" ]; then
    echo "FAIL: oref_spill.c returned $rc, expected 42" >&2
    exit 1
fi

# --- 2. compile-reject: storing an __or call result into a local ---
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cat > "$TMP/badstore.c" <<'EOF'
extern void *__or alloc_obj(void);
extern int use_obj(void *__or);
int f(void){ void *__or o = alloc_obj(); use_obj(o); return use_obj(o); }
EOF
"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib "$TMP/badstore.c" \
    > "$TMP/badstore.i" 2>/dev/null
if "$CCOM" < "$TMP/badstore.i" > "$TMP/badstore.s" 2>"$TMP/badstore.err"; then
    echo "FAIL: storing an __or call result should be rejected (v1 limit)" >&2
    exit 1
fi
if ! grep -qi "not yet supported" "$TMP/badstore.err"; then
    echo "FAIL: expected a clear 'not yet supported' diagnostic, got:" >&2
    cat "$TMP/badstore.err" >&2
    exit 1
fi

echo "PASS"
