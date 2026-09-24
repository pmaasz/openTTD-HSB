# openTTD-HSB — Headless Scenario Builder for OpenTTD

Build OpenTTD scenarios deterministically by code, not by hand-clicking:
`HSB_BUILD=<manifest> openttd -e` replays the editor workflow using the exact
engine functions the GUI calls: heightmap → rivers/estuaries → town JSON
import → industries → start year → save → exit.

Extracted and generalized from
[GermanyTransportTycoonDeluxe](https://github.com/pmaasz/GermanyTransportTycoonDeluxe)
patch 06 (verified 2026-09-24: 4096 map, 124/124 towns, 73/73 industries,
content-deterministic rebuilds). This repo is region-agnostic: geographic
bounds live in the manifest, not in C++.

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
  build-manifest.py         # write HSB_BUILD manifest (embeds --bounds)
  validate-scenario.py      # schema + land/water checks (any --bounds)
  fetch-upstream.sh         # verify submodule pin
  apply-patches.sh          # apply patches/ to upstream
  gen-patch.sh              # regenerate patches/ from builder/
/docs/
  FORMATS.md                # manifest + towns/industries/rivers schemas
  SCENARIO.md               # build runbook
  LOCAL-BUILD.md            # engine build + headless run
  MIGRATION.md              # GTTD_BUILD -> HSB_BUILD
```

No example game data ships here — bring your own heightmap + towns +
industries (see `docs/FORMATS.md`).

## Quickstart

```sh
# 0. checkout + pin check
git submodule update --init --depth 1
sh scripts/fetch-upstream.sh

# 1. patches + build (see docs/LOCAL-BUILD.md for no-sudo deps)
sh scripts/apply-patches.sh
cmake -S openttd-upstream -B build/upstream -DCMAKE_BUILD_TYPE=Release
cmake --build build/upstream -j$(nproc)

# 2. data (any region — bounds must agree everywhere)
python3 scripts/build-heightmap.py \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --zoom 9 --size 4096 --out /tmp/map-4096.png --master-out /tmp/map-master.tif
python3 scripts/build-towns.py \
  --cities cities.csv --heightmap /tmp/map-4096.png \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --out /tmp/towns.json --debug-out /tmp/towns.debug.csv
python3 scripts/validate-scenario.py \
  --heightmap /tmp/map-4096.png --cities cities.csv \
  --towns /tmp/towns.json --industries industries.json --rivers rivers.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5
python3 scripts/build-manifest.py \
  --heightmap /tmp/map-4096.png --towns /tmp/towns.json \
  --industries industries.json --rivers rivers.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --start-year 2025 --save output.scn --out /tmp/build.json

# 3. run the builder (config MUST live inside the personal dir:
#    the engine derives the personal dir from the -c location!)
cp /tmp/map-4096.png /tmp/towns.json <personal-dir>/scenario/heightmap/
HSB_BUILD=/tmp/build.json ./build/upstream/openttd -e \
  -c <personal-dir>/hsb-builder.cfg
# => <personal-dir>/scenario/output.scn
```

Details: `docs/SCENARIO.md`. Formats: `docs/FORMATS.md`.
GTTD migration: `docs/MIGRATION.md`.

## Principles

1. Minimal C++ fork, maximal data-driven content (easy upstream merges).
2. One patch, one concern (`patches/` stays small and reviewable).
3. All generated data reproducible via `scripts/` (never hand-edit binaries).
4. Deterministic rebuilds via fixed `seed` in the manifest.
