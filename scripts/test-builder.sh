#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/openttd-upstream"
BUILD_DIR="${HSB_TEST_BUILD_DIR:-$ROOT/build/test}"
PATCH="$ROOT/patches/01-headless-scenario-builder.diff"
KEEP_PATCH="${HSB_KEEP_PATCH:-0}"
FAST="${HSB_TEST_FAST:-0}"
JOBS="${HSB_TEST_JOBS:-2}"
BUILD_TYPE="${HSB_TEST_BUILD_TYPE:-Debug}"
DEBUG_FLAGS="${HSB_TEST_DEBUG_FLAGS:--O0 -g0}"
if [ -z "${CCACHE:-}" ] && command -v ccache >/dev/null 2>&1; then
  CCACHE=ccache
fi
if git -C "$DIR" apply --check "$PATCH" >/dev/null 2>&1; then
  PATCH_STATE=clean
elif git -C "$DIR" apply --check --reverse "$PATCH" >/dev/null 2>&1; then
  PATCH_STATE=applied
else
  echo "[test-builder] upstream worktree is dirty or the patch is partially applied" >&2
  exit 1
fi
if [ "$PATCH_STATE" = applied ] && [ "$KEEP_PATCH" != 1 ]; then
  echo "[test-builder] patch is already applied; use HSB_KEEP_PATCH=1 or reverse it first" >&2
  exit 1
fi
PATCH_APPLIED=0
if [ "$PATCH_STATE" = clean ]; then
  sh "$ROOT/scripts/apply-patches.sh"
  PATCH_APPLIED=1
fi
cleanup()
{
  if [ "$PATCH_APPLIED" = 1 ] && [ "$KEEP_PATCH" != 1 ]; then
    git -C "$DIR" apply --reverse "$PATCH" >/dev/null 2>&1 || :
  fi
}
trap cleanup EXIT
trap 'exit 1' 1 2 15
set -- -S "$DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DOPTION_DEDICATED=OFF
if [ "$BUILD_TYPE" = Debug ]; then
  set -- "$@" "-DCMAKE_CXX_FLAGS_DEBUG=$DEBUG_FLAGS"
fi
if [ -n "${CCACHE:-}" ]; then
  set -- "$@" "-DCMAKE_CXX_COMPILER_LAUNCHER=$CCACHE"
fi
cmake "$@"
if [ "$FAST" = 1 ]; then
  cmake --build "$BUILD_DIR" --target src/hsb_builder.o -j "$JOBS"
  cmake --build "$BUILD_DIR" --target src/tests/hsb_builder.o -j "$JOBS"
  echo "[test-builder] fast object checks passed"
  exit 0
fi
cmake --build "$BUILD_DIR" --target openttd_test -j "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure
