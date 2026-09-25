#!/usr/bin/env python3
"""Build OpenTTD town-import JSON from a cities CSV.

Input CSV columns: name,lat,lon,pop_real,city,source
  lat/lon: WGS84. pop_real: real inhabitants.
  city: 1 for OpenTTD cities (large metros). source: free-form provenance.

Conversion (per upstream docs/importing_town_data.md):
  lon/lat -> heightmap pixel (col,row) -> JSON x=r/(H-1), y=c/(W-1) (axes swapped).
  Population scaling: target = max(POP_MIN, round(real / POP_DIVISOR)).

Validation: every town is sampled against the built heightmap PNG; towns on
water are auto-nudged to nearest land within NUDGE_PX and the nudge distance
is recorded in the debug CSV.

Bounds (--lat-min/max, --lon-min/max) MUST match scripts/build-heightmap.py.

Generalized from GermanyTransportTycoonDeluxe scripts/build-towns.py.
This file is part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
import argparse
import csv
import json
import math
import os
import sys
import time
import urllib.parse
import urllib.request

import numpy as np
from PIL import Image

GEOCODE_URL = ("https://nominatim.openstreetmap.org/search?"
               "limit=1&format=json&featuretype=city&q=")


def geocode(name, cache, country_suffix="", sleep=1.1):
    if name in cache:
        return cache[name]
    time.sleep(sleep)  # Nominatim usage policy
    q = name + country_suffix
    url = GEOCODE_URL + urllib.parse.quote(q)
    req = urllib.request.Request(url, headers={"User-Agent": "HSB-geocode/1.0"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.load(resp)
    if not data:
        raise RuntimeError(f"geocode: no result for {name!r}")
    lat, lon = float(data[0]["lat"]), float(data[0]["lon"])
    cache[name] = (lat, lon)
    return lat, lon


def lonlat_to_pixel(lat, lon, w, h, lat_min, lat_max, lon_min, lon_max):
    col = (lon - lon_min) / (lon_max - lon_min) * (w - 1)
    row = (lat_max - lat) / (lat_max - lat_min) * (h - 1)
    return col, row


def nearest_land(gray, col, row, max_px):
    h, w = gray.shape
    c0, r0 = int(round(col)), int(round(row))
    if 0 < r0 < h - 1 and 0 < c0 < w - 1 and gray[r0, c0] > 0:
        return c0, r0, 0
    for d in range(1, max_px + 1):
        for dr in range(-d, d + 1):
            for dc in (-d, d):
                r, c = r0 + dr, c0 + dc
                if 0 < r < h - 1 and 0 < c < w - 1 and gray[r, c] > 0:
                    return c, r, d
        for dc in range(-d + 1, d):
            for dr in (-d, d):
                r, c = r0 + dr, c0 + dc
                if 0 < r < h - 1 and 0 < c < w - 1 and gray[r, c] > 0:
                    return c, r, d
    return None


def main(argv=None):
    p = argparse.ArgumentParser(description="Build towns.json for HSB")
    p.add_argument("--cities", default="cities.csv")
    p.add_argument("--heightmap", default="heightmap.png")
    p.add_argument("--out", default="towns.json")
    p.add_argument("--debug-out", default="towns.debug.csv")
    p.add_argument("--lat-min", type=float, required=True)
    p.add_argument("--lat-max", type=float, required=True)
    p.add_argument("--lon-min", type=float, required=True)
    p.add_argument("--lon-max", type=float, required=True)
    p.add_argument("--pop-divisor", type=float, default=450)
    p.add_argument("--pop-min", type=int, default=300)
    p.add_argument("--nudge-px", type=int, default=4)
    p.add_argument("--sample", action="store_true", help="Top entries (city=1 or pop>=100k) only")
    p.add_argument("--strict", action="store_true",
                   help="exit non-zero for water-placed towns (default: warn)")
    p.add_argument("--geocode", metavar="NAMES_TXT",
                   help="geocode names (one per line: name,pop) and print CSV rows")
    p.add_argument("--geocode-suffix", default="",
                   help="suffix appended to geocode queries, e.g. ', Deutschland'")
    args = p.parse_args(argv)

    if args.geocode:
        cache = {}
        with open(args.geocode, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                name, pop = [s.strip() for s in line.split(",", 1)]
                try:
                    lat, lon = geocode(name, cache, country_suffix=args.geocode_suffix)
                    print(f"{name},{lat:.4f},{lon:.4f},{pop},0,estimate")
                except Exception as e:  # noqa: BLE001 - report and continue
                    print(f"# FAILED {name}: {e}")
        return 0

    bounds = (args.lat_min, args.lat_max, args.lon_min, args.lon_max)
    if (not all(math.isfinite(value) for value in bounds)
            or not (-90.0 <= args.lat_min < args.lat_max <= 90.0)
            or not (-180.0 <= args.lon_min < args.lon_max <= 180.0)):
        print("error: bounds must be finite WGS84 intervals", file=sys.stderr)
        return 2
    if not math.isfinite(args.pop_divisor) or args.pop_divisor <= 0:
        print("error: --pop-divisor must be finite and positive", file=sys.stderr)
        return 2
    if args.pop_min < 1 or args.nudge_px < 0:
        print("error: --pop-min must be positive and --nudge-px nonnegative", file=sys.stderr)
        return 2

    try:
        gray = np.asarray(Image.open(args.heightmap).convert("L"))
    except (FileNotFoundError, OSError) as exc:
        print(f"error: cannot read heightmap {args.heightmap}: {exc}", file=sys.stderr)
        return 1
    H, W = gray.shape
    if H != W or not 64 <= H <= 4096 or H & (H - 1):
        print(f"error: heightmap must be square power-of-2 in 64..4096, got {W}x{H}", file=sys.stderr)
        return 1

    towns, debug = [], []
    water_fails = []
    with open(args.cities, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            try:
                pop_real = int(row["pop_real"])
                lat, lon = float(row["lat"]), float(row["lon"])
            except (KeyError, TypeError, ValueError) as exc:
                print(f"error: invalid cities.csv row: {exc}", file=sys.stderr)
                return 1
            if (pop_real < 1 or not math.isfinite(lat) or not math.isfinite(lon)
                    or not (args.lat_min <= lat <= args.lat_max)
                    or not (args.lon_min <= lon <= args.lon_max)):
                print(f"error: invalid city coordinates or population for {row.get('name', '')!r}", file=sys.stderr)
                return 1
            if args.sample and not (row["city"] == "1" or pop_real >= 100000):
                continue
            col, row_px = lonlat_to_pixel(lat, lon, W, H,
                                          args.lat_min, args.lat_max,
                                          args.lon_min, args.lon_max)
            hit = nearest_land(gray, col, row_px, args.nudge_px)
            if hit is None:
                water_fails.append(row["name"])
                continue
            c, r, nudge = hit
            # Axes swapped vs image editors (upstream town-import doc).
            towns.append({
                "name": row["name"],
                "population": max(args.pop_min, round(pop_real / args.pop_divisor)),
                "city": row["city"] == "1",
                "x": r / (H - 1),
                "y": c / (W - 1),
            })
            debug.append({
                "name": row["name"], "lat": lat, "lon": lon,
                "col": round(col, 1), "row": round(row_px, 1),
                "nudge_px": nudge, "pop_real": pop_real,
                "pop_game": max(args.pop_min, round(pop_real / args.pop_divisor)),
                "city": row["city"], "source": row["source"],
            })

    if not towns:
        print("error: no towns were generated", file=sys.stderr)
        return 1
    print(f"[build-towns] towns={len(towns)} cities={sum(1 for t in towns if t['city'])}")
    if water_fails:
        print(f"[build-towns] ON WATER (no land within {args.nudge_px}px): {', '.join(water_fails)}")
        if args.strict:
            return 1
        print("[build-towns] non-strict mode: continuing despite water placements")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(towns, f, ensure_ascii=False, indent=2, allow_nan=False)
    os.makedirs(os.path.dirname(os.path.abspath(args.debug_out)), exist_ok=True)
    with open(args.debug_out, "w", encoding="utf-8", newline="") as f:
        fieldnames = ["name", "lat", "lon", "col", "row", "nudge_px",
                      "pop_real", "pop_game", "city", "source"]
        w = csv.DictWriter(f, fieldnames=fieldnames)
        w.writeheader()
        w.writerows(debug)
    nudged = sum(1 for d in debug if d["nudge_px"] > 0)
    print(f"[build-towns] wrote {args.out} + {args.debug_out} (nudged={nudged})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
