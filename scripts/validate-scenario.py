#!/usr/bin/env python3
"""Validate HSB scenario inputs (and the built .scn once it exists).

Pre-editor checks (default run, CI-green without a .scn):
  1. heightmap PNG exists, grayscale, square power-of-2
  2. towns.json schema: name/population/city/x/y, ranges, unique names
  3. cities.csv <-> towns.json name sets match (if --cities given)
  4. industries.json schema: coords in bounds, land/water placement vs the
     heightmap (ports/rigs/fishing = water ok; NOTE: rivers are carved at
     build time, so river-adjacent placements legitimately disagree with the
     raw PNG — the builder is authoritative)
  5. rivers.json schema: class river|sea, width 1-3, waypoints in bounds
     (skipped if --rivers omitted)

Post-build: --scenario <output.scn> checks file size sanity.

Generalized from GermanyTransportTycoonDeluxe scripts/validate-scenario.py.
This file is part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
import argparse
import csv
import json
import math
import os
import stat
import sys

import numpy as np
from PIL import Image

errors = []
missing = object()


def fail(msg):
    errors.append(msg)
    print(f"[validate-scenario] FAIL: {msg}")


def is_finite_number(value):
    return type(value) in (int, float) and (type(value) is int or math.isfinite(value))


def is_regular_file(path, label):
    try:
        file_stat = os.stat(path)
    except (OSError, ValueError) as exc:
        if isinstance(exc, FileNotFoundError):
            fail(f"{label} missing: {path}")
        else:
            fail(f"{label} unreadable: {path}: {exc}")
        return False
    if not stat.S_ISREG(file_stat.st_mode):
        fail(f"{label} is not a regular file: {path}")
        return False
    try:
        with open(path, "rb") as f:
            if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
                fail(f"{label} is not a regular file: {path}")
                return False
            f.read(1)
    except (OSError, ValueError) as exc:
        fail(f"{label} unreadable: {path}: {exc}")
        return False
    return True


def reject_json_constant(value):
    raise ValueError(f"non-finite JSON constant {value}")


def read_json(path, label):
    if not is_regular_file(path, label):
        return missing
    try:
        with open(path, encoding="utf-8") as f:
            if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
                fail(f"{label} is not a regular file: {path}")
                return missing
            return json.load(f, parse_constant=reject_json_constant)
    except json.JSONDecodeError as exc:
        fail(f"{label} invalid JSON: {path}: {exc.msg} at line {exc.lineno} column {exc.colno}")
    except Exception as exc:
        fail(f"{label} unreadable: {path}: {exc}")
    return missing


def validate_bounds(lat_min, lat_max, lon_min, lon_max):
    bounds = (lat_min, lat_max, lon_min, lon_max)
    if not all(math.isfinite(value) for value in bounds):
        fail("WGS84 bounds must be finite")
        return False
    if not (-90 <= lat_min <= 90 and -90 <= lat_max <= 90
            and -180 <= lon_min <= 180 and -180 <= lon_max <= 180):
        fail("WGS84 bounds require latitude in [-90, 90] and longitude in [-180, 180]")
        return False
    if not (lat_min < lat_max and lon_min < lon_max):
        fail("bounds must satisfy lat_min < lat_max and lon_min < lon_max")
        return False
    return True


def load_heightmap(path, strict):
    gray = None
    w = h = None
    try:
        file_stat = os.stat(path)
    except FileNotFoundError:
        msg = f"heightmap missing: {path} (generated artifact)"
        if strict:
            fail(msg)
        else:
            print(f"[validate-scenario] WARNING: {msg}")
        return gray, w, h
    except (OSError, ValueError) as exc:
        fail(f"heightmap unreadable: {path}: {exc}")
        return gray, w, h

    if not stat.S_ISREG(file_stat.st_mode):
        fail(f"heightmap is not a regular file: {path}")
        return gray, w, h

    try:
        with open(path, "rb") as f:
            if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
                fail(f"heightmap is not a regular file: {path}")
                return gray, w, h
            f.seek(0)
            with Image.open(f) as img:
                w, h = img.size
                if (img.mode != "L" or w != h or w < 64 or w > 4096
                        or w & (w - 1)):
                    fail(f"heightmap {path}: must be grayscale, square, power-of-2, "
                         f"and 64..4096 pixels, got {img.mode} {w}x{h}")
                    return gray, w, h
                gray = np.asarray(img)
    except FileNotFoundError:
        fail(f"heightmap missing: {path}")
        return gray, w, h
    except Exception as exc:
        fail(f"heightmap unreadable: {path}: {exc}")
        return gray, w, h

    print(f"[validate-scenario] heightmap OK ({w}x{w} L)")
    return gray, w, h


def lonlat_to_pixel(lat, lon, w, h, lat_min, lat_max, lon_min, lon_max):
    return ((lon - lon_min) / (lon_max - lon_min) * (w - 1),
            (lat_max - lat) / (lat_max - lat_min) * (h - 1))


def validate_towns(path):
    start_errors = len(errors)
    towns = read_json(path, "towns.json")
    if towns is missing:
        return None
    if not isinstance(towns, list):
        fail("towns.json: top-level value must be an array")
        return None
    if not towns:
        fail("towns.json: array must not be empty")

    names = []
    seen_names = set()
    for index, town in enumerate(towns):
        if not isinstance(town, dict):
            fail(f"towns.json: entry {index} must be an object")
            continue

        name = town.get("name")
        if type(name) is not str or not name.strip():
            fail(f"towns.json: entry {index} must have a nonempty string name")
        else:
            names.append(name)
            if name in seen_names:
                fail(f"towns.json: duplicate name {name!r}")
            seen_names.add(name)

        if type(town.get("population")) is not int or town["population"] < 1:
            fail(f"towns.json: bad population for {name!r}")
        if type(town.get("city")) is not bool:
            fail(f"towns.json: bad city flag for {name!r}")
        for key in ("x", "y"):
            value = town.get(key)
            if not is_finite_number(value):
                fail(f"towns.json: {key} must be a finite number for {name!r}")
            elif not 0 < value < 1:
                fail(f"towns.json: {key} out of range for {name!r}")

    if len(errors) == start_errors:
        print(f"[validate-scenario] towns.json OK ({len(towns)} towns)")
    return names


def read_city_names(path):
    start_errors = len(errors)
    if not is_regular_file(path, "cities.csv"):
        return None
    names = set()
    try:
        with open(path, encoding="utf-8", newline="") as f:
            reader = csv.DictReader(f)
            required = {"name", "lat", "lon", "pop_real", "city", "source"}
            if not reader.fieldnames or not required <= set(reader.fieldnames):
                fail("cities.csv: missing required columns")
                return None
            for index, row in enumerate(reader, start=2):
                name = row.get("name")
                if type(name) is not str or not name.strip():
                    fail(f"cities.csv: invalid name at row {index}")
                elif name in names:
                    fail(f"cities.csv: duplicate name {name!r}")
                else:
                    names.add(name)
    except Exception as exc:
        fail(f"cities.csv unreadable: {path}: {exc}")
        return None
    if len(errors) == start_errors:
        return names
    return None


def validate_cities(path, town_names):
    if not path:
        print("[validate-scenario] cities.csv check skipped (--cities omitted)")
        return
    csv_names = read_city_names(path)
    if csv_names is None or town_names is None:
        return
    if csv_names != set(town_names):
        fail(f"cities.csv/towns.json name mismatch: "
             f"only-csv={sorted(csv_names - set(town_names))} only-json={sorted(set(town_names) - csv_names)}")
    else:
        print("[validate-scenario] cities.csv <-> towns.json consistent")


def validate_industries(path, lat_min, lat_max, lon_min, lon_max,
                        allowed, water_ok, gray, w, h):
    start_errors = len(errors)
    industries = read_json(path, "industries.json")
    if industries is missing:
        return
    if not isinstance(industries, list):
        fail("industries.json: top-level value must be an array")
        return

    print(f"[validate-scenario] industries.json: {len(industries)} placements")
    for index, industry in enumerate(industries):
        if not isinstance(industry, dict):
            fail(f"industries.json: entry {index} must be an object")
            continue

        near = industry.get("near", "<unknown>")
        industry_type = industry.get("type")
        valid_type = type(industry_type) is str and bool(industry_type.strip())
        if not valid_type:
            fail(f"industries.json: missing type ({near})")
        elif allowed is not None and industry_type not in allowed:
            fail(f"industries.json: unknown type {industry_type} ({near})")

        coordinates_valid = True
        for key in ("lat", "lon"):
            if not is_finite_number(industry.get(key)):
                fail(f"industries.json: {key} must be a finite number ({near})")
                coordinates_valid = False
        lat = industry.get("lat")
        lon = industry.get("lon")
        if coordinates_valid and not (lat_min <= lat <= lat_max and lon_min <= lon <= lon_max):
            fail(f"industries.json: out of bounds {industry_type!r} {near}")
            coordinates_valid = False

        if gray is not None and valid_type and coordinates_valid:
            c, r = lonlat_to_pixel(lat, lon, w, h,
                                   lat_min, lat_max, lon_min, lon_max)
            ri, ci = int(round(r)), int(round(c))
            if 0 <= ri < h and 0 <= ci < w:
                water = gray[ri, ci] == 0
                if water and industry_type not in water_ok:
                    fail(f"industries.json: {industry_type} ({near}) placed on water")
                if not water and industry_type in water_ok and industry_type != "Port":
                    fail(f"industries.json: {industry_type} ({near}) not on water")

    if len(errors) == start_errors:
        print("[validate-scenario] industries.json OK (types, bounds, land/water)")


def validate_rivers(path, lat_min, lat_max, lon_min, lon_max):
    start_errors = len(errors)
    rivers = read_json(path, "rivers.json")
    if rivers is missing:
        return
    if not isinstance(rivers, list):
        fail("rivers.json: top-level value must be an array")
        return

    for index, segment in enumerate(rivers):
        if not isinstance(segment, dict):
            fail(f"rivers.json: entry {index} must be an object")
            continue
        name = segment.get("name", f"entry {index}")
        if type(segment.get("class")) is not str or segment["class"] not in ("river", "sea"):
            fail(f"rivers.json: bad class in {name!r}")
        width = segment.get("width")
        if type(width) is not int or not 1 <= width <= 3:
            fail(f"rivers.json: bad width in {name!r}")
        waypoints = segment.get("waypoints")
        if not isinstance(waypoints, list) or len(waypoints) < 2:
            fail(f"rivers.json: at least two waypoints required in {name!r}")
            continue
        for waypoint_index, waypoint in enumerate(waypoints):
            if not isinstance(waypoint, list) or len(waypoint) != 2:
                fail(f"rivers.json: waypoint {waypoint_index} in {name!r} must be [lat, lon]")
                continue
            lat, lon = waypoint
            if not is_finite_number(lat) or not is_finite_number(lon):
                fail(f"rivers.json: waypoint {waypoint_index} in {name!r} must contain finite numbers")
            elif not (lat_min <= lat <= lat_max and lon_min <= lon <= lon_max):
                fail(f"rivers.json: waypoint {waypoint_index} out of bounds in {name!r}")

    if len(errors) == start_errors:
        print(f"[validate-scenario] rivers.json OK ({len(rivers)} segments)")


def validate_scenario(path, strict):
    try:
        file_stat = os.stat(path)
    except FileNotFoundError:
        if strict:
            fail(f"scenario missing: {path} (generated artifact)")
        else:
            print(f"[validate-scenario] no {path} yet — build it per docs/SCENARIO.md")
        return
    except (OSError, ValueError) as exc:
        fail(f"scenario unreadable: {path}: {exc}")
        return

    if not stat.S_ISREG(file_stat.st_mode):
        fail(f"scenario is not a regular file: {path}")
        return
    try:
        with open(path, "rb") as f:
            if not stat.S_ISREG(os.fstat(f.fileno()).st_mode):
                fail(f"scenario is not a regular file: {path}")
                return
            size = os.fstat(f.fileno()).st_size
    except (OSError, ValueError) as exc:
        fail(f"scenario unreadable: {path}: {exc}")
        return

    print(f"[validate-scenario] scenario present: {path} ({size} bytes)")
    if size < 100_000:
        fail("scenario suspiciously small (<100kB)")


def main(argv=None):
    errors.clear()
    p = argparse.ArgumentParser(description="Validate HSB scenario inputs")
    p.add_argument("--heightmap", default="heightmap.png")
    p.add_argument("--cities", default=None, help="cities.csv (omit to skip cross-check)")
    p.add_argument("--towns", default="towns.json")
    p.add_argument("--industries", default="industries.json")
    p.add_argument("--rivers", default=None, help="rivers.json (omit to skip)")
    p.add_argument("--scenario", default="output.scn")
    p.add_argument("--lat-min", type=float, required=True)
    p.add_argument("--lat-max", type=float, required=True)
    p.add_argument("--lon-min", type=float, required=True)
    p.add_argument("--lon-max", type=float, required=True)
    p.add_argument("--industry-types", default=None,
                   help="comma-separated allowed industry type names "
                        "(default: accept any non-empty type)")
    p.add_argument("--water-types", default="Port,Oil Rig,Fishing Grounds",
                   help="comma-separated types allowed on water")
    p.add_argument("--strict", action="store_true",
                   help="fail on missing generated artifacts (default: warn)")
    args = p.parse_args(argv)

    lat_min, lat_max, lon_min, lon_max = args.lat_min, args.lat_max, args.lon_min, args.lon_max
    if not validate_bounds(lat_min, lat_max, lon_min, lon_max):
        print(f"[validate-scenario] {len(errors)} FAILURE(S)")
        return 1

    allowed = ({t.strip() for t in args.industry_types.split(",") if t.strip()}
               if args.industry_types else None)
    water_ok = {t.strip() for t in args.water_types.split(",") if t.strip()}

    gray, w, h = load_heightmap(args.heightmap, args.strict)
    town_names = validate_towns(args.towns)
    validate_cities(args.cities, town_names)
    validate_industries(args.industries, lat_min, lat_max, lon_min, lon_max,
                        allowed, water_ok, gray, w, h)
    if args.rivers:
        validate_rivers(args.rivers, lat_min, lat_max, lon_min, lon_max)
    else:
        print("[validate-scenario] rivers.json check skipped (--rivers omitted)")
    validate_scenario(args.scenario, args.strict)

    if errors:
        print(f"[validate-scenario] {len(errors)} FAILURE(S)")
        return 1
    print("[validate-scenario] ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
