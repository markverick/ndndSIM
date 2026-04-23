#!/usr/bin/env bash
# ndndSIM/go/build.sh
# 
# Build the ndndSIM Go simulation library against a CLEAN upstream ndnd.
#
# The script:
#   1. Checks out a clean copy of ndnd (pristine upstream by default).
#      Uses a local git worktree by default, or clones from NDND_GIT_URL
#      if that variable is set.
#   2. Runs the AST transformer to patch the copy and apply overlay files.
#      The overlay/ directory contains sim/ and all sim-specific additions
#      to ndnd core packages.
#      For onephase and twophase builds, overlay-op/ and overlay-tw/ provide
#      phase-specific net-new files only.
#   3. Generates a go.work that ties together ndndsim, the transformer, and
#      the transformed ndnd (which now includes sim/).
#   4. Builds the CGo simulation library and runs sim tests.
#
# Environment variables (override defaults):
#   NDND_PHASE    – twophase (default) or onephase
#   NDND_SRC      – path to the local ndnd git repository (for worktree mode)
#   NDND_HASH     – git ref to check out from NDND_SRC
#   NDND_GIT_URL  – if set, clone from this URL instead of using NDND_SRC
#   NDND_GIT_BRANCH – branch to clone when NDND_GIT_URL is set
#   OUT_LIB       – output path for the compiled .a archive
#   GO            – Go binary to use
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------- phase selection ----------
NDND_PHASE="${NDND_PHASE:-twophase}"

# ---------- phase-specific defaults ----------
if [[ "$NDND_PHASE" == "onephase" ]]; then
    # named-data/ndnd@main at the commit where dv2 branched off.
    NDND_HASH="${NDND_HASH:-51774b8}"
    NDND_GIT_BRANCH="${NDND_GIT_BRANCH:-main}"
else
    # named-data/ndnd@dv2 — pristine upstream dv2 commit.
    NDND_HASH="${NDND_HASH:-76aeb89c}"
    NDND_GIT_BRANCH="${NDND_GIT_BRANCH:-dv2}"
fi

NDND_SRC="${NDND_SRC:-${SCRIPT_DIR}/../ndnd}"
NDND_GIT_URL="${NDND_GIT_URL:-}"
# Phase-specific output paths so two cmake builds can coexist in the same tree.
OUT_LIB="${OUT_LIB:-${SCRIPT_DIR}/libndndsim-${NDND_PHASE}.a}"
TRANSFORMED_DIR="${SCRIPT_DIR}/.transformed-ndnd-${NDND_PHASE}"

# Locate Go binary: prefer explicit $GO, then a toolchain downloaded into
# GOPATH, then a toolchain in /usr/local/go, then whatever is on PATH.
if [[ -z "${GO:-}" || ! -x "${GO:-}" ]]; then
    _GOPATH="${GOPATH:-${HOME}/go}"
    GO="$(ls "${_GOPATH}"/pkg/mod/golang.org/toolchain@v0.0.1-go1.24.*.linux-amd64/bin/go 2>/dev/null | sort -V | tail -1)"
fi
if [[ -z "${GO:-}" || ! -x "${GO:-}" ]]; then
    GO="/usr/local/go/bin/go"
fi
if [[ -z "${GO:-}" || ! -x "${GO:-}" ]]; then
    GO="$(command -v go)"
fi

copy_overlay_additions() {
    local src_dir="$1"
    local dst_dir="$2"
    local label="$3"
    local -a collisions=()

    [[ -d "$src_dir" ]] || return 0

    while IFS= read -r -d '' src; do
        local rel="${src#$src_dir/}"
        local dst="$dst_dir/$rel"
        if [[ -e "$dst" ]]; then
            collisions+=("$rel")
        fi
    done < <(find "$src_dir" -type f -print0 | sort -z)

    if (( ${#collisions[@]} > 0 )); then
        {
            echo "overlay collision: ${label} would replace transformed upstream files:"
            for rel in "${collisions[@]}"; do
                echo "  - $rel"
            done
            echo "move these patches into the transformer instead of ${label}/"
        } >&2
        return 1
    fi

    while IFS= read -r -d '' src; do
        local rel="${src#$src_dir/}"
        local dst="$dst_dir/$rel"
        mkdir -p "$(dirname "$dst")"
        echo "  ${label}: $rel"
        cp "$src" "$dst"
    done < <(find "$src_dir" -type f -print0 | sort -z)
}

# ---------- step 1: clean worktree ----------
WORK_DIR="$(mktemp -d)"

if [ -n "$NDND_GIT_URL" ]; then
    echo "==> Cloning ${NDND_GIT_URL} (branch: ${NDND_GIT_BRANCH})"
    trap 'rm -rf "$WORK_DIR"' EXIT
    git clone --depth 1 --branch "$NDND_GIT_BRANCH" "$NDND_GIT_URL" "$WORK_DIR"
else
    echo "==> Using clean ndnd at ${NDND_HASH} (local worktree)"
    trap 'git -C "$NDND_SRC" worktree remove --force "$WORK_DIR" 2>/dev/null; rm -rf "$WORK_DIR"' EXIT
    git -C "$NDND_SRC" worktree add --detach "$WORK_DIR" "$NDND_HASH"
fi

# ---------- step 2: transform + overlay ----------
# The overlay/ directory provides only net-new files such as the sim/ package.
# Any patch to an upstream ndnd file must live in the transformer so the build
# never silently replaces pristine sources.
echo "==> Transforming ndnd source and applying overlay (phase: ${NDND_PHASE})"
cd "${SCRIPT_DIR}/transform"
GOWORK=off ${GO} run . \
    --phase "$NDND_PHASE" \
    --src  "$WORK_DIR" \
    --out  "$TRANSFORMED_DIR" \
    --overlay "${SCRIPT_DIR}/overlay" \
    --sim-module "github.com/named-data/ndndsim" \
    --sim-module-dir "${SCRIPT_DIR}/ndndsim"

# Apply phase-specific net-new overlay additions on top (if the directory exists).
if [[ -d "${SCRIPT_DIR}/overlay-op" ]]; then
    echo "==> Applying overlay-op patches for phase: ${NDND_PHASE}"
    if [[ "$NDND_PHASE" == "onephase" ]]; then
        copy_overlay_additions "${SCRIPT_DIR}/overlay-op" "$TRANSFORMED_DIR" "overlay-op"
    fi
fi

# Apply twophase-only net-new overlay additions (if the directory exists).
if [[ -d "${SCRIPT_DIR}/overlay-tw" ]]; then
    echo "==> Applying overlay-tw patches for phase: ${NDND_PHASE}"
    if [[ "$NDND_PHASE" == "twophase" ]]; then
        copy_overlay_additions "${SCRIPT_DIR}/overlay-tw" "$TRANSFORMED_DIR" "overlay-tw"
    fi
fi

# ---------- step 3: go.work ----------
echo "==> Generating go.work"

cat > "${SCRIPT_DIR}/go.work" << EOF
go 1.24.0

use ./ndndsim
use ./transform
use ./.transformed-ndnd-${NDND_PHASE}
EOF

cd "${SCRIPT_DIR}"

# Sync workspace checksums.
${GO} work sync 2>/dev/null || true

# ---------- step 4: build ----------
echo "==> Building simulation library"
${GO} build -buildmode=c-archive \
    -o "$OUT_LIB" \
    github.com/named-data/ndnd/sim/cmd

echo "==> Running sim tests"
${GO} test -count=1 -timeout 300s github.com/named-data/ndnd/sim

echo "==> Done: ${OUT_LIB}"
