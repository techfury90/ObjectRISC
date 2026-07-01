#!/bin/sh
# build.sh — one-shot bootstrap for the in-tree pcc port.
#
# Builds the Object RISC C compiler (`orisc-unknown-none-cpp` and
# `orisc-unknown-none-ccom`) into ${PCC_BUILD:-/tmp/pcc-build}.
# Re-run when you change anything under tools/cc/arch/orisc/ — pcc's
# Makefile picks up object-file invalidations correctly so subsequent
# builds are fast.
#
# Run from anywhere; the script self-locates the source tree.
#
# Native is the default and builds/runs fine on macOS. For a reproducible,
# CI-style Linux toolchain — or on a host whose native toolchain can't build
# pcc — see tools/cc/build-docker.sh, which builds cpp/ccom in a Debian
# container and shims them so run_c.sh and `make` work transparently.

set -eu

CC_DIR=$(cd "$(dirname "$0")" && pwd)
PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"

mkdir -p "$PCC_BUILD"
cd "$PCC_BUILD"

if [ ! -f config.status ]; then
    "$CC_DIR/configure" --target=orisc-unknown-none
fi
make

echo
echo "pcc built into $PCC_BUILD"
echo "  cpp:  $PCC_BUILD/cc/cpp/orisc-unknown-none-cpp"
echo "  ccom: $PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"
echo
echo "examples/cc/run_c.sh expects PCC_BUILD=$PCC_BUILD by default."
