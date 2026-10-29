# OSM reference and OSM-derived network

Two scripts turn OpenStreetMap data into scenario geometry:

1. `scripts/osm-reference.py` reads Geofabrik state extracts (or local
   `.osm.pbf` files) and writes a full-resolution reference: every way's real
   node list, stations, signals, industry/forest/water polygons.
2. `scripts/build-osm-network.py` reads the raw lat/lon nodes from that
   reference and writes the simplified inputs the builder consumes:

| Output | Builder key | Selection |
|--------|-------------|-----------|
| `autobahn-osm.json` | `autobahnen` | `highway=motorway`, both carriageways deduplicated |
| `rail-osm.json` | `rail` | `railway=rail`, `usage=main`, no `service` |
| `stations-osm.json` | `stations` | `railway=station`, one per town, DB category or Hauptbahnhof name |
| `osm-import.json` | — | source URLs, sizes, SHA-256, selection rules, counts |

The reference itself is generated data and is not committed. The derived files
are an ODbL Derivative Database, © OpenStreetMap contributors — see
`docs/ATTRIBUTION.md`.

## Run it

```sh
pip install pyosmium requests

# Full German reference into /tmp/osm/work (downloads ~7 GB, cached in /tmp/osm).
python3 scripts/osm-reference.py --self-test
python3 scripts/osm-reference.py --work-dir /tmp/osm/work --keep-work

# Any region: point at one or more local extracts. The map window must match
# the heightmap/towns/manifest bounds and size.
python3 scripts/osm-reference.py --pbf region.osm.pbf \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 --size 4096 \
  --out-dir /tmp/osm/ref

# Derive the builder inputs.
python3 scripts/build-osm-network.py \
  --reference-dir /tmp/osm/ref \
  --towns /tmp/towns.json --out-dir /tmp/osm/geodata \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 --size 4096

# Then either pass the files explicitly, or use --osm with that directory.
python3 scripts/build-manifest.py ... \
  --autobahnen /tmp/osm/geodata/autobahn-osm.json \
  --rail /tmp/osm/geodata/rail-osm.json \
  --stations /tmp/osm/geodata/stations-osm.json
# or:
python3 scripts/build-manifest.py ... --osm /tmp/osm/geodata

python3 scripts/validate-scenario.py ... --osm /tmp/osm/geodata
```

`build-osm-network.py --check` compares the output files with what the current
reference would produce, so a committed derived file cannot silently go stale.

## Projection

The heightmap is resampled to a **linear geographic grid** by
`scripts/build-heightmap.py`; the builder's `HsbTileForBounds()` maps latitude
linearly and transposes the axes (`x` south, `y` east, matching
`heightmap_rotation: clockwise`). Both OSM scripts use that same mapping:

```text
tile_x = lround((lat_max - lat) / (lat_max - lat_min) * (size - 1))
tile_y = lround((lon - lon_min) / (lon_max - lon_min) * (size - 1))
```

Do not read the reference's `tile` field as input: `build-osm-network.py`
reprojects every node so a reference written for another window cannot leak
into the scenario.

## Selection and simplification

* Motorways are chained by shared endpoints, choosing the straightest
  continuation at junctions. Parallel carriageways are dropped when 70 % of
  their tiles are already covered.
* Rail keeps `railway=rail`, `usage=main`, empty `service`; yard, siding, spur
  and crossover track is excluded. Lines are grouped by OSM `ref` where present
  so unrelated lines do not join at a shared station.
* Stations keep `type=station` (not `halt`) with a DB station category or a
  Hauptbahnhof name; underground/S-Bahn variants are skipped. One station per
  town is chosen by name and distance (18 km maximum).
* Corridors are simplified in tile space with Ramer–Douglas–Peucker
  (`--tolerance-tiles`, default 2) and resampled to at most
  `--max-step-tiles` (default 48) between waypoints. The builder routes each
  leg with the player's own road/rail tools, so these are centrelines, not
  surveyed track.

## Limits

* `motorway_link` ramps are not in the reference and are not invented.
* OSM station matching is name/distance based; a town whose mainline station
  is farther than `--max-station-km` is left without an entry and keeps the
  town-centre station search.
* The reference extractor needs `pyosmium`; only `--self-test` runs without it.
* This is geographic data, not a playable network: the router still decides
  what terrain, water and town buildings allow.
