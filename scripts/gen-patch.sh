#!/bin/sh
# Regenerate patches/01-headless-scenario-builder.diff from builder/ sources.
# Source of truth: builder/hsb_builder.{h,cpp}, math header, tests + upstream anchors below.
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
  echo "[gen-patch] submodule not checked out; run: git -C '$ROOT' submodule update --init --depth 1" >&2
  exit 1
fi
# The normal loop is apply-patches.sh -> build -> gen-patch.sh, so the submodule
# arrives here with the *previous* patch applied and the four hsb_builder files
# sitting untracked. That is not a dirty worktree, it is this script's own
# output, so it is reversed here rather than refused: a refusal that only
# triggers on the documented workflow trains people to reach for
# `git checkout --` inside the submodule, which on an intent-to-add file yields
# a 0-byte source that fails only at link time.
if [ -f "$OUT" ] && git -C "$DIR" apply --check --reverse "$OUT" >/dev/null 2>&1; then
  echo "[gen-patch] previous patch is applied; reversing it first"
  git -C "$DIR" apply --reverse "$OUT" >/dev/null 2>&1 || :
fi
upstream_status=$(git -C "$DIR" status --porcelain --untracked-files=all)
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
  git -C "$DIR" checkout -- src/CMakeLists.txt src/genworld.cpp src/openttd.cpp src/rail.cpp \
      src/saveload/misc_sl.cpp src/tests/CMakeLists.txt >/dev/null 2>&1 || :
  rm -f "$DIR/src/hsb_builder.h" "$DIR/src/hsb_builder.cpp" "$DIR/src/hsb_builder_math.h" \
        "$DIR/src/hsb_builder_net.h" >/dev/null 2>&1 || :
  rm -f "$DIR/src/tests/hsb_builder.cpp" >/dev/null 2>&1 || :
  remaining=$(git -C "$DIR" status --porcelain --untracked-files=all) || return 1
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
cp "$ROOT/builder/hsb_builder_math.h" "$DIR/src/hsb_builder_math.h"
cp "$ROOT/builder/hsb_builder_net.h" "$DIR/src/hsb_builder_net.h"
cp "$ROOT/builder/hsb_builder_test.cpp" "$DIR/src/tests/hsb_builder.cpp"
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
new_hook = ("\t/* HSB test aid: HSB_FAST=1 simulates as fast as the CPU allows.\n"
            "\t *\n"
            "\t * A dedicated server runs the game loop in real time, so a ten-year\n"
            "\t * playthrough of a 4096 map takes hours. _game_speed = 0 is the engine's\n"
            "\t * own \"infinite\" setting - the same mechanism the fast-forward button\n"
            "\t * uses - so the simulation stays deterministic and a save is unaffected;\n"
            "\t * only the pacing changes. Test and CI use only; never set in a shipped\n"
            "\t * build, exactly like the router tunables in hsb_builder.cpp. */\n"
            "\tif (std::getenv(\"HSB_FAST\") != nullptr) _game_speed = 0;\n\n"
            "\t/* HSB headless scenario builder: runs once when the editor is ready. */\n"
            "\tif (HsbBuildRequested()) {\n"
            "\t\t/* _exit, not exit: GameLoop runs on the video thread, and exit()\n"
            "\t\t * from there unwinds the process through atexit handlers while\n"
            "\t\t * other threads are still running - which prints \"pure virtual\n"
            "\t\t * method called\" and a crash log on top of the real error. The\n"
            "\t\t * engine makes the same choice in UserErrorI. The flush is ours\n"
            "\t\t * because _exit does not run the stdio atexit hook, and the\n"
            "\t\t * audit line a gate greps for would otherwise be lost. */\n"
            "\t\tif (!HsbBuildScenario()) {\n"
            "\t\t\tstd::fflush(nullptr);\n"
            "\t\t\t_exit(1);\n"
            "\t\t}\n"
            "\t\t_exit_game = true;\n"
            "\t\treturn;\n"
            "\t}\n\n"
            "\tIncreaseSpriteLRU();\n\n\t/* Check for UDP stuff */")
assert old_hook in t, "hook anchor not found"
t = t.replace(old_hook, new_hook)
p.write_text(t)

# HasRailTypeAvail: the first rail tile of a scenario segfaults without this.
r = upstream / "src" / "rail.cpp"
s = r.read_text()
old_rail_inc = '#include "company_func.h"\n#include "company_base.h"\n#include "engine_base.h"\n\n#include "table/track_data.h"'
assert old_rail_inc in s, "rail.cpp include anchor not found"
new_rail_inc = '#include "company_func.h"\n#include "company_base.h"\n#include "engine_base.h"\n#include "genworld.h"\n\n#include "table/track_data.h"'
s = s.replace(old_rail_inc, new_rail_inc, 1)
old_rail = """bool HasRailTypeAvail(const CompanyID company, const RailType railtype)
{
\treturn !_railtypes_hidden_mask.Test(railtype) && Company::Get(company)->avail_railtypes.Test(railtype);
}"""
new_rail = """bool HasRailTypeAvail(const CompanyID company, const RailType railtype)
{
\t/* The scenario editor and the world generator lay track without a company,
\t * exactly as they lay road, and HasRoadTypeAvail has always had this case
\t * handled. Without it here, ValParamRailType() reads through a company that
\t * does not exist yet and building the first rail tile in a scenario
\t * segfaults - which is what the headless builder does. */
\tif (company == OWNER_DEITY || company == OWNER_TOWN || _game_mode == GM_EDITOR || _generating_world) {
\t\treturn railtype < RAILTYPE_END && !_railtypes_hidden_mask.Test(railtype) &&
\t\t       GetRailTypeInfo(railtype)->label != 0;
\t}
\treturn !_railtypes_hidden_mask.Test(railtype) && Company::Get(company)->avail_railtypes.Test(railtype);
}"""
assert old_rail in s, "HasRailTypeAvail anchor not found"
r.write_text(s.replace(old_rail, new_rail))

