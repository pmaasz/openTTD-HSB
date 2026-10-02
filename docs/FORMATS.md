# Formats — HSB manifest + data files

All paths in the manifest are absolute (written by
`scripts/build-manifest.py` for this machine). Bounds MUST agree across
heightmap, towns, industries, rivers and every network file — they define one
shared lat/lon → tile mapping.

## Manifest (`HSB_BUILD` points here)

```json
{
  "heightmap": "/abs/path/map-4096.png",
  "towns": "/abs/path/towns.json",
  "industries": "/abs/path/industries.json",
  "rivers": "/abs/path/rivers.json",
  "roads": "/abs/path/roads.json",
  "autobahnen": "/abs/path/autobahn.json",
  "rail": "/abs/path/rail.json",
  "airports": "/abs/path/airports.json",
  "harbours": "/abs/path/harbours.json",
  "with_rail": true,
  "with_fleet": true,
  "build_on_slopes": true,
  "autoslope": true,
  "bounds": {"lat_min": 47.0, "lat_max": 55.5,
             "lon_min": 4.0,  "lon_max": 15.5},
  "start_year": 2025,
  "heightmap_height": 12,
  "heightmap_rotation": "clockwise",
  "snow_line_height": 10,
  "seed": 20250924,
  "industry_search_radius": 32,
  "max_loan": 1500000,
  "competitors": 2,
  "currency": "EUR",
  "network_money": 50000000,
  "bus_towns": 24,
  "trains": 12,
  "save": "output.scn"
}
```

* `heightmap`, `towns`: resolved by **basename** through the engine file
  search (`scenario/heightmap` in the personal dir). Copy both files there
  before running (see `docs/SCENARIO.md`).
* `industries`, `rivers` and every network file: read directly from these
  absolute paths.
* `bounds`: required. `lat_min < lat_max`, `lon_min < lon_max`.
* `rivers`: optional — omit the key (or null/empty) to skip waters.
* `heightmap_height`: optional, default 12.
* `heightmap_rotation`: optional, `clockwise` (default) or
  `counter-clockwise`. Must match the rotation you would use by hand.
* `snow_line_height`: optional, default 0 (derive from `snow_coverage`),
  range 0–255. The engine clamps it to at least 2.
* `seed`: optional, default 20250924, range 0–4294967294. Fixed seed ⇒
  deterministic rebuilds (up to engine RNG tail metadata).
* `industry_search_radius`: optional, default 32, range 1–512. Spiral fallback
  radius in tiles around each industry anchor.
* `max_loan`: optional, default 0 (engine default), range 0–4000000000.
* `competitors`: optional, default 0, range 0–15. **0 means no company slots**,
  which drops the player onto the map as a spectator — raise it if the
  scenario is meant to be played. **2 is the minimum once a network is built**,
  because the infrastructure company that owns it takes one slot.
* `currency`: optional, default empty (inherit). A 3-letter code from the
  engine table (`src/currency.h`): `GBP`, `EUR`, `USD`, … An unknown code is a
  build error, not a silent fallback.
* `save`: scenario filename written into the `scenario/` dir.

### Network phase

Every network key is optional and **absent or `null` means the phase is off**.
Name one and the network phase runs; the whole set is data-driven, so a
manifest that names none builds byte-identically to one from before the
network phase existed.

| Key | File shape | Effect |
|-----|-----------|--------|
| `roads` | `[{name?, waypoints}]` | town-to-town corridors, routed first-to-last waypoint |
| `autobahnen` | same | trunk corridors, routed before the town network |
| `rail` | same | main and branch lines |
| `airports` | `[{name, xy}]` | one airport per entry, searched out to 40 tiles |
| `harbours` | same | one dock per entry, searched from water out to 40 tiles |

| Key | Default | Range | Effect |
|-----|---------|-------|--------|
| `with_rail` | `true` | bool | Build the rail network when `rail` is named. Naming `rail` and setting `with_rail: false` is a build error: track without stations and depots looks broken. |
| `with_fleet` | `true` | bool | Buy rolling stock. With no `roads` and no `rail` there is nothing to run on, and that is a build error. |
| `build_on_slopes` | `true` | bool | `construction.build_on_slopes`. Off ⇒ every sloped tile refuses a road and no road network is possible. |
| `autoslope` | `true` | bool | `construction.autoslope`. Off ⇒ `CheckRailSlope` answers "must remove railway track first" everywhere. |
| `network_money` | 50000000 | 0–4000000000 | Cash handed to the borrowed infrastructure company **for the build**. It is given back before the save, so it is a scaffold and not a starting balance. |
| `bus_towns` | 24 | 0–1024 | Towns that get a depot, two stops and a bus, largest first. |
| `trains` | 12 | 0–1024 | Stations that get a spur depot and a train, biggest town first. |

