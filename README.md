# openTTD-HSB — Headless Scenario Builder for OpenTTD

Build OpenTTD scenarios deterministically by code, not by hand-clicking:
`HSB_BUILD=<manifest> openttd -e` replays the editor workflow using the exact
engine functions the GUI calls: heightmap → rivers/estuaries → town JSON
import → industries → roads → rail → airports → docks → a running fleet →
start year → save → exit.

License: GPLv2 (see `LICENSE.md`) to match OpenTTD.

## Repo layout

```text
/openttd-upstream/          # git submodule, pinned (see openttd-upstream.PIN, tag 16.0-beta5)
/builder/
  hsb_builder.h
  hsb_builder.cpp           # source of truth (GPLv2)
  hsb_builder_math.h        # pure geometry, unit tested
  hsb_builder_net.h         # pure network geometry + tunables, unit tested
  hsb_builder_test.cpp      # Catch2 suite
patches/
  01-headless-scenario-builder.diff  # generated from builder/ (see scripts/gen-patch.sh)
/scripts/
  build-heightmap.py        # DEM tiles -> heightmap PNG (any --bounds)
  build-towns.py            # cities.csv -> towns.json (any --bounds)
  render-preview.py         # terrain + rivers + towns + industries PNG, and
                            # a gate: fails when a town anchor lands on water
  build-manifest.py         # write HSB_BUILD manifest (embeds --bounds)
  osm-reference.py          # Geofabrik/local .osm.pbf -> full OSM reference
  build-osm-network.py      # OSM reference -> autobahn/rail/stations JSON
  validate-scenario.py      # schema + land/water checks (any --bounds)
  fetch-upstream.sh         # verify submodule pin
  apply-patches.sh          # apply patches/ to upstream
  gen-patch.sh              # regenerate patches/ from builder/
  test-builder.sh           # build + run Catch2/CTest builder tests
/docs/
  FORMATS.md                # manifest + towns/industries/rivers/network schemas
  OSM.md                    # OSM reference + OSM-derived network pipeline
  ATTRIBUTION.md            # OSM/ODbL and elevation data credits
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

# 2a. optional: OSM-derived Autobahn/rail/stations.
#     pip install pyosmium requests, then:
python3 scripts/osm-reference.py --work-dir /tmp/osm/work
python3 scripts/build-osm-network.py --reference-dir assets/geodata/reference \
  --towns /tmp/towns.json --out-dir assets/geodata \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 --size 4096
#     then pass --osm to build-manifest.py (see docs/OSM.md).

# 2b. optional: the network. Every file is opt-in; name none and the build is
#     unchanged. competitors >= 2 because the network's own company takes a slot.
python3 scripts/build-manifest.py \
  --heightmap /tmp/map-4096.png --towns /tmp/towns.json \
  --industries industries.json --rivers rivers.json \
  --roads roads.json --autobahnen autobahn.json --rail rail.json \
  --airports airports.json --harbours harbours.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --start-year 2025 --competitors 2 --currency EUR \
  --snow-line 10 --save output.scn --out /tmp/build.json

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
hsb: network audit: roads=73226 orphan_roads=0 road_components=1
  largest_road=71890 towns=124 towns_on_network=124 rail=259
  rail_components=37 stations=61 stations_with_rail=61 bus_stops=48
  depots=48 airports=27 docks=21 vehicles=35 consists=35
  vehicles_routed=35 vehicles_parked=0
hsb: intercity road: a bus from the largest town (pop 1222) reaches 124 of 124 towns
```

`towns_on_water` must be 0, `carved_reachable` should equal `carved`, and
`vehicles_parked` must be 0 — a gap in the first means part of the river is a
closed pond no ship can enter, and a nonzero third means the fleet has routes
and will still never move. The network line is **absent** when no network file
is named, so a gate can tell a terrain-only build from one that ran the network.
`mcp/` returns all of it as `run_builder`'s `audit` field, with the network keys
prefixed `network_`.

`HSB_TUNE_ROAD_RUN`, `HSB_TUNE_ROAD_SNAP`, `HSB_TUNE_CORNER_FAN` and
`HSB_TUNE_ROAD_TRACE` tune the routers; `HSB_DUMP_NETWORK=<path>` writes the
network as a character grid, one character per 4×4 tiles, because a number
cannot show that a corridor runs through the middle of nowhere.

## Principles

1. Minimal C++ fork, maximal data-driven content (easy upstream merges).
2. One patch, one concern (`patches/` stays small and reviewable).
3. All generated data reproducible via `scripts/` (never hand-edit binaries).
4. Deterministic rebuilds via fixed `seed` in the manifest.
5. Measure the output; a build that exits 0 can still be a broken map.
