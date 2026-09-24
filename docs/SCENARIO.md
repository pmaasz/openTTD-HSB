# Scenario build — headless builder runbook

The scenario is built **deterministically by code**: `HSB_BUILD=<manifest>
openttd -e` runs `src/hsb_builder.*`, which replays the editor workflow using
the exact engine functions the GUI calls: heightmap → rivers/estuaries →
town JSON import → industries → start year → save → exit.

## Build it

Prerequisites: built engine (`docs/LOCAL-BUILD.md`), wanted NewGRFs + GS in
the personal dir, heightmap PNG + `towns.json` in `scenario/heightmap/`.

```sh
# 1. game data in place (regenerate if sources changed)
python3 scripts/build-heightmap.py \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --zoom 9 --size 4096 --out /tmp/map-4096.png --master-out /tmp/map-master.tif
python3 scripts/build-towns.py \
  --cities cities.csv --heightmap /tmp/map-4096.png \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --out /tmp/towns.json --debug-out /tmp/towns.debug.csv
python3 scripts/build-manifest.py \
  --heightmap /tmp/map-4096.png --towns /tmp/towns.json \
  --industries industries.json --rivers rivers.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --start-year 2025 --save output.scn --out /tmp/build.json
cp /tmp/map-4096.png /tmp/towns.json <personal-dir>/scenario/heightmap/

# 2. run the builder (config MUST live inside the personal dir:
#    the engine derives the personal dir from the -c location!)
HSB_BUILD=/tmp/build.json ./build/upstream/openttd -e \
  -c <personal-dir>/hsb-builder.cfg
# => <personal-dir>/scenario/output.scn (~100 s for 4096)
```

Inputs (yours, versioned): heightmap PNG, `towns.json`, `industries.json`,
`rivers.json`. Outputs (git-ignored): `*.scn`.

## Reference: 4096 build evidence (GTTD origin, 2026-09-24)

* Terrain 4096×4096, height 12, clockwise; ~18k water tiles carved.
* 124/124 towns, 28 industry types, 73/73 industries, start 2025-01-01.
* Two seeded rebuilds: identical map bytes except 0.5% tail metadata
  (RNG state) — content-deterministic.
* Visual check in editor: terrain, towns with buildings, river network,
  industry directory producing, correct date.

## Known quirks (any region)

* SRTM sea-level artifacts: land below sea level renders as water. Nudge town
  anchors to solid land (`towns.debug.csv` records the nudge distance);
  keep port/oil-rig/fishing anchors on real water.
* Town sprawl can cover industry anchors — the spiral fallback (default 32
  tiles) usually finds room; move the anchor if the log shows persistent
  failures.
* The `-c` location defines the personal dir (a `-c` in /tmp reads
  /tmp/scenario/...). Keep the config inside the personal dir.
* The engine rewrites the `-c` file on shutdown — regenerate it per run.

## Manual fallback (no builder)

The classic GUI flow still works: Scenario Editor → Load heightmap
(clockwise) → Town Generation → Load from file (`towns.json`) → draw
rivers per `rivers.json` waypoints → place industries per `industries.json` →
bake settings (start year, climate, currency) → save.
