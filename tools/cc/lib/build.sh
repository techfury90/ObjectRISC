#!/bin/sh
# build.sh — compatibility wrapper around `make lib`.
#
# The canonical build entry point is the top-level Makefile; this
# script exists so old muscle memory and stale scripts that call
# `bash tools/cc/lib/build.sh` keep working. It delegates to
# `make lib`, which now produces build/liborisc.ora (not the old
# in-tree tools/cc/lib/liborisc.ora) — so post-build artefact
# location moved. Update callers when convenient.

set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
exec make lib "$@"