Two settings are always pinned when a network is built and have no manifest
key: `construction.crossing_with_competitor` and
`construction.road_stop_on_town_road`. Both are true, and both are needed — the
railway crosses the scenario's town-owned roads constantly, and a bus stop on a
town street is impossible without the second.

The four construction settings are set **before** `GenerateWorld`, because that
is the only point at which `_settings_game` still takes them from
`_settings_newgame`. Setting them afterwards is a silent no-op, and the symptom
is a refusal tally full of "Site unsuitable for tunnel entrance" rather than an
error.

### What is pinned and why

A scenario's newgame settings win over the player's `openttd.cfg`, so anything
the manifest does not set is inherited from **the builder's own config file** —
machine state, not intent. Two release-blocking bugs have come from that class
of leak, so the builder pins what it can and rejects what it cannot:

| Pinned | How |
|--------|-----|
| start year, snow line, seed, height, rotation | always from the manifest |
| `difficulty.max_loan` | `max_loan`, when non-zero |
| `difficulty.max_no_competitors` | `competitors`, always |
| `locale.currency` | `currency`, when set; unknown code is an error |
| `order.selectgoods` | forced **off** (on = industries never deliver to a station no vehicle has loaded at, a permanent deadlock for new full-load routes) |
| construction difficulty | the four above, when a network is built |
| NewGRF persistent storage | wrapped in gameloop scope (see below) |

## Build log audit lines

A successful build ends with two machine-readable lines. The MCP server parses
them into `run_builder`'s `audit` field, prefixing the network keys with
`network_` so the two merge without either shadowing the other's `towns`. The
numbers below are illustrative for a 4096 map, not a recorded run; paste in your
own lines.

```text
hsb: audit: towns=124 towns_on_water=0 industries=73 water=2810000 \
  sea_fraction=0.168 water_bodies=41 isolated_water_tiles=2 \
  water_unreachable=310 carved=18800 carved_reachable=18800

hsb: network audit: roads=73226 orphan_roads=0 road_components=1 \
  largest_road=71890 towns=124 towns_on_network=124 rail=259 \
  rail_components=37 largest_rail=400 stations=61 stations_with_rail=61 \
  bus_stops=48 depots=48 airports=27 docks=21 vehicles=35 consists=35 \
  vehicles_routed=35 vehicles_parked=0
hsb: intercity road: a bus from the largest town (pop 1222) reaches 124 of 124 towns
```

The network line is **absent** when no network file is named, so a gate can tell
a terrain-only build from one that ran the network.

* `towns_on_water` must be 0.
* `carved_reachable` should equal `carved`; a gap means part of the carved
  river system is a closed pond no ship can enter.
* `water_unreachable` counts water the builder did not carve that is not
  connected to open water — usually DEM lakes, occasionally worth a look.
* `isolated_water_tiles` counts single-tile ponds: shredding of a river into
  disconnected stamps.
* `road_components` should be 1. More means the trunk is in pieces, which is a
  different question from the one below.
* `towns_on_network` counts a town as connected when any road within 8 tiles is
  on the largest component. It is cheap and it is **not** the same thing as
  being drivable to.
* The `intercity road` line is the question that decides whether the buses are
  intercity transport or a municipal shuttle: a flood from the biggest town over
  every road tile, town streets included.
* `vehicles_parked` must be 0. Having orders is not the same as running.

## towns.json — OpenTTD town-import format

Array of `{name, population, city, x, y}`:

* `x = row/(H-1)`, `y = col/(W-1)` — proportions, axes **swapped** vs image
  editors (per upstream `docs/importing_town_data.md`).
* Pixel mapping (must match `build-heightmap.py --bounds`):
  `col = (lon-lon_min)/(lon_max-lon_min)*(W-1)`,
  `row = (lat_max-lat)/(lat_max-lat_min)*(H-1)`.
* Built by `scripts/build-towns.py` from `cities.csv`
  (`name,lat,lon,pop_real,city,source`).

## industries.json — placements

Array of `{type, lat, lon, near?, ...}`:

* `type`: industry display name as shown in-game (NewGRF-dependent, e.g.
  GIST: `"Power Plant"`, `"Oil Refinery"`). The builder resolves names
  against the enabled industry set at runtime and logs every available name.
* `lat`, `lon`: anchor. The engine tries the anchor tile first, then spirals
  out to `industry_search_radius` (default 32 tiles).
* `near`: free-form label for logs only.
* Extra keys are ignored (e.g. `extension`, `note`).

Placement mirrors the editor GUI (`industry_gui.cpp OnPlaceObject`): built as
`OWNER_NONE` with restrictions bypassed and `_generating_world = true`. Those
three overrides are scoped to the placement loop — they used to live to the end
of the build, which handed the whole network phase an `OWNER_NONE` company.

