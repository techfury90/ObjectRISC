#!/bin/sh
# test_oref_calls.sh — regression for the Object RISC `__or` calling
# convention / OREFTY base type (toolchain Phase 1B).
#
# Compile-only: examples/cc/oref_calls.c exercises capability params,
# returns, null, OR-returning calls, and a named OR local. These never
# mint an OR at runtime (that needs the Phase 2 intrinsics), so we
# assert the BACKEND lowers them correctly: it compiles with no errors,
# emits OR-file moves (omov / onull / o1), and never spills a capability
# to byte memory (no `sw oN, ...`).

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

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

"$CPP" -I tools/cc/arch/orisc -I tools/cc/lib examples/cc/oref_calls.c \
    > "$TMP/p.i" 2>/dev/null
if ! "$CCOM" < "$TMP/p.i" > "$TMP/p.s" 2>"$TMP/p.err"; then
    echo "FAIL: ccom errored on oref_calls.c" >&2
    cat "$TMP/p.err" >&2
    exit 1
fi
if grep -qi "error" "$TMP/p.err"; then
    echo "FAIL: compile diagnostics" >&2; cat "$TMP/p.err" >&2; exit 1
fi

# Capabilities must move via the OR file, never spill to byte memory.
if grep -Eq '	sw	*o[0-9]' "$TMP/p.s"; then
    echo "FAIL: a capability was stored to byte memory (sw oN)" >&2
    grep -E '	sw	*o[0-9]' "$TMP/p.s" >&2
    exit 1
fi

# Sanity: the OR-file ops we expect are present (onull for the null
# capability, omov for the forwarding move, o1 as the return reg).
for pat in 'onull o1' 'omov' 'o1'; do
    if ! grep -q "$pat" "$TMP/p.s"; then
        echo "FAIL: expected '$pat' in generated code" >&2
        exit 1
    fi
done

echo "PASS"
