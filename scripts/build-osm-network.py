#!/usr/bin/env python3
"""build-osm-network.py - turn an OSM reference into scenario geometry.

    python3 scripts/build-osm-network.py --reference-dir /tmp/osm \
        --towns /tmp/towns.json --out-dir /tmp/osm-out \
        --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5

The scenario builder takes a list of corridors and routes each consecutive
pair of waypoints with the player's road/rail tools. This script derives the
real OSM centre lines and one real station position per town from the
reference written by scripts/osm-reference.py:

* motorways: ``class == "motorway"`` (``highway=motorway``). OSM maps each
  carriageway separately, so the ways are chained by direction and the two
  directions are deduplicated by tile overlap. ``motorway_link`` ramps are not
  in the reference and are not invented here.
* main rail: ``class == "rail"``, ``usage == "main"``, ``service`` empty. Yard,
  siding, spur and crossover track is left out; double track is deduplicated in
  the same way as the motorways.
* stations: ``type == "station"`` (not ``halt``), with a DB station category or
  a name that identifies a Hauptbahnhof. One station is chosen per town, at its
  real OSM position; city U-Bahn/S-Bahn stops without a DB category are skipped.

The reference stores raw lat/lon nodes, so this script reprojects them with the
builder's own linear latitude mapping and transposed axes
(``HsbTileForBounds()`` in builder/hsb_builder_math.h). Never read the
reference's ``tile`` field: it is metadata, not input.

The output is simplified in tile space: Ramer-Douglas-Peucker with a tolerance
of ``--tolerance-tiles`` (default 2.0, about 400 m) and a maximum waypoint gap of
``--max-step-tiles`` (default 48, about 10 km). A player's road tool can only
build straight legs with one corner, so a denser polyline would not render more
faithfully; it would only make the headless build slower.
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEODATA = os.path.join(ROOT, "assets", "geodata")
REFERENCE = os.path.join(GEODATA, "reference")

LAT_MIN, LAT_MAX = 47.0, 55.5
LON_MIN, LON_MAX = 4.0, 15.5
MAP_SIZE = 4096.0


def configure(lat_min, lat_max, lon_min, lon_max, map_size):
    """Set the map window and size the builder will use."""
    global LAT_MIN, LAT_MAX, LON_MIN, LON_MAX, MAP_SIZE
    if not (math.isfinite(lat_min) and math.isfinite(lat_max)
            and math.isfinite(lon_min) and math.isfinite(lon_max)
            and -90.0 <= lat_min < lat_max <= 90.0
            and -180.0 <= lon_min < lon_max <= 180.0):
        sys.exit("build-osm-network: bounds must be finite WGS84 intervals")
    if map_size < 64 or map_size > 4096 or map_size & (map_size - 1):
        sys.exit("build-osm-network: --size must be a power of two in 64..4096")
    LAT_MIN, LAT_MAX = lat_min, lat_max
    LON_MIN, LON_MAX = lon_min, lon_max
    MAP_SIZE = float(map_size)


def project(lat: float, lon: float) -> tuple[float, float]:
    """lat/lon -> fractional tile space (x south, y east)."""
    x = (LAT_MAX - lat) / (LAT_MAX - LAT_MIN) * (MAP_SIZE - 1.0)
    y = (lon - LON_MIN) / (LON_MAX - LON_MIN) * (MAP_SIZE - 1.0)
    return x, y


def unproject(x: float, y: float) -> tuple[float, float]:
    """Fractional tile space -> lat/lon, the inverse of project()."""
    lat = LAT_MAX - (x / (MAP_SIZE - 1.0)) * (LAT_MAX - LAT_MIN)
    lon = LON_MIN + (y / (MAP_SIZE - 1.0)) * (LON_MAX - LON_MIN)
    return lat, lon


def haversine_km(a: tuple[float, float], b: tuple[float, float]) -> float:
    lat1, lon1 = math.radians(a[0]), math.radians(a[1])
    lat2, lon2 = math.radians(b[0]), math.radians(b[1])
    dlat = lat2 - lat1
    dlon = lon2 - lon1
    h = math.sin(dlat / 2.0) ** 2 + math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2.0) ** 2
    return 2.0 * 6371.0 * math.asin(min(1.0, math.sqrt(h)))


def polyline_km(nodes: list[list[float]]) -> float:
    return sum(haversine_km(a, b) for a, b in zip(nodes, nodes[1:]))


# ---------------------------------------------------------------------------
# Reading the reference
# ---------------------------------------------------------------------------

def iter_objects(path: str, array_key: str, hint: str | None = None):
    """Yield objects from a top-level JSON array, one per line.

    The reference writer emits each feature on its own line.  Parsing a 1.6 GB
    file with ``json.load`` would need more memory than this box has; the
    streaming reader keeps one feature alive at a time.  ``hint`` is a cheap
    substring filter so the four million road ways are not all decoded.
    """
    with open(path, encoding="utf-8") as f:
        started = False
        for line in f:
            stripped = line.strip()
            if not started:
                if stripped.startswith('"%s"' % array_key):
                    started = True
                continue
            if stripped.startswith("]"):
                return
            if not stripped.startswith("{"):
                continue
            if hint is not None and hint not in stripped:
                continue
            yield json.loads(stripped.rstrip(","))


def read_ways(path: str, hint: str) -> list[dict]:
    return list(iter_objects(path, "ways", hint))


def read_stations(path: str) -> list[dict]:
    return list(iter_objects(path, "stations"))


# ---------------------------------------------------------------------------
# Chaining and deduplication
# ---------------------------------------------------------------------------

def endpoint_key(point: list[float]) -> tuple[float, float]:
    return round(point[0], 6), round(point[1], 6)


def direction(a: list[float], b: list[float]) -> float:
    """Bearing in tile space, so x=north and y=east are treated alike."""
    return math.atan2(b[1] - a[1], b[0] - a[0])


def angle_difference(a: float, b: float) -> float:
    d = abs(a - b) % (2.0 * math.pi)
    return min(d, 2.0 * math.pi - d)


def chain_ways(ways: list[dict]) -> list[tuple[list[list[float]], list[dict]]]:
    """Join ways that share an endpoint, choosing the straightest continuation.

    OSM splits a motorway or a railway at every junction, so a 700 km line is
    hundreds of short ways.  Endpoints are shared node coordinates, identical to
    six decimals.  At a junction there can be more than one candidate; the one
    that continues the current direction is the main line, while a branch leaves
    at an angle.  Ways are never reused, and a chain is grown from both ends.
    """
    at_end: dict[tuple[float, float], list[tuple[int, int]]] = collections.defaultdict(list)
    for i, way in enumerate(ways):
        nodes = way["nodes"]
        if len(nodes) < 2:
            continue
        at_end[endpoint_key(nodes[0])].append((i, 0))
        at_end[endpoint_key(nodes[-1])].append((i, 1))

    used = [False] * len(ways)
    chains: list[tuple[list[list[float]], list[dict]]] = []
    for start in range(len(ways)):
        if used[start] or len(ways[start]["nodes"]) < 2:
            continue
        used[start] = True
        nodes = list(ways[start]["nodes"])
        members = [ways[start]]

        def extend(grow_end: bool) -> None:
            nonlocal nodes
            while True:
                endpoint = endpoint_key(nodes[-1] if grow_end else nodes[0])
                candidates = [(i, e) for (i, e) in at_end.get(endpoint, []) if not used[i]]
                if not candidates:
                    return
                if grow_end:
                    out = direction(nodes[-2], nodes[-1])
                else:
                    out = direction(nodes[1], nodes[0])
                best = None
                for i, end in candidates:
                    ns = ways[i]["nodes"]
                    if end == 0:
                        p, q = ns[0], ns[1]
                    else:
                        p, q = ns[-1], ns[-2]
                    cost = angle_difference(out, direction(p, q))
                    if best is None or cost < best[0]:
                        best = (cost, i, end)
                if best is None:
                    return
                _, i, end = best
                used[i] = True
                members.append(ways[i])
                segment = ways[i]["nodes"] if end == 0 else list(reversed(ways[i]["nodes"]))
                if grow_end:
                    nodes.extend(segment[1:])
                else:
                    nodes = list(reversed(segment[1:])) + nodes

        extend(True)
        extend(False)
        chains.append((nodes, members))
    return chains


def rasterize(nodes: list[list[float]]) -> set[tuple[int, int]]:
    """Every tile the polyline touches, using an edge-connected staircase."""
    tiles: set[tuple[int, int]] = set()
    previous = None
    for lat, lon in nodes:
        x, y = project(lat, lon)
        xi, yi = int(x), int(y)
        if previous is not None:
            x0, y0 = previous
            dx = 1 if xi > x0 else -1 if xi < x0 else 0
            dy = 1 if yi > y0 else -1 if yi < y0 else 0
            x, y = x0, y0
            while x != xi:
                x += dx
                tiles.add((x, y))
            while y != yi:
                y += dy
                tiles.add((x, y))
        tiles.add((xi, yi))
        previous = (xi, yi)
    return tiles


def dilate(tiles: set[tuple[int, int]], radius: int = 1) -> set[tuple[int, int]]:
    out = set()
    for x, y in tiles:
        for dx in range(-radius, radius + 1):
            for dy in range(-radius, radius + 1):
                out.add((x + dx, y + dy))
    return out


def dedupe_chains(
    chains: list[tuple[str, list[list[float]], list[dict]]],
    overlap: float,
    min_km: float,
) -> list[tuple[str, list[list[float]], list[dict]]]:
    """Drop a chain whose tiles another, longer chain already covers.

    OSM maps both carriageways of an Autobahn and both tracks of a double-track
    railway. At 4096 tiles one tile is about 200 m, so the two
    directions usually land on the same tiles.  The longest chain claims the
    corridor first; a second chain is kept only if less than ``overlap`` of its
    tiles are already covered by a kept chain (including a one-tile halo, so a
    carriageway a few metres to the side still counts as the same road).
    """
    ordered = sorted(chains, key=lambda c: -polyline_km(c[1]))
    accepted: set[tuple[int, int]] = set()
    kept = []
    for label, nodes, members in ordered:
        km = polyline_km(nodes)
        if km < min_km:
            continue
        tiles = rasterize(nodes)
        if not tiles:
            continue
        covered = len(tiles & accepted) / len(tiles)
        if covered > overlap:
            continue
        kept.append((label, nodes, members))
        accepted |= dilate(tiles)
    return kept


# ---------------------------------------------------------------------------
# Simplification
# ---------------------------------------------------------------------------

def rdp(points: list[tuple[float, float]], epsilon: float) -> list[tuple[float, float]]:
    """Ramer-Douglas-Peucker, iterative so a long chain cannot hit recursion."""
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        i, j = stack.pop()
        ax, ay = points[i]
        bx, by = points[j]
        dx, dy = bx - ax, by - ay
        length2 = dx * dx + dy * dy
        best, best_index = -1.0, -1
        for k in range(i + 1, j):
            px, py = points[k]
            if length2 == 0.0:
                distance = math.hypot(px - ax, py - ay)
            else:
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / length2))
                distance = math.hypot(px - (ax + t * dx), py - (ay + t * dy))
            if distance > best:
                best, best_index = distance, k
        if best > epsilon:
            keep[best_index] = True
            stack.append((i, best_index))
            stack.append((best_index, j))
    return [p for p, k in zip(points, keep) if k]


def resample(points: list[tuple[float, float]], max_step: float) -> list[tuple[float, float]]:
    """Split long straight runs so one refused tile costs one piece, not a line."""
    if not points:
        return points
    out = [points[0]]
    for a, b in zip(points, points[1:]):
        distance = math.hypot(b[0] - a[0], b[1] - a[1])
        steps = max(1, int(math.ceil(distance / max_step)))
        for k in range(1, steps + 1):
            out.append((a[0] + (b[0] - a[0]) * k / steps,
                        a[1] + (b[1] - a[1]) * k / steps))
    return out


def simplify(nodes: list[list[float]], tolerance: float, max_step: float) -> list[list[float]]:
    tiles = [project(lat, lon) for lat, lon in nodes]
    simplified = resample(rdp(tiles, tolerance), max_step)
    out = []
    for x, y in simplified:
        lat, lon = unproject(x, y)
        out.append([round(lat, 6), round(lon, 6)])
    return out


# ---------------------------------------------------------------------------
# Network builders
# ---------------------------------------------------------------------------

def normalise_motorway_ref(way: dict) -> str:
    ref = (way.get("ref") or "").strip()
    if not ref:
        return "A?"
    # OSM spells it "A 1" or "A1"; the scenario uses "A1".
    return ref.replace(" ", "")


def build_motorways(ways: list[dict], args) -> list[dict]:
    groups: dict[str, list[dict]] = collections.defaultdict(list)
    for way in ways:
        groups[normalise_motorway_ref(way)].append(way)

    raw = []
    for ref, group in groups.items():
        for nodes, members in chain_ways(group):
            raw.append((ref, nodes, members))
    print("motorway: %d ways, %d raw chains" % (len(ways), len(raw)), file=sys.stderr)

    kept = dedupe_chains(raw, args.dedupe_overlap, args.min_motorway_km)
    print("motorway: %d chains, %.0f km after dedupe" %
          (len(kept), sum(polyline_km(n) for _, n, _ in kept)), file=sys.stderr)

    out = []
    for index, (ref, nodes, _) in enumerate(kept, 1):
        simplified = simplify(nodes, args.tolerance_tiles, args.max_step_tiles)
        out.append({
            "name": ref,
            "class": "motorway",
            "km": round(polyline_km(nodes), 1),
            "piece": "%d/%d" % (index, len(kept)),
            "waypoints": simplified,
        })
    return out


def build_rail(ways: list[dict], args) -> list[dict]:
    groups: dict[str, list[dict]] = collections.defaultdict(list)
    for way in ways:
        # Group by the line number where OSM has one.  Grouping everything
        # together joins unrelated lines at a shared station and produces a
        # 1 300 km chain, which routes as one corridor and hides the topology.
        groups[(way.get("ref") or "").strip() or "?"].append(way)

    raw = []
    for ref, group in groups.items():
        for nodes, members in chain_ways(group):
            names = [m.get("name") for m in members if m.get("name")]
            label = collections.Counter(names).most_common(1)[0][0] if names else ("Strecke %s" % ref if ref != "?" else "Bahnstrecke")
            raw.append((label, nodes, members))
    print("rail: %d ways, %d raw chains" % (len(ways), len(raw)), file=sys.stderr)

    kept = dedupe_chains(raw, args.dedupe_overlap, args.min_rail_km)
    print("rail: %d chains, %.0f km after dedupe" %
          (len(kept), sum(polyline_km(n) for _, n, _ in kept)), file=sys.stderr)

    out = []
    for index, (label, nodes, _) in enumerate(kept, 1):
        simplified = simplify(nodes, args.tolerance_tiles, args.max_step_tiles)
        out.append({
            "name": label,
            "class": "main",
            "km": round(polyline_km(nodes), 1),
            "piece": "%d/%d" % (index, len(kept)),
            "waypoints": simplified,
        })
    return out


# ---------------------------------------------------------------------------
# Stations
# ---------------------------------------------------------------------------

def town_latlon(town: dict) -> tuple[float, float]:
    """towns.json stores the map's normalised latitude and longitude shares."""
    return unproject(town["x"] * (MAP_SIZE - 1.0), town["y"] * (MAP_SIZE - 1.0))


