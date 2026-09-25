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
  "seed": 20250924,
  "industry_search_radius": 32,
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
* `seed`: optional, default 20250924. Fixed seed ⇒ deterministic rebuilds
  (up to engine RNG tail metadata).
* `industry_search_radius`: optional, default 32, range 1–512. Spiral fallback
  radius in tiles around each industry anchor.
* `save`: scenario filename written into the `scenario/` dir.

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
* `width`: brush width in tiles (1–3 at 4096; scale to your map size).
* `waypoints`: `[[lat,lon], ...]` polyline, interpolated in lat/lon
  (`dist/0.002` steps), painted via the same bounds mapping as industries.
* Carved **after** terrain gen, **before** towns. `FixSlopes()` runs after.

## heightmap PNG

Grayscale `L`, square (`W=H`), power-of-2 in 64–4096. Sea = gray 0, land = 1..255.
Columns and rows are linearly spaced over the requested longitude and latitude
bounds. Built by `scripts/build-heightmap.py` (Terrarium DEM → bilinear
resample → normalize).
