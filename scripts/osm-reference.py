#!/usr/bin/env python3
"""osm-reference.py - build a full-resolution OpenStreetMap reference network.

    python3 scripts/osm-reference.py                        # all 16 German states
    python3 scripts/osm-reference.py --states nordrhein-westfalen
    python3 scripts/osm-reference.py --out-dir /tmp/osmref --skip-download
    python3 scripts/osm-reference.py --pbf /data/region.osm.pbf
    python3 scripts/osm-reference.py --self-test

This is the full-resolution reference used by scripts/build-osm-network.py to
derive autobahn/rail/station inputs. It is not a scenario file: the raw output
is many gigabytes, so keep it out of git.

Why
---
This script pulls the real network out of OpenStreetMap so hand-built geometry
can be checked against it: is the rail network the right length, are the
motorways in the right places, are the stations in the right towns. Everything
is emitted at full resolution - the real node coordinates of every way, not a
straight line between the endpoints - because a reference you cannot overlay is
worthless.

Data source: Geofabrik state extracts, because overpass-api.de answers 406 in
this environment. Each state is ~100-900 MB, the sixteen together ~7 GB.
Downloads are cached in --cache-dir (default /tmp/osm) and reused.

Map projection (must match the builder)
---------------------------------------
The heightmap is resampled to a linear geographic grid by
scripts/build-heightmap.py, and the builder's HsbTileForBounds() maps latitude
linearly with transposed axes (x south, y east, matching the default clockwise
heightmap rotation). This reference uses the same mapping:

    tile_x = lround((lat_max - lat) / (lat_max - lat_min) * (size - 1))
    tile_y = lround((lon - lon_min) / (lon_max - lon_min) * (size - 1))

Every emitted point carries both lat/lon and tile, so downstream code never has
to redo this arithmetic; way geometry is [[lat, lon], ...] in `nodes`, and
--way-tiles adds the parallel `tiles` array. The bounds and size are CLI
options and must match the manifest.

Determinism
-----------
No timestamps anywhere. Each output file records the source URL, the byte size
and the SHA-256 of the input extract instead, so two runs over the same input
produce byte-identical files (check with sha256sum). Arrays are sorted by OSM
id then object kind, floats are rounded to a fixed number of decimals, and JSON
is written with sorted keys and compact separators.

Resume
------
Extracting all sixteen states takes about 90 minutes and the merge only happens
afterwards, so an interrupted run used to lose every shard it had written. Each
state now writes a small manifest (<state>.state.json) into --work-dir when its
extraction finishes, naming its shards and every number it contributed. A later
run over the same --work-dir reuses a state whose manifest is present, whose
shards are all still there and whose extract has the same byte size and
SHA-256; anything else is extracted again. The output is the same either way.
--no-resume ignores the manifests, and a work directory given with --work-dir
is never deleted (the automatic temporary one still is, unless --keep-work).

Memory
------
Each state is read once and written straight out as per-state JSONL shards
(so peak memory is one libosmium node-location index, not a whole country); the shards are then merged by a k-way streaming merge into the final
sorted JSON files.
"""

from __future__ import annotations

import argparse
import hashlib
import heapq
import json
import math
import os
import shutil
import sys
import tempfile
import time

try:
    import requests
except ImportError:
    sys.exit("osm-reference.py needs 'requests': pip install requests")

# pyosmium is needed to *extract*, not to import: scripts/osm-reference-stats.py
# imports this module for the projection and the tag lists below, and it should
# not have to drag in a C++ extension to ask for a tile coordinate. So the
# import failure is deferred to extract_state(), which is where it bites.
try:
    import osmium
except ImportError:
    osmium = None
    _SimpleHandler = object
else:
    _SimpleHandler = osmium.SimpleHandler


# --------------------------------------------------------------------------
# map projection (imported by scripts/osm-reference-stats.py)
# --------------------------------------------------------------------------

LAT_MIN, LAT_MAX = 47.0, 55.5
LON_MIN, LON_MAX = 4.0, 15.5
MAP_TILES = 4096

PROJECTION = {}


def configure(lat_min, lat_max, lon_min, lon_max, map_tiles):
    """Set the map window this reference is generated for."""
    global LAT_MIN, LAT_MAX, LON_MIN, LON_MAX, MAP_TILES
    if not (math.isfinite(lat_min) and math.isfinite(lat_max)
            and math.isfinite(lon_min) and math.isfinite(lon_max)
            and -90.0 <= lat_min < lat_max <= 90.0
            and -180.0 <= lon_min < lon_max <= 180.0):
        sys.exit("[osm-ref] bounds must be finite WGS84 intervals")
    if map_tiles < 64 or map_tiles > 4096 or map_tiles & (map_tiles - 1):
        sys.exit("[osm-ref] --size must be a power of two in 64..4096")
    LAT_MIN, LAT_MAX = lat_min, lat_max
    LON_MIN, LON_MAX = lon_min, lon_max
    MAP_TILES = map_tiles
    PROJECTION.clear()
    PROJECTION.update({
        "map_tiles": MAP_TILES,
        "lat_min": LAT_MIN,
        "lat_max": LAT_MAX,
        "lon_min": LON_MIN,
        "lon_max": LON_MAX,
        "tile_x": "lround((lat_max - lat) / (lat_max - lat_min) * (map_tiles - 1))",
        "tile_y": "lround((lon - lon_min) / (lon_max - lon_min) * (map_tiles - 1))",
        "axes": "tile x grows SOUTH and tile y grows EAST; the heightmap is "
                "loaded with heightmap_rotation = clockwise, so the map is the "
                "window transposed, not a raster of it",
        "rounding": "round-half-away-from-zero, the same std::lround as "
                    "HsbTileForBounds() in builder/hsb_builder_math.h",
        "projection_note": "latitude is mapped linearly: build-heightmap.py "
                           "resamples the Mercator source into a linear "
                           "geographic grid before writing the PNG",
        "checks": {
            "Hamburg (53.55, 9.99)": [939, 2133],
            "Hamburg Hbf (53.5527, 10.0064)": [938, 2139],
            "Berlin (52.52, 13.4055)": [1436, 3349],
            "Berlin Hbf (52.5251, 13.3694)": [1433, 3336],
            "München (48.1372, 11.5755)": [3547, 2698],
            "Flensburg (54.7753, 9.4017)": [349, 1923],
            "Konstanz (47.6633, 9.175)": [3775, 1843],
        },
        "cross_check": "scripts/build-towns.py stores the same pair of "
                       "fractions (x = latitude share from the north, "
                       "y = longitude share) on the same window",
    })


configure(LAT_MIN, LAT_MAX, LON_MIN, LON_MAX, MAP_TILES)

COORD_DECIMALS = 6
KM_DECIMALS = 4
EARTH_RADIUS_KM = 6371.0088


def tile_xy(lat, lon):
    """Map tile for a coordinate, or None when outside the map bounds."""
    if not inside_bounds(lat, lon):
        return None
    x = int(math.floor((LAT_MAX - lat) / (LAT_MAX - LAT_MIN) * (MAP_TILES - 1) + 0.5))
    y = int(math.floor((lon - LON_MIN) / (LON_MAX - LON_MIN) * (MAP_TILES - 1) + 0.5))
    if 0 <= x < MAP_TILES and 0 <= y < MAP_TILES:
        return [x, y]
    return None


