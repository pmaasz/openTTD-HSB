#!/bin/sh
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/openttd-upstream"
BUILD_DIR="${HSB_TEST_BUILD_DIR:-$ROOT/build/test}"
PATCH="$ROOT/patches/01-headless-scenario-builder.diff"
if [ -n "$(git -C "$DIR" status --porcelain --untracked-files=all)" ]; then
  echo "[test-builder] upstream worktree is dirty" >&2
  exit 1
fi
sh "$ROOT/scripts/apply-patches.sh"
cleanup()
{
  git -C "$DIR" apply --reverse "$PATCH" >/dev/null 2>&1 || :
}
trap cleanup EXIT
trap 'exit 1' 1 2 15
cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DOPTION_DEDICATED=OFF
cmake --build "$BUILD_DIR" --target openttd_test -j "${HSB_TEST_JOBS:-2}"
ctest --test-dir "$BUILD_DIR" --output-on-failure
