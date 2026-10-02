# Scenario build — headless builder runbook

The scenario is built **deterministically by code**: `HSB_BUILD=<manifest>
openttd -e` runs `src/hsb_builder.*`, which replays the editor workflow using
the exact engine functions the GUI calls: heightmap → start year →
rivers/estuaries → town JSON import → industries → **roads → rail → airports →
docks → a running fleet** → audit → save → exit.

The network phase is **opt-in per input file**. A manifest that names none of
`roads`, `autobahnen`, `rail`, `airports`, `harbours` builds exactly what it
built before: same save, same audit line, no extra keys in the manifest.

## Build it

Prerequisites: built engine (`docs/LOCAL-BUILD.md`), wanted NewGRFs + GS in
the personal dir, heightmap PNG + `towns.json` in `scenario/heightmap/`.

Provide your own `cities.csv`, `industries.json`, and `rivers.json`; the
accepted schemas — and the optional `roads.json`, `autobahn.json`, `rail.json`,
`airports.json`, `harbours.json` — are in `docs/FORMATS.md`.

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

# 2b. OPTIONAL: the network. competitors >= 2, because the company that owns
#     the network takes one slot and the player needs another to start in.
python3 scripts/build-manifest.py \
  --heightmap /tmp/map-4096.png --towns /tmp/towns.json \
  --industries industries.json --rivers rivers.json \
  --roads roads.json --autobahnen autobahn.json --rail rail.json \
  --airports airports.json --harbours harbours.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --start-year 2025 --snow-line 10 --competitors 2 --currency EUR \
  --save output.scn --out /tmp/build.json

# 3. run the builder (config MUST live inside the personal dir:
#    the engine derives the personal dir from the -c location!)
HSB_BUILD=/tmp/build.json ./build/upstream/openttd -e \
  -c <personal-dir>/hsb-builder.cfg
# => <personal-dir>/scenario/output.scn (~100 s for 4096)
```

Inputs (yours, versioned): heightmap PNG, `towns.json`, `industries.json`,
`rivers.json`, and the optional network files. Outputs (git-ignored): `*.scn`.

## Read the audit lines, not the exit code

Exit 0 means the build ran. It does not mean the map is right. The lines before
the save are a measurement of what was actually produced. The numbers below are
illustrative for a 4096 map, not a recorded run; read the field names and the
healthy-value column, then paste in your own lines.

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

| Field | Healthy | What a bad value means |
|-------|---------|------------------------|
| `towns_on_water` | 0 | a town anchor landed in a lake; nudge it or lower `--sea-level` |
| `carved_reachable` | == `carved` | part of the river is a closed pond no ship can enter |
| `isolated_water_tiles` | 0 or near | the river is shredded into single-tile stamps |
| `water_unreachable` | small | DEM lakes with no outlet; usually harmless, occasionally not |
| `sea_fraction` | 0.1–0.4 | a crop that is nearly all sea or nearly all land is a bounds mistake |
| `road_components` | 1 | the trunk road is in pieces |
| `towns_on_network` | == `towns` | cheap check; see the intercity line for the real one |
| intercity `reaches` | == `towns` | a bus cannot be driven from the biggest town to the others |
| `vehicles_parked` | 0 | routed vehicles that never left the depot: the map is a museum |
| `stations_with_rail` | > 0 | track with no station anywhere is track the player must dig up |

The `network audit` line is **absent** when no network file is named, so a gate
can tell a terrain-only build from one that ran the network.

`mcp/server.py` parses both lines into `run_builder`'s `audit` field (network
keys prefixed `network_`), so an agent can assert `towns_on_water == 0` and
`network_vehicles_parked == 0` without scraping logs.

## When the network will not route

The routers are deliberately honest: they report what they refused rather than
quietly producing a plausible-looking map. Three lines do the diagnosing.

```text
hsb: road refusal x599: No suitable road
hsb: rail refusal x31: ?: Must remove road first [tile type 2]
hsb: stitch at (105,133)-(169,133) reported success but the pieces are still separate
```

* **`hsb: road refusal xN:`** — the top refusal reason and how often it
  happened. "No suitable road" in the hundreds is a bank that will not bridge;
  "Site unsuitable for tunnel entrance" is terrain with no entrance for one.
* **`hsb: tune ...`** is the *first* line of every build, naming the
  configuration. Two logs with the same numbers are only comparable if the line
  above them agrees.
* **`HSB_TUNE_ROAD_RUN`** is the biggest lever on a fragmented network: it is
  the tiles per straight piece before a corridor turns. Measured on a 256-tile
  map, 64 gave 1906 road tiles in 25 components and 16 gave 1278 in 49 — shorter
  pieces build more road and connect less of it. On a 4096 map the default was
  tuned against exactly that trade-off.
* **`HSB_DUMP_NETWORK=/tmp/net.txt`** writes the network as a character grid,
  one character per 4×4 tiles, with `R` a corridor the builder laid and `^` a
  town street. Everything else is a number, and a number cannot show that a
  corridor runs through the middle of nowhere.

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
* The network phase needs `competitors >= 2`. The company that owns the network
  takes one slot; with only one the player opens the save as a spectator with
  nothing to build, which no exit code reveals.
* `build_on_slopes` and `autoslope` are pinned **before** `GenerateWorld`,
  because that is the only point at which `_settings_game` takes them from
  `_settings_newgame`. Setting them afterwards is a silent no-op.
* A town is only ever given the road tiles **inside its own claim**. The trunk
  stays the company's, which is what lets the railway cross it; the town's
  approach is the town's, which is what makes the town grow along the road that
  reaches it.

## Manual fallback (no builder)

The classic GUI flow still works: Scenario Editor → Load heightmap
(clockwise) → Town Generation → Load from file (`towns.json`) → draw
rivers per `rivers.json` waypoints → place industries per `industries.json` →
draw roads per the corridor files → build rail, airports and docks → bake
settings (start year, climate, currency) → save.

Turn on **Build on slopes** and **Autoslope** in the editor's construction
settings first, or the road tool refuses every sloped tile the way the builder
would without them.