def km_per_tile(lat):
    """Rough km per tile across the map at this latitude, the smaller axis.

    Only used to turn a search radius in km into a tile reach, so it takes the
    east-west axis: at 51 deg N a tile is ~0.20 km wide and ~0.23 km tall, and
    using the smaller one means a lookup never walks off the map early.
    """
    return ((LON_MAX - LON_MIN) * 111.320
            * math.cos(math.radians(lat)) / MAP_TILES)


def latlon(lat, lon):
    return [round(lat, COORD_DECIMALS), round(lon, COORD_DECIMALS)]


def self_test():
    """Check the recorded projection examples against tile_xy() itself.

    PROJECTION["checks"] and the docstring above it are hand-written claims, and
    they had already drifted from the code: the checks were computed for a
    transposed projection and one of them (München) was wrong in both. The
    projection goes into the header of every output file, so a claim that is out
    of step with the code is a wrong claim on six gigabytes of data. This runs
    the examples back through the function and fails loudly.
    """
    if (LAT_MIN, LAT_MAX, LON_MIN, LON_MAX, MAP_TILES) != (47.0, 55.5, 4.0, 15.5, 4096):
        print("[osm-ref] self-test skipped: checks are recorded for the default map window")
        return 0
    bad = 0
    for label, expected in sorted(PROJECTION["checks"].items()):
        lat, lon = (float(v) for v in label.rsplit("(", 1)[1].rstrip(")").split(", "))
        got = tile_xy(lat, lon)
        if got != list(expected):
            bad += 1
            print("[osm-ref] FAIL %-30s recorded %s, tile_xy gives %s"
                  % (label, expected, got))
    print("[osm-ref] self-test: %d of %d projection checks agree"
          % (len(PROJECTION["checks"]) - bad, len(PROJECTION["checks"])))
    return 1 if bad else 0


def haversine_km(a, b) -> float:
    """Great-circle distance in km between two (lat, lon) pairs."""
    p1 = math.radians(a[0])
    p2 = math.radians(b[0])
    dp = p2 - p1
    dl = math.radians(b[1] - a[1])
    h = math.sin(dp / 2.0) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2.0) ** 2
    return 2.0 * EARTH_RADIUS_KM * math.asin(min(1.0, math.sqrt(h)))


def polyline_km(points) -> float:
    return sum(haversine_km(points[i - 1], points[i])
               for i in range(1, len(points)))


def ring_area_km2(ring) -> float:
    """Shoelace area in an equirectangular projection local to the ring.

    Every ring is counted, so a multipolygon with several outers sums all of
    them (OSM has no holes here: libosmium returns outer rings only, so a hole
    left untagged in the source counts as solid - the usual convention).
    """
    if len(ring) < 3:
        return 0.0
    lat0 = sum(p[0] for p in ring) / len(ring)
    lon0 = sum(p[1] for p in ring) / len(ring)
    kx = math.radians(1.0) * EARTH_RADIUS_KM * math.cos(math.radians(lat0))
    ky = math.radians(1.0) * EARTH_RADIUS_KM
    pts = [((p[1] - lon0) * kx, (p[0] - lat0) * ky) for p in ring]
    acc = 0.0
    for i, (x1, y1) in enumerate(pts):
        x2, y2 = pts[(i + 1) % len(pts)]
        acc += x1 * y2 - x2 * y1
    return abs(acc) / 2.0


def inside_bounds(lat, lon) -> bool:
    return LAT_MIN <= lat <= LAT_MAX and LON_MIN <= lon <= LON_MAX


# --------------------------------------------------------------------------
# inputs: which states, which tags
# --------------------------------------------------------------------------

GEOFABRIK_URL = "https://download.geofabrik.de/europe/germany/%s-latest.osm.pbf"

STATES = [
    "baden-wuerttemberg", "bayern", "berlin", "brandenburg", "bremen",
    "hamburg", "hessen", "mecklenburg-vorpommern", "niedersachsen",
    "nordrhein-westfalen", "rheinland-pfalz", "saarland", "sachsen",
    "sachsen-anhalt", "schleswig-holstein", "thueringen",
]

# Other names a state extract may already have in the cache under.
CACHE_ALIASES = {
    "nordrhein-westfalen": ["nrw", "nordrhein_westfalen", "nw"],
    "mecklenburg-vorpommern": ["mecklenburg_vorpommern", "mv"],
    "rheinland-pfalz": ["rheinland_pfalz", "rlp"],
    "sachsen-anhalt": ["sachsen_anhalt"],
    "schleswig-holstein": ["schleswig_holstein", "sh"],
    "baden-wuerttemberg": ["baden_wuerttemberg", "bw"],
}

CATEGORY_FILES = {
    "rail": ("osm-rail.json", "ways"),
    "road": ("osm-road.json", "ways"),
    "signals": ("osm-signals.json", "signals"),
    "stations": ("osm-stations.json", "stations"),
    "industry": ("osm-industry.json", "areas"),
    "forest": ("osm-forest.json", "polygons"),
    "water": ("osm-water.json", "features"),
}
CATEGORIES = list(CATEGORY_FILES)
# libosmium hands out nodes, then ways, then assembled areas, each in ascending
# id order. Shards are written per (category, kind) so every one of them is
# sorted by construction; merging them all with the key (id, kind) then yields
# one globally id-ascending array, with the kind only breaking ties between
# OSM's separate id spaces.
KIND_ORDER = ("node", "way", "area")

RAIL_CLASSES = ("light_rail", "narrow_gauge", "rail", "tram")
RAIL_DISCARD = ("abandoned", "construction", "dismantled", "disused",
                "miniature", "monorail", "no", "platform", "platform_edge",
                "preserved", "proposed", "razed", "subway")
ROAD_CLASSES = ("living_street", "motorway", "primary", "residential", "road",
                "secondary", "tertiary", "trunk", "unclassified")
ROAD_DISCARD_NOTE = (
    "highway=*_link slip roads are NOT included (they would add ~17k ways of "
    "ramps and nothing else); service, footway, path, cycleway, track, "
    "pedestrian, steps, construction and proposed are discarded")

SIGNAL_KINDS = {
    "signal": "signal",
    "signal_box": "signal_box",
    "stop": "stop",
    "buffer_stop": "buffer_stop",
}
STATION_KINDS = {"station": "station", "halt": "halt"}

INDUSTRY_PAIRS = {
    ("landuse", "industrial"): "landuse:industrial",
    ("landuse", "railway"): "landuse:railway",
    ("man_made", "works"): "man_made:works",
}
INDUSTRY_RAILWAY = {"industrial": "railway:industrial"}

FOREST_PAIRS = {
    ("natural", "wood"): "natural:wood",
    ("landuse", "forest"): "landuse:forest",
    ("landcover", "forest"): "landcover:forest",
}

WATER_AREA_PAIRS = {
    ("natural", "water"): "natural:water",
    ("waterway", "riverbank"): "waterway:riverbank",
}
WATER_LINE = {
    "stream": "waterway:stream",
    "river": "waterway:river",
    "canal": "waterway:canal",
}