def normalise_name(text: str) -> str:
    text = text.lower()
    for source, target in (("ä", "ae"), ("ö", "oe"), ("ü", "ue"), ("ß", "ss")):
        text = text.replace(source, target)
    return " ".join("".join(c if c.isalnum() else " " for c in text).split())


def station_category(station: dict) -> int | None:
    raw = (station.get("tags") or {}).get("railway:station_category")
    if raw is None:
        return None
    try:
        return int(raw)
    except ValueError:
        return None


def build_stations(towns: list[dict], stations: list[dict], args) -> tuple[list[dict], dict]:
    candidates = [s for s in stations
                  if s.get("type") == "station"
                  and s.get("lat") is not None
                  and s.get("lon") is not None]
    # A DB category is the best signal that this is a mainline station rather
    # than a U-Bahn stop: OSM tags both as railway=station.  Without it, keep
    # only names that read like a Hauptbahnhof.
    filtered = []
    for station in candidates:
        category = station_category(station)
        name = station.get("name") or ""
        normalised = normalise_name(name)
        # A Hauptbahnhof often has a separate underground S-Bahn station
        # ("Hauptbahnhof (tief)", "Hauptbahnhof (Tiefgleise)"). It carries the
        # same DB category and would otherwise win the name bonus, but the
        # mainline platforms are the above-ground station.
        if "tief" in normalised or "unterirdisch" in normalised or "tunnel" in normalised:
            continue
        layer = (station.get("tags") or {}).get("layer")
        try:
            if layer is not None and int(layer) < 0:
                continue
        except ValueError:
            pass
        if category is not None or "hauptbahnhof" in normalised or " hbf" in normalised:
            filtered.append(station)
    print("stations: %d stations, %d mainline candidates" %
          (len(candidates), len(filtered)), file=sys.stderr)

    claimed: set[int] = set()
    selected = []
    missed = []
    for town in sorted(towns, key=lambda t: (-t["population"], t["name"])):
        point = town_latlon(town)
        name = normalise_name(town["name"])
        tokens = [w for w in name.split() if len(w) > 2 and w not in {"am", "an", "der", "im", "in", "bei", "auf"}]

        scored = []
        for station in filtered:
            if station["id"] in claimed:
                continue
            distance = haversine_km(point, (station["lat"], station["lon"]))
            if distance > args.max_station_km:
                continue
            station_name = normalise_name(station.get("name") or "")
            score = -distance
            if station_name.startswith(name):
                score += 60.0
            elif tokens and all(token in station_name for token in tokens):
                score += 40.0
            elif tokens and tokens[0] in station_name:
                score += 20.0
            category = station_category(station)
            if category is not None:
                score += {1: 40, 2: 35, 3: 30, 4: 25, 5: 15, 6: 10, 7: 5}.get(category, 0)
            if "hauptbahnhof" in station_name or " hbf" in station_name:
                score += 20.0
            scored.append((score, distance, station))

        if not scored:
            missed.append(town["name"])
            continue
        scored.sort(key=lambda item: (-item[0], item[1], item[2]["id"]))
        _, distance, station = scored[0]
        claimed.add(station["id"])
        selected.append({
            "name": station.get("name") or town["name"],
            "town": town["name"],
            "lat": round(station["lat"], 6),
            "lon": round(station["lon"], 6),
            "distance_km": round(distance, 2),
            "category": station_category(station),
            "osm_id": station["id"],
        })

    selected.sort(key=lambda s: s["town"])
    print("stations: %d towns matched, %d without a mainline station" %
          (len(selected), len(missed)), file=sys.stderr)
    if missed:
        print("stations: no station for %s" % ", ".join(missed), file=sys.stderr)
    return selected, {"matched": len(selected), "missed": missed}


