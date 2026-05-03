#!/bin/sh
# run_hello_c.sh — build and run hello.c through the orisc pcc port.
#
# Pipeline:
#   cpp  -> ccom (the orisc-targeted pcc proper) -> asmorisc -> simorisc
#
# Expects pcc to have been configured for orisc and built — see
# tools/cc/arch/orisc/TODO. The build directory defaults to /tmp/pcc-build
# (override via PCC_BUILD env var).
#
# Output: "Hello, world!" on stdout, exit code 0.

set -eu
cd "$(dirname "$0")/../.."

PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CPP="$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
CCOM="$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

if [ ! -x "$CPP" ] || [ ! -x "$CCOM" ]; then
    cat >&2 <<EOF
error: pcc binaries not found in $PCC_BUILD
       (looked for: $CPP and $CCOM)

Build pcc for orisc once:
    mkdir -p /tmp/pcc-build && cd /tmp/pcc-build
    $PWD/tools/cc/configure --target=orisc-unknown-none
    make
EOF
    exit 1
fi

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

# 1. Preprocess and compile hello.c to assembly.
"$CPP"  examples/cc/hello.c > "$TMP/hello.i"
"$CCOM" < "$TMP/hello.i"     > "$TMP/hello.s"

# 2. Assemble crt0 + console_io bridge + program.
python3 tools/asm/asmorisc \
    tools/cc/arch/orisc/crt0.s \
    tools/cc/arch/orisc/console_io.s \
    "$TMP/hello.s" \
    -o "$TMP/hello.orx"

# 3. Run.
exec python3 tools/sim/simorisc "$TMP/hello.orx"
