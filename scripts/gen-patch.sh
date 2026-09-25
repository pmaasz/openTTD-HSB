#!/bin/sh
# Regenerate patches/01-headless-scenario-builder.diff from builder/ sources.
# Source of truth: builder/hsb_builder.{h,cpp} + upstream anchors below.
# Usage: sh scripts/gen-patch.sh [--check]
# Part of openTTD-HSB (GPLv2, see LICENSE.md).
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/openttd-upstream"
OUT="$ROOT/patches/01-headless-scenario-builder.diff"
CHECK=0
case "$#" in
  0) ;;
  1)
    if [ "$1" != "--check" ]; then
      echo "usage: sh scripts/gen-patch.sh [--check]" >&2
      exit 2
    fi
    CHECK=1
    ;;
  *)
    echo "usage: sh scripts/gen-patch.sh [--check]" >&2
    exit 2
    ;;
esac
if [ ! -d "$DIR/src" ]; then
  echo "[gen-patch] submodule not checked out; run: git submodule update --init --depth 1" >&2
  exit 1
fi
upstream_status=$(git -C "$DIR" status --porcelain)
if [ -n "$upstream_status" ]; then
  echo "[gen-patch] upstream worktree is dirty; revert it first (see patches/README.md)" >&2
  exit 1
fi
if [ "$CHECK" -eq 1 ] && [ ! -f "$OUT" ]; then
  echo "[gen-patch] missing expected patch: $OUT" >&2
  exit 1
fi
clean_upstream()
{
  git -C "$DIR" reset -q >/dev/null 2>&1 || :
  git -C "$DIR" checkout -- src/CMakeLists.txt src/genworld.cpp src/openttd.cpp >/dev/null 2>&1 || :
  rm -f "$DIR/src/hsb_builder.h" "$DIR/src/hsb_builder.cpp" >/dev/null 2>&1 || :
  remaining=$(git -C "$DIR" status --porcelain) || return 1
  if [ -n "$remaining" ]; then
    echo "[gen-patch] cleanup failed; upstream is still dirty" >&2
    return 1
  fi
}
GENERATED=
cleanup()
{
  status=$?
  trap - 0 1 2 15
  if ! clean_upstream; then
    if [ "$status" -eq 0 ]; then
      status=1
    fi
  fi
  if [ -n "$GENERATED" ]; then
    rm -f "$GENERATED" || :
  fi
  exit "$status"
}
trap cleanup 0
trap 'exit 1' 1 2 15
GENERATED=$(mktemp "$ROOT/patches/.01-headless-scenario-builder.diff.XXXXXX")
cp "$ROOT/builder/hsb_builder.h" "$DIR/src/hsb_builder.h"
cp "$ROOT/builder/hsb_builder.cpp" "$DIR/src/hsb_builder.cpp"
python3 - "$DIR" <<'PY'
import pathlib
import sys

upstream = pathlib.Path(sys.argv[1])
p = upstream / "src" / "openttd.cpp"
t = p.read_text()
old_inc = '#include "genworld.h"\n#include "progress.h"'
new_inc = '#include "genworld.h"\n#include "hsb_builder.h"\n#include "progress.h"'
assert old_inc in t, "include anchor not found"
t = t.replace(old_inc, new_inc)
old_hook = "\tIncreaseSpriteLRU();\n\n\t/* Check for UDP stuff */"
new_hook = ("\t/* HSB headless scenario builder: runs once when the editor is ready. */\n"
            "\tif (HsbBuildRequested()) {\n"
            "\t\tif (!HsbBuildScenario()) exit(1);\n"
            "\t\t_exit_game = true;\n"
            "\t\treturn;\n"
            "\t}\n\n"
            "\tIncreaseSpriteLRU();\n\n\t/* Check for UDP stuff */")
assert old_hook in t, "hook anchor not found"
t = t.replace(old_hook, new_hook)
p.write_text(t)
r = upstream / "src" / "genworld.cpp"
s = r.read_text()
old_mapping = "target_tile = TileXY((1 - y_proportion * Map::MaxX()), x_proportion * Map::MaxY());"
new_mapping = "target_tile = TileXY((1.0 - y_proportion) * Map::MaxX(), x_proportion * Map::MaxY());"
assert old_mapping in s, "counter-clockwise town mapping anchor not found"
s = s.replace(old_mapping, new_mapping)
r.write_text(s)
q = upstream / "src" / "CMakeLists.txt"
u = q.read_text()
old_cmake = "    genworld.cpp\n    genworld.h\n    genworld_gui.cpp"
new_cmake = "    genworld.cpp\n    genworld.h\n    genworld_gui.cpp\n    hsb_builder.cpp\n    hsb_builder.h"
assert old_cmake in u, "CMake anchor not found"
u = u.replace(old_cmake, new_cmake)
q.write_text(u)
print("[gen-patch] anchors applied")
PY
git -C "$DIR" add -N src/hsb_builder.h src/hsb_builder.cpp
git -C "$DIR" diff > "$GENERATED"
clean_upstream
git -C "$DIR" apply --check "$GENERATED"
if [ "$CHECK" -eq 1 ]; then
  if cmp -s "$GENERATED" "$OUT"; then
    echo "[gen-patch] check OK: generated patch matches $OUT byte-for-byte"
  else
    cmp_status=$?
    if [ "$cmp_status" -eq 1 ]; then
      echo "[gen-patch] MISMATCH: generated patch differs byte-for-byte from $OUT" >&2
      echo "[gen-patch] patch regeneration is required" >&2
    else
      echo "[gen-patch] comparison failed with status $cmp_status" >&2
    fi
    exit 1
  fi
else
  chmod 644 "$GENERATED"
  mv "$GENERATED" "$OUT"
  echo "[gen-patch] wrote $OUT"
  echo "[gen-patch] check OK"
fi