# ---------------------------------------------------------------------------
# Provenance
# ---------------------------------------------------------------------------

def source_manifest(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        # The header is small; read the first few lines rather than the payload.
        header = []
        for _ in range(12):
            line = f.readline()
            if not line:
                break
            header.append(line)
            if line.strip().startswith('"ways"') or line.strip().startswith('"stations"'):
                break
    text = "".join(header)
    # Pull generated_from out without parsing the 1.6 GB body: the array is
    # complete within the header we read.
    marker = '"generated_from": ['
    start = text.find(marker)
    if start < 0:
        return {}
    end = text.find("]", start)
    if end < 0:
        return {}
    try:
        return {"generated_from": json.loads(text[start + len(marker) - 1:end + 1])}
    except json.JSONDecodeError:
        return {}


def write_json(path: str, value) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(value, f, indent=1, ensure_ascii=False)
        f.write("\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reference-dir", default=REFERENCE,
                        help="directory with osm-{road,rail,stations}.json")
    parser.add_argument("--out-dir", default=GEODATA,
                        help="where to write autobahn-osm.json, rail-osm.json, stations-osm.json")
    parser.add_argument("--towns", default=None,
                        help="towns.json to place stations against "
                             "(default: <out-dir>/towns.json)")
    parser.add_argument("--lat-min", type=float, default=LAT_MIN)
    parser.add_argument("--lat-max", type=float, default=LAT_MAX)
    parser.add_argument("--lon-min", type=float, default=LON_MIN)
    parser.add_argument("--lon-max", type=float, default=LON_MAX)
    parser.add_argument("--size", type=int, default=int(MAP_SIZE),
                        help="map size in tiles, power of two in 64..4096 "
                             "(default: 4096); must match the manifest")
    parser.add_argument("--motorway-ref", default=None,
                        help="only import this OSM ref (for a small test build)")
    parser.add_argument("--tolerance-tiles", type=float, default=2.0,
                        help="RDP tolerance in tiles (default 2.0, about 400 m)")
    parser.add_argument("--max-step-tiles", type=float, default=48.0,
                        help="longest gap between waypoints in tiles (default 48)")
    parser.add_argument("--dedupe-overlap", type=float, default=0.70,
                        help="drop a parallel chain when this share of its tiles is "
                             "already covered (default 0.70)")
    parser.add_argument("--min-motorway-km", type=float, default=0.5,
                        help="drop motorway fragments shorter than this (default 0.5)")
    parser.add_argument("--min-rail-km", type=float, default=0.5,
                        help="drop rail fragments shorter than this (default 0.5)")
    parser.add_argument("--max-station-km", type=float, default=18.0,
                        help="farthest station from a town centre (default 18)")
    parser.add_argument("--check", action="store_true",
                        help="fail if the committed files differ from what this generates")
    args = parser.parse_args(argv)
    configure(args.lat_min, args.lat_max, args.lon_min, args.lon_max, args.size)

    road_path = os.path.join(args.reference_dir, "osm-road.json")
    rail_path = os.path.join(args.reference_dir, "osm-rail.json")
    stations_path = os.path.join(args.reference_dir, "osm-stations.json")
    for path in (road_path, rail_path, stations_path):
        if not os.path.exists(path):
            print("build-osm-network: missing %s (run scripts/osm-reference.py)" % path,
                  file=sys.stderr)
            return 1

    towns_path = args.towns or os.path.join(args.out_dir, "towns.json")
    with open(towns_path, encoding="utf-8") as f:
        towns = json.load(f)

    road_ways = read_ways(road_path, '"class":"motorway"')
    if args.motorway_ref:
        wanted = args.motorway_ref.replace(" ", "")
        road_ways = [w for w in road_ways if normalise_motorway_ref(w) == wanted]
        print("motorway: filtered to %s (%d ways)" % (wanted, len(road_ways)), file=sys.stderr)

    rail_ways = read_ways(rail_path, '"usage":"main"')
    rail_ways = [w for w in rail_ways
                 if w.get("class") == "rail"
                 and (w.get("usage") or "") == "main"
                 and not (w.get("service") or "")]
    stations = read_stations(stations_path)

    autobahn = build_motorways(road_ways, args)
    rail = build_rail(rail_ways, args)
    station_rows, station_stats = build_stations(towns, stations, args)

    provenance = {
        "generated_by": "scripts/build-osm-network.py",
        "license": {
            "name": "Open Database License (ODbL) 1.0",
            "url": "https://opendatacommons.org/licenses/odbl/1-0/",
            "attribution": "© OpenStreetMap contributors",
            "copyright": "https://www.openstreetmap.org/copyright",
            "modification_statement": "docs/ATTRIBUTION.md",
        },
        "projection": {
            "axes": "tile x grows south, tile y grows east (clockwise transposed map)",
            "latitude": "linear, the builder's HsbTileForBounds()",
            "rounding": "lround to the builder's (map_size - 1) grid",
            "reference_tile_fields": "not used: every node is reprojected here",
        },
        "selection": {
            "motorway": "class == motorway; motorway_link is not in the reference",
            "rail": "class == rail and usage == main and service is empty",
            "stations": "type == station with a DB category or a Hauptbahnhof name; "
                        "one per town, at the real OSM position",
        },
        "parameters": {
            "tolerance_tiles": args.tolerance_tiles,
            "max_step_tiles": args.max_step_tiles,
            "dedupe_overlap": args.dedupe_overlap,
            "min_motorway_km": args.min_motorway_km,
            "min_rail_km": args.min_rail_km,
            "max_station_km": args.max_station_km,
        },
        "counts": {
            "motorway_ways": len(road_ways),
            "motorway_corridors": len(autobahn),
            "rail_ways": len(rail_ways),
            "rail_corridors": len(rail),
            "stations": len(station_rows),
            **station_stats,
        },
        "source": {
            "road": source_manifest(road_path),
            "rail": source_manifest(rail_path),
            "stations": source_manifest(stations_path),
        },
    }

    outputs = {
        "autobahn-osm.json": autobahn,
        "rail-osm.json": rail,
        "stations-osm.json": station_rows,
        "osm-import.json": provenance,
    }
    for name, value in outputs.items():
        path = os.path.join(args.out_dir, name)
        if args.check:
            try:
                with open(path, encoding="utf-8") as f:
                    current = json.load(f)
            except (OSError, json.JSONDecodeError):
                print("build-osm-network: %s is missing or unreadable" % path, file=sys.stderr)
                return 1
            if current != value:
                print("build-osm-network: %s is out of date" % path, file=sys.stderr)
                return 1
            continue
        write_json(path, value)
        print("build-osm-network: wrote %s" % path)

    if args.check:
        print("build-osm-network: committed OSM inputs are current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