INDUSTRY_CLASSES = frozenset(INDUSTRY_PAIRS.values()) | frozenset(INDUSTRY_RAILWAY.values())
FOREST_CLASSES = frozenset(FOREST_PAIRS.values())
WATER_AREA_CLASSES = frozenset(WATER_AREA_PAIRS.values())
WATER_LINE_CLASSES = frozenset(WATER_LINE.values())

TAG_WHITELIST = frozenset([
    "name", "ref", "alt_name", "old_name", "short_name", "int_ref",
    "official_name", "operator", "usage", "service", "bridge", "tunnel",
    "layer", "layers", "level", "location", "electrified", "voltage", "gauge",
    "tracktype", "maxspeed", "oneway", "junction", "lanes", "network", "route",
    "waterway", "water", "landuse", "natural", "landcover", "man_made",
    "signal_type", "position", "colour", "state", "direction", "type", "stop",
    "ref:coloured", "ref:sign", "description",
])
TAG_PREFIXES = ("ref:", "railway:", "network:", "operator:", "route:",
                "maxspeed:", "signal:", "start_date", "end_date")

SELECTION = {
    "rail": list(RAIL_CLASSES),
    "rail_discarded": list(RAIL_DISCARD),
    "road": list(ROAD_CLASSES),
    "road_note": ROAD_DISCARD_NOTE,
    "signals": sorted(set(SIGNAL_KINDS) | set(SIGNAL_KINDS.values())),
    "stations": sorted(STATION_KINDS),
    "industry": sorted(INDUSTRY_CLASSES),
    "forest": sorted(FOREST_CLASSES),
    "water": sorted(WATER_AREA_CLASSES | WATER_LINE_CLASSES),
    "tags_kept": sorted(TAG_WHITELIST),
    "tags_kept_by_prefix": list(TAG_PREFIXES),
    "areas": ("closed ways and multipolygon relations are assembled by "
              "libosmium into rings; open ways carrying the same tags are kept "
              "as geometry with rings=[] and area_km2=0, flagged open=true"),
    "bounds": "features are kept whole; nodes outside the selected map window "
              "get tile=null and are counted in the summary",
}

INDEX_TYPES = ["flex_mem", "dense_mem_array", "sparse_mem_array",
               "dense_mmap_array", "sparse_mmap_array", "dense_file_array",
               "sparse_file_array"]


# --------------------------------------------------------------------------
# download
# --------------------------------------------------------------------------

