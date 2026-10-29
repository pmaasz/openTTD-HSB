# Attribution

This file records the upstream data licenses for generated scenario inputs.
It is not a replacement for `LICENSE.md`, which covers the code.

## OpenStreetMap

`scripts/osm-reference.py` reads OpenStreetMap data via Geofabrik extracts (or
local `.osm.pbf` files). `scripts/build-osm-network.py` derives
`autobahn-osm.json`, `rail-osm.json` and `stations-osm.json` from it.

Those files are a **Derivative Database** of OpenStreetMap data:

* © OpenStreetMap contributors
* License: **Open Database License (ODbL) 1.0**
* https://www.openstreetmap.org/copyright
* https://opendatacommons.org/licenses/odbl/1-0/

The ODbL covers those files and the geometry derived from them, not the
repository's GPLv2 code or the non-OSM data.

**Modification statement.** Source ways were filtered by tag, split at
junctions, chained by shared endpoints, deduplicated when parallel
carriageways/tracks covered the same tiles, simplified in the map's tile space
(Ramer–Douglas–Peucker, default tolerance 2 tiles; maximum waypoint gap 48
tiles), and reprojected from WGS84 to the builder's linear, transposed tile
grid. Station points are selected one per modelled town and are otherwise
unmodified coordinates. OpenTTD then routes between the simplified waypoints,
so the final tiles are a further lossy representation.

The raw reference output is generated, git-ignored and not packaged. Keep
`osm-import.json` with the derived files: it records the Geofabrik URLs, byte
sizes and SHA-256 hashes the derivation came from.

## Elevation and water

`scripts/build-heightmap.py` reads AWS Terrain Tiles (Terrarium encoding),
produced by the Mapzen/Tilezen *joerd* pipeline:

* Europe land: EU-DEM, a Copernicus product (© European Union, Copernicus data
  and information policy, Regulation (EU) No 1159/2013). Not endorsed by the
  European Union.
* Sea/bathymetry: ETOPO1 (NOAA, public domain).
* High-Alpine tail: SRTM / USGS 3DEP (USGS, public domain, credit requested).

**Modification statement.** Tiles are mosaicked, cropped to the requested
bounds, resampled to a linear geographic grid, and normalised to 8-bit
grayscale by `scripts/build-heightmap.py`. The scenario then carves rivers,
sets sea level and places content on the resulting terrain.

## Town coordinates

Extension-town coordinates may be geocoded through OpenStreetMap Nominatim
(ODbL, © OpenStreetMap contributors) by `scripts/build-towns.py --geocode`.
Record the query log with any generated `cities.csv`.
