# Formats — HSB manifest + data files

All paths in the manifest are absolute (written by
`scripts/build-manifest.py` for this machine). Bounds MUST agree across
heightmap, towns, industries, and rivers — they define one shared lat/lon →
tile mapping.

## Manifest (`HSB_BUILD` points here)

```json
{
  "heightmap": "/abs/path/map-4096.png",
  "towns": "/abs/path/towns.json",
  "industries": "/abs/path/industries.json",
  "rivers": "/abs/path/rivers.json",
  "bounds": {"lat_min": 47.0, "lat_max": 55.5,
             "lon_min": 4.0,  "lon_max": 15.5},
  "start_year": 2025,
  "heightmap_height": 12,
  "heightmap_rotation": "clockwise",
  "snow_line_height": 10,
  "seed": 20250924,
  "industry_search_radius": 32,
  "max_loan": 1500000,
  "competitors": 0,
  "currency": "EUR",
  "save": "output.scn"
}
```

* `heightmap`, `towns`: resolved by **basename** through the engine file
  search (`scenario/heightmap` in the personal dir). Copy both files there
  before running (see `docs/SCENARIO.md`).
* `industries`, `rivers`: read directly from these absolute paths.
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
  scenario is meant to be played.
* `currency`: optional, default empty (inherit). A 3-letter code from the
  engine table (`src/currency.h`): `GBP`, `EUR`, `USD`, … An unknown code is a
  build error, not a silent fallback.
* `save`: scenario filename written into the `scenario/` dir.

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
| NewGRF persistent storage | wrapped in gameloop scope (see below) |

## Build log audit line

A successful build ends with one machine-readable line, which the MCP server
parses into `run_builder`'s `audit` field. The numbers below are illustrative
for a 4096 map, not a recorded run; paste in your own line.

```text
hsb: audit: towns=124 towns_on_water=0 industries=73 water=2810000 \
  sea_fraction=0.168 water_bodies=41 isolated_water_tiles=2 \
  water_unreachable=310 carved=18800 carved_reachable=18800
```

* `towns_on_water` must be 0.
* `carved_reachable` should equal `carved`; a gap means part of the carved
  river system is a closed pond no ship can enter.
* `water_unreachable` counts water the builder did not carve that is not
  connected to open water — usually DEM lakes, occasionally worth a look.
* `isolated_water_tiles` counts single-tile ponds: shredding of a river into
  disconnected stamps.

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
`OWNER_NONE` with restrictions bypassed and `_generating_world = true`.

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