def human(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024.0 or unit == "GB":
            return "%.1f %s" % (n, unit)
        n /= 1024.0
    return "%.1f GB" % n


def sha256_of(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def cached_path(state: str, cache_dir: str):
    """An extract for this state already in the cache, or None.

    The canonical name is <state>.osm.pbf, but a cache may already hold the
    file under one of the usual abbreviations (nrw.osm.pbf and friends), which
    is reused rather than downloaded a second time.
    """
    candidates = [state] + CACHE_ALIASES.get(state, [])
    for name in candidates:
        path = os.path.join(cache_dir, name + ".osm.pbf")
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return path
    return None


def download_state(state: str, cache_dir: str, retries: int, quiet: bool):
    """Fetch one state extract. Returns (path, url, bytes)."""
    url = GEOFABRIK_URL % state
    dest = os.path.join(cache_dir, state + ".osm.pbf")
    part = dest + ".part"
    os.makedirs(cache_dir, exist_ok=True)
    for attempt in range(1, retries + 1):
        have = os.path.getsize(part) if os.path.exists(part) else 0
        headers = {"Range": "bytes=%d-" % have} if have else {}
        try:
            with requests.get(url, headers=headers, stream=True,
                              timeout=(30, 600)) as resp:
                if have and resp.status_code == 200:
                    have = 0          # server ignored the range, start over
                resp.raise_for_status()
                want = int(resp.headers.get("Content-Length", 0)) + have
                mode = "ab" if (have and resp.status_code == 206) else "wb"
                got = have
                last = time.time()
                with open(part, mode) as out:
                    for chunk in resp.iter_content(8 * 1024 * 1024):
                        if not chunk:
                            continue
                        out.write(chunk)
                        got += len(chunk)
                        now = time.time()
                        if not quiet and now - last > 10.0:
                            last = now
                            pct = (" %.0f%%" % (100.0 * got / want)) if want else ""
                            print("\r[osm-ref] %-22s %s / %s%s"
                                  % (state, human(got), human(want), pct),
                                  end="", flush=True)
                if not quiet:
                    print("\r[osm-ref] %-22s %s downloaded       "
                          % (state, human(got)), flush=True)
                size = os.path.getsize(part)
                if want and size != want:
                    raise IOError("short read: %d of %d bytes" % (size, want))
            os.replace(part, dest)
            return dest, url, size
        except Exception as exc:  # noqa: BLE001 - any transport hiccup retries
            if attempt == retries:
                sys.exit("[osm-ref] download of %s failed: %s" % (state, exc))
            wait = 5.0 * attempt
            print("[osm-ref] %s: attempt %d/%d failed (%s); retry in %.0fs"
                  % (state, attempt, retries, exc, wait), file=sys.stderr)
            time.sleep(wait)
    sys.exit("[osm-ref] download of %s failed" % state)


# --------------------------------------------------------------------------
# extraction
# --------------------------------------------------------------------------

def tag_view(tags):
    """Whitelisted tags as a plain dict (a C++ TagList has no .items())."""
    out = {}
    for t in tags:
        if t.k in TAG_WHITELIST or any(t.k.startswith(p) for p in TAG_PREFIXES):
            out[t.k] = t.v
    return out


def node_points(nodes):
    """Node locations as [(lat, lon), ...]; unresolvable nodes are dropped."""
    pts = []
    invalid = 0
    for nr in nodes:
        loc = nr.location
        if not loc.valid():
            invalid += 1
            continue
        pts.append((loc.lat, loc.lon))
    return pts, invalid


class Extractor(_SimpleHandler):
    """Pull every reference feature out of one state extract, in one pass.

    The handler has node(), way() and area() callbacks, so libosmium reads the
    file twice: once for the multipolygon relations, once for everything else,
    with a node-location index filled in on the way. Because area() exists,
    closed ways arrive as assembled areas - which is what we want for the
    polygon features, and the only place where a closed railway/road way can
    show up.
    """

    def __init__(self, state, sink, way_tiles=False):
        super().__init__()
        self.state = state
        self.sink = sink
        self.way_tiles = way_tiles
        self.counts = dict.fromkeys(CATEGORIES, 0)
        self.km = {}
        self.area_km2 = {}
        self.skipped = {}
        self.outside_features = 0
        self.outside_nodes = 0
        self.duplicate_ways = 0
        self.oneway_km = {}
        # With area processing switched on, libosmium hands every closed way to
        # the handler twice: once as the way itself and once as an assembled
        # area ring carrying the same id. Both copies describe the same
        # geometry, so the second one is dropped here and counted, instead of
        # double counting length and area later on.
        self._seen_ids = {cat: set() for cat in ("rail", "road", "industry",
                                                  "forest", "water")}

    def _first_time(self, category, ident, kind):
        """False when this id/kind-space has already been emitted."""
        if kind == "node":
            return True
        seen = self._seen_ids[category]
        if ident in seen:
            self.duplicate_ways += 1
            return False
        seen.add(ident)
        return True

    # -- bookkeeping -----------------------------------------------------

    def _skip(self, why):
        self.skipped[why] = self.skipped.get(why, 0) + 1

    def _emit(self, category, ident, kind, record, pts):
        out = 0
        for lat, lon in pts:
            if not inside_bounds(lat, lon):
                out += 1
        if out:
            self.outside_features += 1
            self.outside_nodes += out
        self.counts[category] += 1
        self.sink.write(category, ident, kind, record)

    def _anchor(self, pts):
        """Representative point of a feature: first node, for polylines the
        node in the middle, for rings the ring centroid."""
        lat = sum(p[0] for p in pts) / len(pts)
        lon = sum(p[1] for p in pts) / len(pts)
        return round(lat, COORD_DECIMALS), round(lon, COORD_DECIMALS)

    # -- record builders -------------------------------------------------

    def _line_record(self, category, ident, kind, cls, tags, pts, invalid):
        """rail / road ways: full geometry, length, bridge/tunnel/layer info."""
        if not self._first_time(category, ident, kind):
            return
        layer = tags.get("layer", tags.get("layers"))
        try:
            layer = int(layer)
        except (TypeError, ValueError):
            layer = layer or None
        km = polyline_km(pts)
        rec = {
            "id": ident,
            "kind": kind,
            "class": cls,
            "ref": tags.get("ref"),
            "tags": tag_view(tags),
            "nodes": [latlon(lat, lon) for lat, lon in pts],
            "km": round(km, KM_DECIMALS),
            "bridge": tags.get("bridge") in ("yes", "viaduct", "movable"),
            "tunnel": tags.get("tunnel") in ("yes", "building_passage", "culvert"),
            "layers": layer,
            "usage": tags.get("usage"),
            "service": tags.get("service"),
            "lat": pts[0][0],
            "lon": pts[0][1],
            "tile": tile_xy(pts[0][0], pts[0][1]),
        }
        if invalid:
            rec["incomplete_nodes"] = invalid
        if self.way_tiles:
            rec["tiles"] = [tile_xy(lat, lon) for lat, lon in pts]
        self.km[cls] = self.km.get(cls, 0.0) + km
        if tags.get("oneway") in ("yes", "-1", "reversible", "alternating"):
            self.oneway_km[cls] = self.oneway_km.get(cls, 0.0) + km
        self._emit(category, ident, kind, rec, pts)

    def _point_record(self, category, ident, kind, ftype, tags, pts, invalid):
        """signals / stations: one representative point, geometry kept for ways."""
        lat, lon = self._anchor(pts)
        rec = {
            "id": ident,
            "kind": kind,
            "type": ftype,
            "name": tags.get("name"),
            "ref": tags.get("ref"),
            "tags": tag_view(tags),
            "lat": lat,
            "lon": lon,
            "tile": tile_xy(lat, lon),
            "km": round(polyline_km(pts), KM_DECIMALS),
        }
        if kind == "way":
            rec["nodes"] = [latlon(p[0], p[1]) for p in pts]
            if self.way_tiles:
                rec["tiles"] = [tile_xy(p[0], p[1]) for p in pts]
        if invalid:
            rec["incomplete_nodes"] = invalid
        self._emit(category, ident, kind, rec, pts)

    def _polygon_record(self, category, ident, kind, cls, rings, tags, from_way):
        if not self._first_time(category, ident, kind):
            return
        area = sum(ring_area_km2(ring) for ring in rings)
        perim = sum(polyline_km(ring) for ring in rings)
        flat = [p for ring in rings for p in ring]
        lat, lon = self._anchor(flat)
        rec = {
            "id": ident,
            "kind": kind,
            "class": cls,
            "name": tags.get("name"),
            "ref": tags.get("ref"),
            "tags": tag_view(tags),
            "rings": [[latlon(p[0], p[1]) for p in ring] for ring in rings],
            "area_km2": round(area, KM_DECIMALS),
            "km": round(perim, KM_DECIMALS),
            "from_way": bool(from_way),
            "multipolygon": len(rings) > 1,
            "lat": lat,
            "lon": lon,
            "tile": tile_xy(lat, lon),
        }
        self.area_km2[cls] = self.area_km2.get(cls, 0.0) + area
        self._emit(category, ident, kind, rec, flat)

    def _line_feature(self, category, ident, kind, cls, tags, pts):
        """waterway=stream|river|canal and friends: geometry, no area."""
        if not self._first_time(category, ident, kind):
            return
        lat, lon = self._anchor(pts)
        km = polyline_km(pts)
        rec = {
            "id": ident,
            "kind": kind,
            "class": cls,
            "name": tags.get("name"),
            "ref": tags.get("ref"),
            "tags": tag_view(tags),
            "nodes": [latlon(p[0], p[1]) for p in pts],
            "area_km2": 0.0,
            "km": round(km, KM_DECIMALS),
            "from_way": kind == "way",
            "multipolygon": False,
            "lat": lat,
            "lon": lon,
            "tile": tile_xy(lat, lon),
        }
        if self.way_tiles:
            rec["tiles"] = [tile_xy(p[0], p[1]) for p in pts]
        self._emit(category, ident, kind, rec, pts)

    def _open_polygon(self, ident, category, cls, tags, pts):
        """A polygon-class tag on an open way: geometry kept, area left at 0."""
        if not self._first_time(category, ident, "way"):
            return
        lat, lon = self._anchor(pts)
        km = polyline_km(pts)
        rec = {
            "id": ident,
            "kind": "way",
            "class": cls,
            "name": tags.get("name"),
            "ref": tags.get("ref"),
            "tags": tag_view(tags),
            "rings": [],
            "nodes": [latlon(p[0], p[1]) for p in pts],
            "area_km2": 0.0,
            "km": round(km, KM_DECIMALS),
            "from_way": True,
            "multipolygon": False,
            "open": True,
            "lat": lat,
            "lon": lon,
            "tile": tile_xy(lat, lon),
        }
        if self.way_tiles:
            rec["tiles"] = [tile_xy(p[0], p[1]) for p in pts]
        self._emit(category, ident, "way", rec, pts)

    # -- classification --------------------------------------------------

    @staticmethod
    def area_class(tags):
        """(category, class) for a polygon-ish feature, or None."""
        railway = tags.get("railway")
        waterway = tags.get("waterway")
        for t in tags:
            pair = (t.k, t.v)
            cls = WATER_AREA_PAIRS.get(pair) or FOREST_PAIRS.get(pair) \
                or INDUSTRY_PAIRS.get(pair)
            if cls is not None:
                category = ("water" if cls in WATER_AREA_CLASSES
                            else "forest" if cls in FOREST_CLASSES else "industry")
                return category, cls
            if t.k == "waterway" and t.v in WATER_LINE:
                return "water", WATER_LINE[t.v]
        if railway in INDUSTRY_RAILWAY:
            return "industry", INDUSTRY_RAILWAY[railway]
        if waterway in WATER_LINE:
            return "water", WATER_LINE[waterway]
        return None

    # -- callbacks -------------------------------------------------------

    def node(self, n):
        tags = n.tags
        railway = tags.get("railway")
        if railway in SIGNAL_KINDS:
            self._point_record("signals", n.id, "node", SIGNAL_KINDS[railway],
                               tags, [(n.lat, n.lon)], 0)
        elif railway in STATION_KINDS:
            self._point_record("stations", n.id, "node", STATION_KINDS[railway],
                               tags, [(n.lat, n.lon)], 0)
        elif railway in INDUSTRY_RAILWAY or self.area_class(tags) is not None:
            cls = self.area_class(tags)
            if cls is not None and cls[0] == "industry":
                rec = {
                    "id": n.id,
                    "kind": "node",
                    "class": cls[1],
                    "name": tags.get("name"),
                    "ref": tags.get("ref"),
                    "tags": tag_view(tags),
                    "rings": [],
                    "area_km2": 0.0,
                    "km": 0.0,
                    "from_way": False,
                    "multipolygon": False,
                    "lat": round(n.lat, COORD_DECIMALS),
                    "lon": round(n.lon, COORD_DECIMALS),
                    "tile": tile_xy(n.lat, n.lon),
                }
                self._emit("industry", n.id, "node", rec, [(n.lat, n.lon)])

    def way(self, w):
        tags = w.tags
        railway = tags.get("railway")
        highway = tags.get("highway")
        if railway is None and highway is None and not any(
                t.k in ("natural", "landuse", "landcover", "waterway", "man_made")
                for t in tags):
            return
        pts, invalid = node_points(w.nodes)
        if len(pts) < 2:
            self._skip("way_with_fewer_than_2_nodes")
            return
        if railway in RAIL_CLASSES:
            self._line_record("rail", w.id, "way", railway, tags, pts, invalid)
        elif highway in ROAD_CLASSES:
            self._line_record("road", w.id, "way", highway, tags, pts, invalid)
        elif railway in SIGNAL_KINDS:
            self._point_record("signals", w.id, "way", SIGNAL_KINDS[railway],
                               tags, pts, invalid)
        elif railway in STATION_KINDS:
            self._point_record("stations", w.id, "way", STATION_KINDS[railway],
                               tags, pts, invalid)
        elif tags.get("waterway") in WATER_LINE:
            self._line_feature("water", w.id, "way", WATER_LINE[tags["waterway"]],
                               tags, pts)
        else:
            cls = self.area_class(tags)
            # A *closed* way in a polygon class is handed to the area callback
            # as well, which is where the proper ring comes from; emitting it
            # here too would shadow that ring with an area-less line, so only
            # genuinely open ways are kept here.
            if cls is not None and not w.nodes.is_closed():
                self._open_polygon(w.id, cls[0], cls[1], tags, pts)

    def area(self, a):
        tags = a.tags
        railway = tags.get("railway")
        highway = tags.get("highway")
        ident = a.orig_id()
        rings = []
        for ring in a.outer_rings():
            pts, invalid = node_points(ring)
            if len(pts) >= 3:
                rings.append(pts)
            elif pts:
                self._skip("area_ring_with_%d_nodes" % len(pts))
        if railway in RAIL_CLASSES:
            if not rings:
                self._skip("closed_rail_way_without_usable_ring")
                return
            flat = [p for r in rings for p in r]
            self._line_record("rail", ident, "way", railway, tags, flat, 0)
            return
        if highway in ROAD_CLASSES:
            if not rings:
                self._skip("closed_road_way_without_usable_ring")
                return
            flat = [p for r in rings for p in r]
            self._line_record("road", ident, "way", highway, tags, flat, 0)
            return
        cls = self.area_class(tags)
        if cls is None:
            return
        if not rings:
            self._skip("area_without_outer_ring")
            return
        category, klass = cls
        if klass in WATER_LINE_CLASSES:
            flat = [p for r in rings for p in r]
            self._line_feature(category, ident, "area", klass, tags, flat)
            return
        self._polygon_record(category, ident, "area", klass, rings, tags,
                             a.from_way())


# --------------------------------------------------------------------------
# per-state shards
# --------------------------------------------------------------------------

class Shards:
    """Per-state JSONL shards, one line per feature:

        <id>\\t<kind>\\t<compact json>

    One file per (category, object kind) so that every shard is sorted by id
    just by construction: libosmium emits nodes, then ways, then areas, each in
    ascending id order. The writer checks that assumption and the merger falls
    back to an in-memory sort for the rare shard that turns out not to be
    ordered, so ordering can never silently break.
    """

    def __init__(self, work_dir, state, buffer_bytes=4 << 20):
        self.work_dir = work_dir
        self.state = state
        self.buffer_bytes = buffer_bytes
        self.paths = {}
        self.files = {}
        self.is_sorted = {}
        self.last = {}

    def write(self, category, ident, kind, record):
        path = self.paths.get((category, kind))
        if path is None:
            path = os.path.join(self.work_dir, "%s.%s.%s.jsonl"
                                % (category, kind, self.state))
            self.paths[(category, kind)] = path
            self.files[(category, kind)] = open(path, "w", encoding="utf-8",
                                                buffering=self.buffer_bytes)
            self.is_sorted[(category, kind)] = True
            self.last[(category, kind)] = None
        key = (category, kind)
        if self.last[key] is not None and ident < self.last[key]:
            self.is_sorted[key] = False
        self.last[key] = ident
        self.files[key].write("%013d\t%s\t%s\n" % (
            ident, kind, json.dumps(record, sort_keys=True, separators=(",", ":"))))

    def close(self):
        for fh in self.files.values():
            fh.close()


def extract_state(path, state, sink, way_tiles, idx, quiet):
    if osmium is None:
        sys.exit("osm-reference.py needs 'osmium' (pyosmium) to read an "
                 "extract: pip install osmium")
    started = time.time()
    handler = Extractor(state, sink, way_tiles=way_tiles)
    filters = [osmium.filter.KeyFilter(
        "railway", "highway", "natural", "landuse", "landcover", "waterway",
        "man_made")]
    try:
        handler.apply_file(path, idx=idx, filters=filters)
    except Exception as exc:  # noqa: BLE001
        sys.exit("[osm-ref] %s: extraction failed: %r" % (state, exc))
    sink.close()
    if not quiet:
        counts = "  ".join("%s=%d" % (c, handler.counts[c])
                           for c in CATEGORIES if handler.counts[c])
        print("[osm-ref] %-22s %5.0fs  %s" % (state, time.time() - started, counts))
    return handler


# --------------------------------------------------------------------------
# resume: one manifest per finished state
# --------------------------------------------------------------------------

def manifest_path(work_dir: str, state: str) -> str:
    return os.path.join(work_dir, "%s.state.json" % state)


def save_manifest(work_dir, state, meta, sink, handler, way_tiles):
    """Record what this state contributed, so a later run can reuse its shards.

    Written after the shards are closed, and written atomically, so a run that
    dies mid-extraction leaves a manifest-less (and therefore ignored) set of
    shards rather than a manifest that points at half a file.
    """
    man = {
        "state": state,
        "meta": meta,
        "way_tiles": bool(way_tiles),
        "counts": dict(handler.counts),
        "skipped": dict(sorted(handler.skipped.items())),
        "outside_features": handler.outside_features,
        "outside_nodes": handler.outside_nodes,
        "duplicate_ways": handler.duplicate_ways,
        "oneway_km": dict(handler.oneway_km),
        "shards": [{"category": cat, "kind": kind, "path": path,
                    "state": state, "sorted": sink.is_sorted[(cat, kind)]}
                   for (cat, kind), path in sorted(sink.paths.items())],
    }
    path = manifest_path(work_dir, state)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(man, fh, sort_keys=True, separators=(",", ":"))
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)


def load_manifest(work_dir, state, meta, way_tiles):
    """The manifest of this state if its shards can be reused as they stand.

    The extract is identified by its byte size and its SHA-256 (the latter only
    when it was recorded), because Geofabrik republishes each file in place and
    a re-run a day later may well have a different one; a state extracted from
    a different extract must not be reused.
    """
    try:
        with open(manifest_path(work_dir, state), "r", encoding="utf-8") as fh:
            man = json.load(fh)
    except (OSError, ValueError):
        return None
    if man.get("state") != state:
        return None
    if bool(man.get("way_tiles")) != bool(way_tiles):
        return None            # shard contents differ, so they must be redone
    old = man.get("meta", {})
    for key in ("bytes", "sha256"):
        if old.get(key) != meta.get(key):
            return None
    shards = man.get("shards") or []
    if not shards:
        return None            # nothing was written for this state anyway
    for shard in shards:
        if not os.path.exists(shard["path"]):
            return None
        # Manifests written before the per-shard state was recorded still name
        # the state in the file name (<category>.<kind>.<state>.jsonl), so an
        # older work directory stays resumable instead of being thrown away.
        shard.setdefault("state", os.path.basename(shard["path"]).split(".")[2])
    return man


# --------------------------------------------------------------------------
# merge shards -> final JSON
# --------------------------------------------------------------------------

def shard_stream(path, is_sorted):
    with open(path, "r", encoding="utf-8") as fh:
        if is_sorted:
            for line in fh:
                i = line.index("\t")
                j = line.index("\t", i + 1)
                yield int(line[:i]), line[i + 1:j], line[j + 1:]
        else:
            rows = []
            for line in fh:
                i = line.index("\t")
                j = line.index("\t", i + 1)
                rows.append((int(line[:i]), line[i + 1:j], line[j + 1:]))
            rows.sort(key=lambda r: (r[0], r[1]))
            for row in rows:
                yield row


def tagged_stream(shard):
    """A shard's rows, each carrying the state it came from.

    The state matters because neighbouring extracts overlap: each is the region plus whatever
    each state clips in around its edges: summing km per state gives the German
    figure, while the merged file alone counts the whole 47-55.5 / 4-15.5
    window, neighbours and all. A way along a state border lands in both
    extracts, so the per-state sum is a slight overcount - by a fraction of a
    percent - and it says so rather than pretending to be exact.
    """
    state = shard["state"]
    for ident, kind, payload in shard_stream(shard["path"], shard["sorted"]):
        yield ident, kind, payload, state


def merge_category(category, shards, out_path, header, quiet=False):
    """k-way merge the shards of one category into the final JSON file.

    shards is {kind: [shard, ...]}. All shards are merged in one pass with the
    sort key (id, kind), so ids come out ascending; the kind is only a
    tiebreaker, because OSM numbers nodes, ways and relations separately and a
    node and a way can therefore share a number. Every shard is itself sorted
    by that key, which is what makes the streaming merge valid.
    """
    body_path = out_path + ".body"
    started = time.time()
    count = 0
    duplicates = 0
    unsorted_shards = 0
    km_by_class = {}
    km2_by_class = {}
    count_by_type = {}
    km_by_state = {}
    count_by_state = {}
    streams = []
    for kind in KIND_ORDER:
        for shard in shards.get(kind) or []:
            if not shard["sorted"]:
                unsorted_shards += 1
            streams.append(tagged_stream(shard))
    prev = None
    with open(body_path, "w", encoding="utf-8", buffering=4 << 20) as body:
        for ident, kind, payload, state in heapq.merge(
                *streams, key=lambda t: (t[0], t[1])):
            key = (ident, kind)
            if key == prev:
                duplicates += 1
                continue
            prev = key
            rec = json.loads(payload)
            count += 1
            count_by_state[state] = count_by_state.get(state, 0) + 1
            cls = rec.get("class")
            if cls is not None:
                km_by_class[cls] = km_by_class.get(cls, 0.0) + rec.get("km", 0.0)
                km2_by_class[cls] = km2_by_class.get(cls, 0.0) + rec.get("area_km2", 0.0)
                per_state = km_by_state.setdefault(state, {})
                per_state[cls] = per_state.get(cls, 0.0) + rec.get("km", 0.0)
            ftype = rec.get("type")
            if ftype is not None:
                count_by_type[ftype] = count_by_type.get(ftype, 0) + 1
            if count > 1:
                body.write(",\n")
            else:
                body.write("\n")
            body.write(payload)
        body.write("\n")

    totals = {}
    if km_by_class:
        totals["km_by_class"] = {k: round(v, KM_DECIMALS)
                                 for k, v in sorted(km_by_class.items())}
    if km2_by_class:
        totals["area_km2_by_class"] = {k: round(v, KM_DECIMALS)
                                       for k, v in sorted(km2_by_class.items())}
    if count_by_type:
        totals["count_by_type"] = dict(sorted(count_by_type.items()))
    if km_by_state:
        totals["km_by_state"] = {state: {k: round(v, KM_DECIMALS)
                                         for k, v in sorted(classes.items())}
                                 for state, classes in sorted(km_by_state.items())}
        totals["count_by_state"] = dict(sorted(count_by_state.items()))

    header = dict(header)
    header["count"] = count
    header["duplicates_merged"] = duplicates
    header["totals"] = totals

    filename, array_key = CATEGORY_FILES[category]
    with open(out_path, "w", encoding="utf-8") as out:
        out.write("{\n")
        for key in sorted(header):
            out.write("%s: %s,\n" % (json.dumps(key),
                                     json.dumps(header[key], sort_keys=True)))
        out.write("%s: [\n" % json.dumps(array_key))
        with open(body_path, "r", encoding="utf-8") as body:
            shutil.copyfileobj(body, out, 4 << 20)
        out.write("]\n}\n")
    os.unlink(body_path)
    result = {
        "count": count,
        "duplicates": duplicates,
        "unsorted_shards": unsorted_shards,
        "km_by_class": km_by_class,
        "km2_by_class": km2_by_class,
        "count_by_type": count_by_type,
        "file": filename,
        "bytes": os.path.getsize(out_path),
        "seconds": time.time() - started,
    }
    if not quiet:
        print("[osm-ref] %-24s %8d features  %9s  %3.0fs"
              % (filename, count, human(result["bytes"]), result["seconds"]))
    return result


# --------------------------------------------------------------------------
# summary
# --------------------------------------------------------------------------

def print_summary(rows, results, skipped, outside, duplicate_ways, out_dir,
                  elapsed, oneway):
    """Print the per-state table, the class totals and the diagnostics.

    oneway maps class -> km of oneway-tagged linework. German motorways are
    mapped as two separate oneway carriageways, so their OSM length is
    roughly double the official centreline length; the summary says so rather
    than quietly reporting a number that cannot be compared to the real one.
    """
    def line(st, c):
        return ("%-22s %9s %10.0f %9s %10.0f %8s %8s %10.0f %10.0f %10.0f" % (
            st, c["rail"], results["km_by_class"].get("rail", 0.0),
            c["road"], sum(v for k, v in results["km_by_class"].items()
                           if k in ROAD_CLASSES),
            c["signals"], c["stations"],
            sum(v for k, v in results["km2_by_class"].items() if k in FOREST_CLASSES),
            sum(v for k, v in results["km2_by_class"].items() if k in WATER_AREA_CLASSES),
            sum(v for k, v in results["km2_by_class"].items() if k in INDUSTRY_CLASSES)))

    head = ("%-22s %9s %10s %9s %10s %8s %8s %10s %10s %10s" % (
        "state", "rail_ways", "rail_km", "road_ways", "road_km", "signals",
        "stations", "forest_km2", "water_km2", "industry_km2"))

    print("")
    print(head)
    print("-" * len(head))
    for row in rows:
        print(line(row["state"], row["counts"]))
    print("-" * len(head))
    all_counts = {c: sum(r["counts"][c] for r in rows) for c in CATEGORIES}
    print(line("ALL (%d states)" % len(rows), all_counts))

    print("")
    print("length per class (km)")
    for cls in sorted(results["km_by_class"]):
        note = ""
        if cls in ("motorway", "trunk", "primary"):
            frac = 100.0 * oneway.get(cls, 0.0) / max(results["km_by_class"][cls], 1e-9)
            note = "   (%.0f%% tagged oneway)" % frac
        print("  %-28s %12.1f%s" % (cls, results["km_by_class"][cls], note))
    print("")
    print("  note: German motorway and trunk roads are mapped in OSM as two")
    print("        separate oneway carriageways, so the km above counts each")
    print("        direction separately. Halve motorway+trunk before comparing")
    print("        with the official network length.")
    print("")
    print("area per class (km2, summed over polygons - overlaps included)")
    for cls in sorted(results["km2_by_class"]):
        print("  %-28s %12.1f" % (cls, results["km2_by_class"][cls]))
    print("")
    print("features written")
    for cat in CATEGORIES:
        res = results["per_category"][cat]
        print("  %-28s %9d  %9s" % (res["file"], res["count"],
                                    human(res["bytes"])))
    for cat in ("signals", "stations"):
        types = results["per_category"][cat]["count_by_type"]
        print("  %-28s %9s" % ("  " + cat + " by type",
                                ", ".join("%s=%d" % kv for kv in sorted(types.items()))))
    print("")
    print("diagnostics")
    print("  features reaching outside the map window : %d (%d nodes)"
          % (outside[0], outside[1]))
    print("  closed ways delivered twice by libosmium : %d (dropped)"
          % duplicate_ways)
    print("  duplicate ids merged across states       : %d"
          % sum(r["duplicates"] for r in results["per_category"].values()))
    print("  shards that needed an in-memory sort     : %d"
          % sum(r["unsorted_shards"] for r in results["per_category"].values()))
    print("  features skipped as malformed            : %d"
          % sum(skipped.values()))
    for why in sorted(skipped):
        print("  skipped: %-36s %d" % (why, skipped[why]))
    print("")
    print("done in %.0fs, output in %s" % (elapsed, out_dir))


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def parse_states(spec):
    """Resolve --states into a de-duplicated list, in STATES order.

    Order and repeats are normalised so that '--states a,b' and '--states b,a'
    produce byte-identical files: neighbouring extracts repeat a handful of
    ways, and which copy survives the merge must not depend on argument order.
    """
    if not spec or spec.strip().lower() == "all":
        return list(STATES)
    wanted = set()
    for name in spec.replace(" ", "").split(","):
        if not name:
            continue
        if name not in STATES:
            sys.exit("[osm-ref] unknown state %r; valid: %s"
                     % (name, ", ".join(STATES)))
        wanted.add(name)
    if not wanted:
        sys.exit("[osm-ref] --states selected nothing")
    return [s for s in STATES if s in wanted]


HELP_EPILOG = """\
tag selection
  rail       railway={rail}
             discarded: {raild}
  road       highway={road}
             {roadnote}
  signals    railway=signal|signal_box|stop|buffer_stop on nodes and ways
  stations   railway=station|halt on nodes and ways
  industry   landuse=industrial|railway, railway=industrial, man_made=works
  forest     natural=wood, landuse=forest, landcover=forest
  water      natural=water, waterway=riverbank, waterway=stream|river|canal

output (--out-dir, default reference/)
  osm-rail.json      ways     reference rail network, class = railway value
  osm-road.json      ways     reference road network, class = highway value
  osm-signals.json   signals  signals, signal boxes, stops, buffer stops
  osm-stations.json  stations stations and halts
  osm-industry.json  areas    industrial landuse / works
  osm-forest.json    polygons forest polygons
  osm-water.json     features water polygons and waterway lines

record shapes
  way     id, kind, class, ref, tags, nodes [[lat,lon],...], km, bridge,
          tunnel, layers, usage, service, lat/lon/tile anchor
  point   id, kind, type, name, ref, tags, lat, lon, tile, km (+nodes for ways)
  polygon id, kind, class, name, ref, tags, rings [[lat,lon],...], area_km2,
          km (perimeter), from_way, multipolygon, lat/lon/tile centroid
          open=true means the source way was not closed, so area_km2 is 0
  a way with fewer than 2 resolvable nodes is skipped and counted in skipped

determinism
  No timestamps: each file records the source URL, byte size and SHA-256 of the
  input extract instead. Arrays are sorted by OSM id then kind, coordinates are
  rounded to {coord} decimals, km/areas to {km}, JSON keys are sorted and the
  separators are compact, so re-running over the same extract gives
  byte-identical files. Compare with sha256sum.

examples
  python3 scripts/osm-reference.py --states nordrhein-westfalen --out-dir /tmp/osmtest-out
  python3 scripts/osm-reference-stats.py --dir /tmp/osmtest-out
  python3 scripts/osm-reference.py                # all 16 states, ~7 GB input
  python3 scripts/osm-reference.py --work-dir /tmp/osm-work --keep-work
      # resumable: interrupt and re-run this, finished states are reused
"""


def build_parser():
    parser = argparse.ArgumentParser(
        prog="osm-reference.py",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__,
        epilog=HELP_EPILOG.format(
            rail=",".join(RAIL_CLASSES), raild=",".join(RAIL_DISCARD),
            road=",".join(ROAD_CLASSES), roadnote=ROAD_DISCARD_NOTE,
            coord=COORD_DECIMALS, km=KM_DECIMALS))
    parser.add_argument("--states", default="all",
                        help="comma separated German states, or 'all' "
                             "(default). Valid: " + ", ".join(STATES))
    parser.add_argument("--pbf", action="append", default=None,
                        metavar="PATH",
                        help="read a local .osm.pbf instead of a Geofabrik "
                             "state extract; repeat for several files")
    parser.add_argument("--lat-min", type=float, default=LAT_MIN)
    parser.add_argument("--lat-max", type=float, default=LAT_MAX)
    parser.add_argument("--lon-min", type=float, default=LON_MIN)
    parser.add_argument("--lon-max", type=float, default=LON_MAX)
    parser.add_argument("--size", type=int, default=MAP_TILES,
                        help="map size in tiles, power of two in 64..4096 "
                             "(default: 4096); must match the manifest")
    parser.add_argument("--out-dir", default=None,
                        help="output directory (default: "
                             "reference/ in the repo)")
    parser.add_argument("--cache-dir", default="/tmp/osm",
                        help="extract cache, <state>.osm.pbf "
                             "(default: /tmp/osm)")
    parser.add_argument("--work-dir", default=None,
                        help="scratch directory for per-state shards "
                             "(default: a temporary directory inside --out-dir)")
    parser.add_argument("--idx", default="sparse_file_array",
                        help="libosmium node-location index, one of: "
                             + ", ".join(INDEX_TYPES) + " (default: "
                             "sparse_file_array, disk backed, needed for "
                             "multi-GB extracts)")
    parser.add_argument("--retries", type=int, default=4,
                        help="download attempts per state (default: 4); "
                             "interrupted downloads resume with a Range header")
    parser.add_argument("--skip-download", action="store_true",
                        help="never fetch, fail if an extract is not cached")
    parser.add_argument("--no-hash", action="store_true",
                        help="skip the input SHA-256 (faster, not comparable "
                             "between machines)")
    parser.add_argument("--way-tiles", action="store_true",
                        help="emit a per-node tile index for every way too; "
                             "roughly doubles the output size")
    parser.add_argument("--keep-work", action="store_true",
                        help="keep the per-state shard directory")
    parser.add_argument("--no-resume", action="store_true",
                        help="ignore the per-state manifests in --work-dir and "
                             "extract every state again")
    parser.add_argument("--quiet", action="store_true",
                        help="print only the final summary")
    parser.add_argument("--self-test", action="store_true",
                        help="check the recorded projection examples against "
                             "tile_xy() and exit; needs no data and no pyosmium")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    configure(args.lat_min, args.lat_max, args.lon_min, args.lon_max, args.size)
    if args.self_test:
        return self_test()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_dir = args.out_dir or os.path.join(root, "reference")
    local_pbf = {}
    if args.pbf:
        for path in args.pbf:
            if not os.path.isfile(path):
                sys.exit("[osm-ref] --pbf file not found: %s" % path)
            state = os.path.basename(path)
            for suffix in (".osm.pbf", ".pbf"):
                if state.endswith(suffix):
                    state = state[:-len(suffix)]
                    break
            if state in local_pbf:
                sys.exit("[osm-ref] --pbf state name repeated: %s" % state)
            local_pbf[state] = path
        states = list(local_pbf)
    else:
        states = parse_states(args.states)
    if args.idx not in INDEX_TYPES:
        sys.exit("[osm-ref] unknown --idx %r; pick one of: %s"
                 % (args.idx, ", ".join(INDEX_TYPES)))

    os.makedirs(out_dir, exist_ok=True)
    work_dir_is_temp = args.work_dir is None
    work_dir = args.work_dir or tempfile.mkdtemp(prefix=".osm-reference-", dir=out_dir)
    os.makedirs(work_dir, exist_ok=True)

    started = time.time()
    states_meta = []
    rows = []
    shard_index = {cat: {} for cat in CATEGORIES}
    skipped = {}
    outside = [0, 0]
    duplicate_ways = 0
    oneway = {}

    try:
        for state in states:
            if local_pbf:
                cache_path = local_pbf[state]
                url = "file://" + os.path.abspath(cache_path)
                size = os.path.getsize(cache_path)
                if not args.quiet:
                    print("[osm-ref] %-22s local    %-10s %s"
                          % (state, os.path.basename(cache_path), human(size)))
            else:
                cache_path = cached_path(state, args.cache_dir)
                if cache_path is not None:
                    url = GEOFABRIK_URL % state
                    size = os.path.getsize(cache_path)
                    if not args.quiet:
                        print("[osm-ref] %-22s cached   %-10s %s"
                              % (state, os.path.basename(cache_path), human(size)))
            if not local_pbf and cache_path is None and args.skip_download:
                sys.exit("[osm-ref] %s is not in %s and --skip-download was given"
                         % (state, args.cache_dir))
            elif not local_pbf and cache_path is None:
                cache_path, url, size = download_state(state, args.cache_dir,
                                                       args.retries, args.quiet)
            meta = {"state": state, "url": url,
                    "file": os.path.basename(cache_path), "bytes": size}
            if not args.no_hash:
                meta["sha256"] = sha256_of(cache_path)

            sink = Shards(work_dir, state)
            man = None if args.no_resume else load_manifest(work_dir, state, meta,
                                                            args.way_tiles)
            if man is not None:
                states_meta.append(man["meta"])
                for shard in man["shards"]:
                    shard_index[shard["category"]].setdefault(
                        shard["kind"], []).append({"path": shard["path"],
                                                   "state": shard["state"],
                                                   "sorted": shard["sorted"]})
                for why, n in man["skipped"].items():
                    skipped[why] = skipped.get(why, 0) + n
                outside[0] += man["outside_features"]
                outside[1] += man["outside_nodes"]
                duplicate_ways += man["duplicate_ways"]
                for cls, km in man["oneway_km"].items():
                    oneway[cls] = oneway.get(cls, 0.0) + km
                rows.append({"state": state, "counts": man["counts"]})
                if not args.quiet:
                    counts = "  ".join("%s=%d" % (c, man["counts"][c])
                                       for c in CATEGORIES if man["counts"][c])
                    print("[osm-ref] %-22s reused   %s" % (state, counts))
                continue

            states_meta.append(meta)
            handler = extract_state(cache_path, state, sink, args.way_tiles,
                                    args.idx, args.quiet)
            for key, path in sink.paths.items():
                shard_index[key[0]].setdefault(key[1], []).append(
                    {"path": path, "state": state,
                     "sorted": sink.is_sorted[key]})
            for why, n in handler.skipped.items():
                skipped[why] = skipped.get(why, 0) + n
            outside[0] += handler.outside_features
            outside[1] += handler.outside_nodes
            duplicate_ways += handler.duplicate_ways
            for cls, km in handler.oneway_km.items():
                oneway[cls] = oneway.get(cls, 0.0) + km
            rows.append({"state": state, "counts": dict(handler.counts)})
            save_manifest(work_dir, state, meta, sink, handler, args.way_tiles)

        header_common = {
            "source": "OpenStreetMap via Geofabrik state extracts (ODbL, "
                      "CC BY-SA 4.0)",
            "generated_from": states_meta,
            "projection": PROJECTION,
            "selection": SELECTION,
            "way_tiles": bool(args.way_tiles),
        }
        results = {"km_by_class": {}, "km2_by_class": {}, "per_category": {}}
        for cat in CATEGORIES:
            out_path = os.path.join(out_dir, CATEGORY_FILES[cat][0])
            header = dict(header_common)
            header["skipped"] = dict(sorted(skipped.items()))
            res = merge_category(cat, shard_index[cat], out_path, header,
                                 args.quiet)
            results["per_category"][cat] = res
            results["km_by_class"].update(res["km_by_class"])
            results["km2_by_class"].update(res["km2_by_class"])

        print_summary(rows, results, skipped, outside, duplicate_ways, out_dir,
                      time.time() - started, oneway)
    finally:
        if args.keep_work:
            if not args.quiet:
                print("[osm-ref] shards kept in %s" % work_dir)
        elif work_dir_is_temp:
            shutil.rmtree(work_dir, ignore_errors=True)
        elif not args.quiet:
            print("[osm-ref] shards kept in %s" % work_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())