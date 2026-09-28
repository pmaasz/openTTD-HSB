# openTTD-HSB — Headless Scenario Builder for OpenTTD

Build OpenTTD scenarios deterministically by code, not by hand-clicking:
`HSB_BUILD=<manifest> openttd -e` replays the editor workflow using the exact
engine functions the GUI calls: heightmap → rivers/estuaries → town JSON
import → industries → start year → save → exit.

License: GPLv2 (see `LICENSE.md`) to match OpenTTD.

## Repo layout

```text
/openttd-upstream/          # git submodule, pinned (see openttd-upstream.PIN, tag 15.3)
/builder/
  hsb_builder.h
  hsb_builder.cpp           # source of truth (GPLv2)
patches/
  01-headless-scenario-builder.diff  # generated from builder/ (see scripts/gen-patch.sh)
/scripts/
  build-heightmap.py        # DEM tiles -> heightmap PNG (any --bounds)
  build-towns.py            # cities.csv -> towns.json (any --bounds)
  render-preview.py         # terrain + rivers + towns + industries PNG, and
                            # a gate: fails when a town anchor lands on water
  build-manifest.py         # write HSB_BUILD manifest (embeds --bounds)
  validate-scenario.py      # schema + land/water checks (any --bounds)
  fetch-upstream.sh         # verify submodule pin
  apply-patches.sh          # apply patches/ to upstream
  gen-patch.sh              # regenerate patches/ from builder/
  test-builder.sh           # build + run Catch2/CTest builder tests
/docs/
  FORMATS.md                # manifest + towns/industries/rivers schemas
  SCENARIO.md               # build runbook
  LOCAL-BUILD.md            # engine build + headless run
```

## Quickstart

```sh
# 0. checkout + pin check
git submodule update --init --depth 1
sh scripts/fetch-upstream.sh

# 1. patches + build (see docs/LOCAL-BUILD.md for no-sudo deps)
sh scripts/apply-patches.sh
cmake -S openttd-upstream -B build/upstream -DCMAKE_BUILD_TYPE=Release
cmake --build build/upstream -j2

# 2. data (any region — bounds must agree everywhere)
# Provide your own cities.csv, industries.json, and rivers.json first; their
# schemas are documented in docs/FORMATS.md.
python3 scripts/build-heightmap.py \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --zoom 9 --size 4096 --sea-level -2 \
  --out /tmp/map-4096.png --master-out /tmp/map-master.tif
python3 scripts/build-towns.py \
  --cities cities.csv --heightmap /tmp/map-4096.png \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --out /tmp/towns.json --debug-out /tmp/towns.debug.csv
python3 scripts/render-preview.py \
  --heightmap /tmp/map-4096.png --towns /tmp/towns.json \
  --industries industries.json --rivers rivers.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --size 1024 --out /tmp/preview.png
python3 scripts/validate-scenario.py \
  --heightmap /tmp/map-4096.png --cities cities.csv \
  --towns /tmp/towns.json --industries industries.json --rivers rivers.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5
python3 scripts/build-manifest.py \
  --heightmap /tmp/map-4096.png --towns /tmp/towns.json \
  --industries industries.json --rivers rivers.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --start-year 2025 --snow-line 10 --save output.scn --out /tmp/build.json

# 3. run the builder (config MUST live inside the personal dir:
#    the engine derives the personal dir from the -c location!)
cp /tmp/map-4096.png /tmp/towns.json <personal-dir>/scenario/heightmap/
HSB_BUILD=/tmp/build.json ./build/upstream/openttd -e \
  -c <personal-dir>/hsb-builder.cfg
# => <personal-dir>/scenario/output.scn
```

Details: `docs/SCENARIO.md`. 
Formats: `docs/FORMATS.md`.
Release notes: `CHANGELOG.md`.
AI agents: `mcp/` (MCP server, 10 tools).

## Does the build actually work?

Exit 0 only means it ran. The build logs a measurement of what it produced
(illustrative numbers for a 4096 map, not a recorded run):

```text
hsb: audit: towns=124 towns_on_water=0 industries=73 water=2810000
  sea_fraction=0.168 water_bodies=41 isolated_water_tiles=2
  water_unreachable=310 carved=18800 carved_reachable=18800
```

`towns_on_water` must be 0, and `carved_reachable` should equal `carved` — a
gap means part of the river is a closed pond no ship can enter. `mcp/` returns
this as `run_builder`'s `audit` field so a gate can assert on the numbers
rather than on the exit code.

## Principles

1. Minimal C++ fork, maximal data-driven content (easy upstream merges).
2. One patch, one concern (`patches/` stays small and reviewable).
3. All generated data reproducible via `scripts/` (never hand-edit binaries).
4. Deterministic rebuilds via fixed `seed` in the manifest.
5. Measure the output; a build that exits 0 can still be a broken map.
