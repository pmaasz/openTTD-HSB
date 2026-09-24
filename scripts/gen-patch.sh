#!/bin/sh
# Regenerate patches/01-headless-scenario-builder.diff from builder/ sources.
# Source of truth: builder/hsb_builder.{h,cpp} + hook template below.
# Usage: sh scripts/gen-patch.sh
# Part of openTTD-HSB (GPLv2, see LICENSE.md).
set -eu
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/openttd-upstream"
OUT="$ROOT/patches/01-headless-scenario-builder.diff"
if [ ! -d "$DIR/src" ]; then
  echo "[gen-patch] submodule not checked out; run: git submodule update --init --depth 1" >&2
  exit 1
fi
if ! git -C "$DIR" diff --quiet || [ -n "$(git -C "$DIR" status --porcelain)" ]; then
  echo "[gen-patch] upstream worktree is dirty — revert first (see scripts/apply-patches.sh)" >&2
  exit 1
fi
cp "$ROOT/builder/hsb_builder.h" "$DIR/src/hsb_builder.h"
cp "$ROOT/builder/hsb_builder.cpp" "$DIR/src/hsb_builder.cpp"
python3 - <<'PY'
import pathlib
root = pathlib.Path(".").resolve()
# NOTE: run from repo root (gen-patch.sh does not cd).
p = pathlib.Path("openttd-upstream/src/openttd.cpp")
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
q = pathlib.Path("openttd-upstream/src/CMakeLists.txt")
u = q.read_text()
old_cmake = "    genworld.cpp\n    genworld.h\n    genworld_gui.cpp"
new_cmake = "    genworld.cpp\n    genworld.h\n    genworld_gui.cpp\n    hsb_builder.cpp\n    hsb_builder.h"
assert old_cmake in u
u = u.replace(old_cmake, new_cmake)
q.write_text(u)
print("[gen-patch] hooks applied")
PY
git -C "$DIR" add -N src/hsb_builder.h src/hsb_builder.cpp
git -C "$DIR" diff > "$OUT"
git -C "$DIR" checkout -- src/CMakeLists.txt src/openttd.cpp
rm "$DIR/src/hsb_builder.h" "$DIR/src/hsb_builder.cpp"
git -C "$DIR" reset -q
echo "[gen-patch] wrote $OUT"
git -C "$DIR" apply --check "$OUT" && echo "[gen-patch] check OK"
