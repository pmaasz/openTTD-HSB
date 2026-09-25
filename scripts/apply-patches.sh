#!/bin/sh
# Apply HSB patches to the upstream checkout.
# Usage: sh scripts/apply-patches.sh [--check-only]
# Part of openTTD-HSB (GPLv2, see LICENSE.md).
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/openttd-upstream"
case "$#" in
  0) ;;
  1)
    if [ "$1" != "--check-only" ]; then
      echo "usage: sh scripts/apply-patches.sh [--check-only]" >&2
      exit 2
    fi
    ;;
  *)
    echo "usage: sh scripts/apply-patches.sh [--check-only]" >&2
    exit 2
    ;;
esac
if [ ! -d "$DIR/src" ]; then
  echo "[apply-patches] submodule not checked out; run: git -C '$ROOT' submodule update --init --depth 1" >&2
  exit 1
fi
if [ "${1:-}" = "--check-only" ]; then
  git -C "$DIR" apply --check "$ROOT/patches/01-headless-scenario-builder.diff"
  echo "[apply-patches] check OK: 01-headless-scenario-builder.diff applies cleanly"
  exit 0
fi
git -C "$DIR" apply "$ROOT/patches/01-headless-scenario-builder.diff"
echo "[apply-patches] applied 01-headless-scenario-builder.diff"
git -C "$DIR" apply --stat "$ROOT/patches/01-headless-scenario-builder.diff"
echo "[apply-patches] to revert: git -C '$DIR' apply --reverse '$ROOT/patches/01-headless-scenario-builder.diff'"