# The start view the builder asked for, written into the .scn as data.
r = upstream / "src" / "saveload" / "misc_sl.cpp"
s = r.read_text()
old_inc_sl = '#include "../viewport_func.h"\n#include "../gfx_func.h"\n#include "../core/random_func.hpp"\n#include "../fios.h"'
assert old_inc_sl in s, "misc_sl include anchor not found"
new_inc_sl = ('#include "../viewport_func.h"\n#include "../gfx_func.h"\n#include "../core/random_func.hpp"\n'
              '#include "../debug.h"\n#include "../fios.h"\n#include "../landscape.h"\n#include "../hsb_builder.h"')
s = s.replace(old_inc_sl, new_inc_sl, 1)
old_zoom = "ZoomLevel _saved_scrollpos_zoom;\n"
new_zoom = """ZoomLevel _saved_scrollpos_zoom;

/**
 * The tile a generated scenario should open on, or #INVALID_TILE.
 *
 * The headless scenario builder has no window, so there is no viewport to read a
 * position from and the saved value is deliberately invalid. The player then
 * starts on the map corner - a field with a river and nothing on it, which is
 * exactly what a first playthrough opened on. The builder sets this tile so that
 * the position is written into the .scn as data. A stock, unpatched engine reads
 * the same saved fields, so the fix ships without needing a patched binary on the
 * player's machine.
 */
TileIndex _hsb_start_view_tile = INVALID_TILE;
"""
assert old_zoom in s, "misc_sl scrollpos anchor not found"
s = s.replace(old_zoom, new_zoom, 1)
old_save = """void SaveViewportBeforeSaveGame()
{
\t/* Don't use GetMainWindow() in case the window does not exist. */
\tconst Window *w = FindWindowById(WC_MAIN_WINDOW, 0);
"""
new_save = """void SaveViewportBeforeSaveGame()
{
\t/* Don't use GetMainWindow() in case the window does not exist. */
\tconst Window *w = FindWindowById(WC_MAIN_WINDOW, 0);

\t/* The headless builder asks for a specific opening view, and it asks for it
\t * here rather than in ResetViewportAfterLoadGame() because that is the only
\t * side that reaches the .scn as data: a stock, unpatched engine loading the
\t * scenario reads these same saved fields. Doing it on the load side would only
\t * ever help players running this patched binary, and the binary a player
\t * actually has is the stock one.
\t *
\t * This also overrides the editor's own viewport, which exists even with
\t * "-v null" and is left pointing at the map corner. */
\tif (_hsb_start_view_tile != INVALID_TILE) {
\t\t/* The saved position is the top-left of the viewport in 2D viewport pixels,
\t\t * so it is the tile's 2D coordinate minus half a nominal window. The window
\t\t * size is not known here, and it does not have to be: ClampViewportToMap()
\t\t * keeps the result on the map for any window, and a town that sits slightly
\t\t * off-centre is a far better opening view than the empty map corner. */
\t\tPoint pt = RemapCoords2(TileX(_hsb_start_view_tile) * TILE_SIZE, TileY(_hsb_start_view_tile) * TILE_SIZE);
\t\t_saved_scrollpos_x = pt.x - 320 * ZOOM_BASE;
\t\t_saved_scrollpos_y = pt.y - 200 * ZOOM_BASE;
\t\t_saved_scrollpos_zoom = ZoomLevel::Normal;
\t\tDebug(sl, 0, "hsb: saving start view for tile ({},{}): 2D ({},{}), scrollpos ({},{}), zoom {}",
\t\t      TileX(_hsb_start_view_tile), TileY(_hsb_start_view_tile),
\t\t      pt.x, pt.y, _saved_scrollpos_x, _saved_scrollpos_y, _saved_scrollpos_zoom);
\t\treturn;
\t}
"""
assert old_save in s, "SaveViewportBeforeSaveGame anchor not found"
r.write_text(s.replace(old_save, new_save, 1))

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
new_cmake = "    genworld.cpp\n    genworld.h\n    genworld_gui.cpp\n    hsb_builder.cpp\n    hsb_builder.h\n    hsb_builder_math.h\n    hsb_builder_net.h"
assert old_cmake in u, "CMake anchor not found"
u = u.replace(old_cmake, new_cmake)
q.write_text(u)
r = upstream / "src" / "tests" / "CMakeLists.txt"
t = r.read_text()
old_tests = "    history_func.cpp\n"
new_tests = "    history_func.cpp\n    hsb_builder.cpp\n"
assert old_tests in t, "test CMake anchor not found"
r.write_text(t.replace(old_tests, new_tests))
print("[gen-patch] anchors applied")
PY
git -C "$DIR" add -N src/hsb_builder.h src/hsb_builder.cpp src/hsb_builder_math.h src/hsb_builder_net.h src/tests/hsb_builder.cpp
git -C "$DIR" diff --no-ext-diff --no-textconv --src-prefix=a/ --dst-prefix=b/ > "$GENERATED"
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
