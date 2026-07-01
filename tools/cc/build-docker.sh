#!/bin/sh
# build-docker.sh — stand up a Linux-container C front-end for the Object RISC
# toolchain and install shim binaries so run_c.sh and `make` compile through it
# transparently.
#
# WHAT: an OPT-IN, reproducible way to build + run the pcc C front-end (cpp, ccom)
# inside a Debian arm64 Linux container, shimmed so run_c.sh and `make` use it
# transparently. Only the two pcc binaries are containerized; the Python tools
# (asmorisc/orld/simorisc) run natively on the host.
#
# WHEN: you want a CI-style reproducible Linux toolchain, or you're on a host whose
# native toolchain can't build pcc. The DEFAULT path is native (tools/cc/build.sh) —
# pcc builds and runs fine on macOS, so this is a convenience, not a requirement.
#
# The container mounts the repo AND the macOS temp dirs (/var/folders, /private/tmp)
# at their NATIVE paths, so sources written to mktemp dirs (some device tests do this)
# are visible with no path rewriting — the shims are trivial passthroughs.
#
# Idempotent. Re-run after a reboot (it restarts the container + refreshes the shims;
# the pcc build inside the container persists across stop/start — only a `docker rm`
# forces the ~2-3 min rebuild).
#
#   tools/cc/build-docker.sh                          # set up (default PCC_BUILD=/tmp/pcc-build)
#   PCC_BUILD=/somewhere tools/cc/build-docker.sh     # shims elsewhere
#
# Requires: colima + docker  (brew install colima docker).

set -eu
export PATH="/opt/homebrew/bin:$PATH"

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PCC_BUILD="${PCC_BUILD:-/tmp/pcc-build}"
CONTAINER="${ORISC_CC_CONTAINER:-orisc-cc}"
IMAGE=debian:stable-slim
LPCC=/tmp/pb                                   # pcc build dir inside the container

if ! command -v colima >/dev/null 2>&1 || ! command -v docker >/dev/null 2>&1; then
    echo "error: colima and docker are required — run: brew install colima docker" >&2
    exit 1
fi

# 1) Linux VM up.
if ! colima status >/dev/null 2>&1; then
    echo "==> starting colima (Linux VM)..."
    colima start --cpu 4 --memory 4
fi

# 2) Container up. Mount repo + macOS temp dirs at native paths (colima shares them),
#    working dir = repo root so relative -I paths resolve as on the host.
if ! docker inspect "$CONTAINER" >/dev/null 2>&1; then
    echo "==> creating container '$CONTAINER' and installing build deps..."
    docker run -d --name "$CONTAINER" -v "$ROOT:$ROOT" -w "$ROOT" "$IMAGE" sleep infinity >/dev/null
    docker exec "$CONTAINER" sh -c \
        'export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y -qq build-essential bison flex >/dev/null 2>&1'
elif [ "$(docker inspect -f '{{.State.Running}}' "$CONTAINER")" != "true" ]; then
    echo "==> starting container '$CONTAINER'..."
    docker start "$CONTAINER" >/dev/null
fi

# 3) Build pcc inside the container (only if missing).
if ! docker exec "$CONTAINER" test -x "$LPCC/cc/cpp/orisc-unknown-none-cpp"; then
    echo "==> building pcc inside the container (one-time, ~2-3 min)..."
    docker exec "$CONTAINER" sh -c \
        "mkdir -p $LPCC && cd $LPCC && $ROOT/tools/cc/configure --target=orisc-unknown-none >/tmp/cfg.log 2>&1 && make >/tmp/mk.log 2>&1" \
        || { echo "pcc build FAILED:"; docker exec "$CONTAINER" tail -20 /tmp/mk.log; exit 1; }
fi

# 4) Install trivial passthrough shims at $PCC_BUILD (paths are native; cwd = repo root).
mkdir -p "$PCC_BUILD/cc/cpp" "$PCC_BUILD/cc/ccom"

cat > "$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" <<'SHIM'
#!/bin/bash
# cpp shim: run pcc's cpp in the container. A source file outside the mounted
# repo (e.g. a mktemp'd guest under /var/folders, which colima doesn't share) is
# staged into a repo dir the container can see. ccom needs none of this (stdin).
export PATH="/opt/homebrew/bin:$PATH"
ROOT="@@ROOT@@"; CONT="@@CONTAINER@@"; CPP="@@LPCC@@/cc/cpp/orisc-unknown-none-cpp"
STAGE="$ROOT/build/.shim-stage"
args=(); staged=()
for x in "$@"; do
  case "$x" in
    "$ROOT"/*|"$ROOT"|-*|"") args+=("$x") ;;                    # repo-absolute / flag: as-is
    /*) if [ -f "$x" ]; then mkdir -p "$STAGE"; s="$STAGE/$$-${x##*/}"; \
          cp "$x" "$s"; staged+=("$s"); args+=("$s"); else args+=("$x"); fi ;;  # outside repo
    *)  args+=("$x") ;;                                          # relative: resolves from -w
  esac
done
docker exec -i -w "$ROOT" "$CONT" "$CPP" "${args[@]}"
rc=$?
[ "${#staged[@]}" -gt 0 ] && rm -f "${staged[@]}"
exit $rc
SHIM

cat > "$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom" <<'SHIM'
#!/bin/bash
export PATH="/opt/homebrew/bin:$PATH"
exec docker exec -i @@CONTAINER@@ @@LPCC@@/cc/ccom/orisc-unknown-none-ccom "$@"
SHIM

sed -i '' -e "s|@@ROOT@@|$ROOT|g" -e "s|@@CONTAINER@@|$CONTAINER|g" -e "s|@@LPCC@@|$LPCC|g" \
    "$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" "$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"
chmod +x "$PCC_BUILD/cc/cpp/orisc-unknown-none-cpp" "$PCC_BUILD/cc/ccom/orisc-unknown-none-ccom"

echo "OK — cpp/ccom now route through the '$CONTAINER' Linux container."
echo "     PCC_BUILD=$PCC_BUILD ; run_c.sh and 'make' work transparently."
