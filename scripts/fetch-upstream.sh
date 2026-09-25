#!/bin/sh
# Verify the pinned upstream checkout matches openttd-upstream.PIN.
# Used by CI and by developers after `git submodule update --init`.
# Part of openTTD-HSB (GPLv2, see LICENSE.md).
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
PIN_FILE="$ROOT/openttd-upstream.PIN"
DIR="$ROOT/openttd-upstream"
want=$(grep '^sha=' "$PIN_FILE" | cut -d= -f2)
if [ ! -d "$DIR/src" ]; then
  echo "[fetch-upstream] submodule not checked out; run: git -C '$ROOT' submodule update --init --depth 1" >&2
  exit 1
fi
have=$(git -C "$DIR" rev-parse HEAD)
echo "[fetch-upstream] want=$want"
echo "[fetch-upstream] have=$have"
if [ "$want" = "$have" ]; then
  echo "[fetch-upstream] OK: upstream pinned ($want)"
else
  echo "[fetch-upstream] MISMATCH: expected $want" >&2
  exit 1
fi
for f in src/openttd.cpp src/CMakeLists.txt src/genworld.cpp; do
  [ -f "$DIR/$f" ] || { echo "[fetch-upstream] missing $f" >&2; exit 1; }
done
echo "[fetch-upstream] key files present"