A `Forest` entry additionally plants a dense tree block around its anchor.

## rivers.json — carved waterways

Array of `{name, class, width, waypoints}`:

* `class`: `river` (inland, `MakeRiver`) or `sea` (tidal, `MakeSea` — ports work).
* `width`: brush width in tiles per axis (1–3 at 4096; scale to your map size):
  1 → 1×1, 2 → 2×2, 3 → 3×3.
* `waypoints`: `[[lat,lon], ...]` polyline, interpolated in lat/lon
  (`dist/0.002` steps), painted via the same bounds mapping as industries.
* Consecutive samples are joined by a **4-connected staircase walk**, not a
  straight stamp. A straight stamp leaves tiles touching only at their corners
  wherever the river runs diagonally, which looks connected on the map and is
  not: a chain of ponds a ship cannot enter.
* After carving, every body that does **not** touch open water within 96 tiles
  gets a channel cut to the nearest open water. Author waypoints that end *near*
  the coast, not exactly on it — the DEM coastline often sits a tile or two
  inland, so without this step every river comes out a closed pond. Watch
  `carved_reachable` in the audit line.
* Carved **after** terrain gen, **before** towns. `FixSlopes()` runs after.

## Corridors — roads.json, autobahn.json, rail.json

Array of `{name?, waypoints: [[lat,lon], ...]}`, at least two waypoints, all
inside `bounds`:

```json
[{"name": "A1", "waypoints": [[54.0, 6.5], [52.0, 10.0], [49.0, 9.0]]}]
```

Waypoints are connected in order; each endpoint is first **snapped** up to 60
tiles onto ground that takes a road without a structure being built first (a
town centre is solid with houses, so a corridor aims at the edge of a town and
the town then grows along it). An endpoint with nothing buildable within 60
tiles is counted as `unsnappable` rather than silently routing into the water.

Corridors are cut into straight pieces of at most `HSB_TUNE_ROAD_RUN` tiles per
axis, each with its own right-angle corner — the geometry the player's autoroad
tool produces, and the reason a diagonal run does not double in length.

`roads.json` is typically generated from `towns.json`; `autobahn.json` and
`rail.json` are usually hand-authored. Bounds are checked against the manifest
before any routing starts.

## airports.json, harbours.json — named places

Array of `{name, xy: [lat, lon]}`:

```json
[{"iata": "FRA", "name": "Frankfurt", "xy": [50.1, 8.6]}]
```

Extra keys are ignored. These are *data*, not a rule about which cities deserve
one — the position is the real position, because an airport is a big flat field
and belongs outside its town. The build searches outwards from `xy` for a site
the command itself accepts, which is the only honest test.

## heightmap PNG

Grayscale `L`, square (`W=H`), power-of-2 in 64–4096. Sea = gray 0, land = 1..255.
Columns and rows are linearly spaced over the requested longitude and latitude
bounds. Built by `scripts/build-heightmap.py` (Terrarium DEM → bilinear
resample → normalize).

The sea threshold is `--sea-level` metres, default `0`. SRTM and similar DEMs do
not put the coastline at exactly 0 m — tidal flats and reclaimed land are
recorded as land and render as dry ground the engine then refuses to build on.
Pass a small negative value (`--sea-level -2`) to drop them; the float32 master
TIFF keeps the raw elevations, so nothing is lost.

## Router tunables (environment, not the manifest)

Read once per build and printed in the `hsb: tune` line **before anything is
measured**, so a log always says which configuration produced it. Defaults are
the values that were compiled in before any of this existed, so an empty
environment routes what it always routed.

| Variable | Default | Range | Effect |
|----------|---------|-------|--------|
| `HSB_TUNE_ROAD_RUN` | 64 | 1–512 | Tiles per straight piece before a corridor turns. Moves the rail as well as the road. |
| `HSB_TUNE_ROAD_SNAP` | 8 | 1–64 | How far a corner or endpoint may be nudged onto buildable ground. |
| `HSB_TUNE_CORNER_FAN` | 8 | 0–64, or `"0,-3,3"` | Corner offsets to try, in order. A comma-separated list sets the offsets outright. |
| `HSB_TUNE_ROAD_TRACE` | 3 | 0–1000 | Failing links to trace in full. 0 turns the trace off, which is what a sweep wants. |
| `HSB_DUMP_NETWORK` | unset | path | Write the network as a character grid, one character per 4×4 tiles. |
| `HSB_FAST` | unset | — | Simulate as fast as the CPU allows. Test and CI only. |

Out-of-range values are clamped and the clamp is logged; a value that is not a
number falls back to the default rather than becoming 0, because a run of 0 turns
every chunk into a corner and a snap of 0 snaps nowhere.
