# Scenario build — headless builder runbook

The scenario is built **deterministically by code**: `HSB_BUILD=<manifest>
openttd -e` runs `src/hsb_builder.*`, which replays the editor workflow using
the exact engine functions the GUI calls: heightmap → start year →
rivers/estuaries → town JSON import → industries → audit → save → exit.

## Build it

Prerequisites: built engine (`docs/LOCAL-BUILD.md`), wanted NewGRFs + GS in
the personal dir, heightmap PNG + `towns.json` in `scenario/heightmap/`.

Provide your own `cities.csv`, `industries.json`, and `rivers.json`; the
accepted schemas are in `docs/FORMATS.md`.

```sh
# 1. game data in place (regenerate if sources changed)
python3 scripts/build-heightmap.py \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --zoom 9 --size 4096 --sea-level -2 \
  --out /tmp/map-4096.png --master-out /tmp/map-master.tif
python3 scripts/build-towns.py \
  --cities cities.csv --heightmap /tmp/map-4096.png \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --out /tmp/towns.json --debug-out /tmp/towns.debug.csv

# 2. LOOK at it before spending 100 s of engine time. Exits 1 when a town
#    anchor lands on water, which is the one placement bug worth catching
#    this early.
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
  --start-year 2025 --snow-line 10 --max-loan 1500000 \
  --competitors 0 --currency EUR \
  --save output.scn --out /tmp/build.json
cp /tmp/map-4096.png /tmp/towns.json <personal-dir>/scenario/heightmap/

# 3. run the builder (config MUST live inside the personal dir:
#    the engine derives the personal dir from the -c location!)
HSB_BUILD=/tmp/build.json ./build/upstream/openttd -e \
  -c <personal-dir>/hsb-builder.cfg
# => <personal-dir>/scenario/output.scn (~100 s for 4096)
```

Inputs (yours, versioned): heightmap PNG, `towns.json`, `industries.json`,
`rivers.json`. Outputs (git-ignored): `*.scn`.

## Read the audit line, not the exit code

Exit 0 means the build ran. It does not mean the map is right. The last line
before the save is a measurement of what was actually produced. The numbers
below are illustrative for a 4096 map, not a recorded run; read the field
names and the healthy-value column, then paste in your own line.

```text
hsb: audit: towns=124 towns_on_water=0 industries=73 water=2810000
  sea_fraction=0.168 water_bodies=41 isolated_water_tiles=2
  water_unreachable=310 carved=18800 carved_reachable=18800
```

| Field | Healthy | What a bad value means |
|-------|---------|------------------------|
| `towns_on_water` | 0 | a town anchor landed in a lake; nudge it or lower `--sea-level` |
| `carved_reachable` | == `carved` | part of the river is a closed pond no ship can enter |
| `isolated_water_tiles` | 0 or near | the river is shredded into single-tile stamps |
| `water_unreachable` | small | DEM lakes with no outlet; usually harmless, occasionally not |
| `sea_fraction` | 0.1–0.4 | a crop that is nearly all sea or nearly all land is a bounds mistake |

`mcp/server.py` parses this line into `run_builder`'s `audit` field, so an
agent can assert `towns_on_water == 0` without scraping logs.

## Reference: 4096 build evidence

* Terrain 4096×4096, height 12, clockwise; ~18k water tiles carved.
* 124/124 towns, 28 industry types, 73/73 industries, start 2025-01-01.
* Two seeded rebuilds: identical map bytes except 0.5% tail metadata
  (RNG state) — content-deterministic.
* Visual check in editor: terrain, towns with buildings, river network,
  industry directory producing, correct date.

## Known quirks (any region)

* SRTM sea-level artifacts: land below sea level renders as water. Pass
  `--sea-level -2` to the heightmap build to drop tidal flats and polders; then
  nudge town anchors to solid land (`towns.debug.csv` records the nudge
  distance) and keep port/oil-rig/fishing anchors on real water.
* Town sprawl can cover industry anchors — the spiral fallback (default 32
  tiles) usually finds room; move the anchor if the log shows persistent
  failures.
* The `-c` location defines the personal dir (a `-c` in /tmp reads
  /tmp/scenario/...). Keep the config inside the personal dir.
* The engine rewrites the `-c` file on shutdown — regenerate it per run.
* The config is also where the scenario would otherwise inherit its currency,
  loan and company slots from. Pin them in the manifest
  (`docs/FORMATS.md` § *What is pinned and why*).

## Manual fallback (no builder)

The classic GUI flow still works: Scenario Editor → Load heightmap
(clockwise) → Town Generation → Load from file (`towns.json`) → draw
rivers per `rivers.json` waypoints → place industries per `industries.json` →
bake settings (start year, climate, currency) → save.
