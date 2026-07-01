#!/bin/sh
# test_oref_spill.sh — regression for `__or` autos/params as per-frame
# OBJSTORE-memory variables (toolchain Phase 2), plus the Phase-4 unblock
# of capability *call results* held across a call.
#
# Three checks:
#  1. RUNTIME: examples/cc/oref_spill.c homes an `__or` parameter in the
#     per-frame OBJSTORE and reloads it across a clobbering call; main()
#     returns 42 iff the capability survived (prologue ObjAllocStore +
#     O12-anchor chain, OREFLD/OREFST homing, epilogue ObjFree).
#  2. RUNTIME: examples/cc/oref_callresult.c stores a capability *call
#     result* into an `__or` home and holds it across a further clobbering
#     call, then returns it. This used to be REJECTED at compile time (the
#     OREFST scratch spilled a capability under false cross-call OR
#     pressure); macdefs.h's PRUNE_CALLLIVE removes that false pressure, so
#     it now compiles and — this check proves — runs correctly (returns 42).
#  3. COMPILE: the previously-rejected pattern (`void *__or o = alloc();
#     use(o); return use(o);`) now compiles cleanly and never spills a
#     capability to byte memory (no `sw oN, ...`).

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

# run_c.sh's cpp can crash intermittently on some hosts (SIGBUS, exit 138);
# retry a handful of times so a flaky preprocessor doesn't red the suite.
run_c_expect42() {
    _src="$1"
    _i=0
    _rc=0
    while [ "$_i" -lt 8 ]; do
        _rc=0
        examples/cc/run_c.sh "$_src" >/dev/null 2>&1 || _rc=$?
        [ "$_rc" != "138" ] && break   # 138 == cpp SIGBUS; retry
        _i=$((_i + 1))
    done
    echo "$_rc"
}

# --- 1. runtime: capability param survives a clobbering call ---
rc=$(run_c_expect42 examples/cc/oref_spill.c)
if [ "$rc" != "42" ]; then
    echo "FAIL: oref_spill.c returned $rc, expected 42" >&2
    exit 1
fi

# --- 2. runtime: capability call result survives, held across a call ---
rc=$(run_c_expect42 examples/cc/oref_callresult.c)
if [ "$rc" != "42" ]; then
    echo "FAIL: oref_callresult.c returned $rc, expected 42" >&2
    exit 1
fi

# --- 3. compile: storing an __or call result now works (no capability
#        byte-spill). Retry cpp on the intermittent SIGBUS. ---
TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cat > "$TMP/callstore.c" <<'EOF'
extern void *__or alloc_obj(void);
extern int use_obj(void *__or);
int f(void){ void *__or o = alloc_obj(); use_obj(o); return use_obj(o); }
EOF
i=0
while [ "$i" -lt 8 ]; do
    if "$CPP" -I tools/cc/arch/orisc -I tools/cc/lib "$TMP/callstore.c" \
        > "$TMP/callstore.i" 2>/dev/null; then
        break
    fi
    i=$((i + 1))
done
if ! "$CCOM" < "$TMP/callstore.i" > "$TMP/callstore.s" 2>"$TMP/callstore.err"; then
    echo "FAIL: storing an __or call result should now compile, but ccom errored:" >&2
    cat "$TMP/callstore.err" >&2
    exit 1
fi
if grep -Eq '	sw	*o[0-9]' "$TMP/callstore.s"; then
    echo "FAIL: a capability was spilled to byte memory (sw oN)" >&2
    grep -E '	sw	*o[0-9]' "$TMP/callstore.s" >&2
    exit 1
fi

echo "PASS"
