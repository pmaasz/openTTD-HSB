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
import os
import sys

import numpy as np
from PIL import Image

errors = []


def fail(msg):
    errors.append(msg)
    print(f"[validate-scenario] FAIL: {msg}")


def lonlat_to_pixel(lat, lon, w, h, lat_min, lat_max, lon_min, lon_max):
    return (lon - lon_min) / (lon_max - lon_min) * w, (lat_max - lat) / (lat_max - lat_min) * h


def main(argv=None):
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
    allowed = ({t.strip() for t in args.industry_types.split(",") if t.strip()}
               if args.industry_types else None)
    water_ok = {t.strip() for t in args.water_types.split(",") if t.strip()}

    # 1. heightmap
    try:
        img = Image.open(args.heightmap)
        w, h = img.size
        if img.mode != "L" or w != h or w & (w - 1) or w < 64:
            fail(f"heightmap {args.heightmap}: must be square power-of-2 grayscale, got {img.mode} {w}x{h}")
        else:
            print(f"[validate-scenario] heightmap OK ({w}x{w} L)")
            gray = np.asarray(img)
    except FileNotFoundError:
        msg = f"heightmap missing: {args.heightmap} (generated artifact)"
        if args.strict:
            fail(msg)
        else:
            print(f"[validate-scenario] WARNING: {msg}")
        gray = None
        w = h = 2048

    # 2. towns.json
    with open(args.towns, encoding="utf-8") as f:
        towns = json.load(f)
    names = [t.get("name") for t in towns]
    if len(set(names)) != len(names):
        fail("towns.json: duplicate names")
    for t in towns:
        if not isinstance(t.get("population"), int) or t["population"] < 1:
            fail(f"towns.json: bad population for {t.get('name')}")
        if not isinstance(t.get("city"), bool):
            fail(f"towns.json: bad city flag for {t.get('name')}")
        for k in ("x", "y"):
            if not 0 <= t.get(k, -1) <= 1:
                fail(f"towns.json: {k} out of range for {t.get('name')}")
    print(f"[validate-scenario] towns.json OK ({len(towns)} towns)")

    # 3. cities.csv <-> towns.json
    if args.cities:
        with open(args.cities, encoding="utf-8") as f:
            csv_names = {r["name"] for r in csv.DictReader(f)}
        if csv_names != set(names):
            fail(f"cities.csv/towns.json name mismatch: "
                 f"only-csv={sorted(csv_names - set(names))} only-json={sorted(set(names) - csv_names)}")
        else:
            print("[validate-scenario] cities.csv <-> towns.json consistent")
    else:
        print("[validate-scenario] cities.csv check skipped (--cities omitted)")

    # 4. industries.json
    with open(args.industries, encoding="utf-8") as f:
        inds = json.load(f)
    print(f"[validate-scenario] industries.json: {len(inds)} placements")
    for it in inds:
        if not isinstance(it.get("type"), str) or not it["type"]:
            fail(f"industries.json: missing type ({it.get('near')})")
        elif allowed is not None and it["type"] not in allowed:
            fail(f"industries.json: unknown type {it['type']} ({it.get('near')})")
        if not (lat_min <= it["lat"] <= lat_max and lon_min <= it["lon"] <= lon_max):
            fail(f"industries.json: out of bounds {it['type']} {it.get('near')}")
        if gray is not None:
            c, r = lonlat_to_pixel(it["lat"], it["lon"], w, h,
                                   lat_min, lat_max, lon_min, lon_max)
            ri, ci = int(round(r)), int(round(c))
            if 0 <= ri < h and 0 <= ci < w:
                water = gray[ri, ci] == 0
                if water and it["type"] not in water_ok:
                    fail(f"industries.json: {it['type']} ({it.get('near')}) placed on water")
                if not water and it["type"] in water_ok and it["type"] != "Port":
                    fail(f"industries.json: {it['type']} ({it.get('near')}) not on water")
    if not any(e.startswith("industries.json") for e in errors):
        print("[validate-scenario] industries.json OK (types, bounds, land/water)")

    # 5. rivers.json
    if args.rivers:
        with open(args.rivers, encoding="utf-8") as f:
            rivers = json.load(f)
        for seg in rivers:
            if seg.get("class") not in ("river", "sea"):
                fail(f"rivers.json: bad class in {seg.get('name')}")
            if not 1 <= seg.get("width", 0) <= 3:
                fail(f"rivers.json: bad width in {seg.get('name')}")
            for lat, lon in seg.get("waypoints", []):
                if not (lat_min <= lat <= lat_max and lon_min <= lon <= lon_max):
                    fail(f"rivers.json: waypoint out of bounds in {seg.get('name')}")
        if not any(e.startswith("rivers.json") for e in errors):
            print(f"[validate-scenario] rivers.json OK ({len(rivers)} segments)")
    else:
        print("[validate-scenario] rivers.json check skipped (--rivers omitted)")

    # 6. built scenario (builder output, git-ignored)
    if os.path.exists(args.scenario):
        size = os.path.getsize(args.scenario)
        print(f"[validate-scenario] scenario present: {args.scenario} ({size} bytes)")
        if size < 100_000:
            fail("scenario suspiciously small (<100kB)")
    else:
        print(f"[validate-scenario] no {args.scenario} yet — build it per docs/SCENARIO.md")

    if errors:
        print(f"[validate-scenario] {len(errors)} FAILURE(S)")
        return 1
    print("[validate-scenario] ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
