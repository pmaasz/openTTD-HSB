#!/usr/bin/env python3
"""Write the HSB scenario-builder manifest (absolute paths, this machine).

The headless builder (HSB_BUILD=manifest openttd -e) resolves heightmap +
towns by basename through the engine file search (scenario/heightmap in the
personal dir), reads industries.json (and optionally rivers.json) directly,
and saves the .scn into the scenario dir. See docs/FORMATS.md and
builder/hsb_builder.* (patches/01-headless-scenario-builder.diff).

Bounds (--lat-min/max, --lon-min/max) MUST match the bounds used to build
the heightmap (scripts/build-heightmap.py) and towns (scripts/build-towns.py):
they define the lat/lon -> tile mapping for industries and rivers.

This file is part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
import argparse
import json
import os
import sys


def main(argv=None):
    p = argparse.ArgumentParser(description="Write HSB builder manifest")
    p.add_argument("--heightmap", required=True, help="path to heightmap PNG (power of 2)")
    p.add_argument("--towns", required=True, help="path to towns.json (town-import format)")
    p.add_argument("--industries", required=True, help="path to industries.json")
    p.add_argument("--rivers", default=None, help="path to rivers.json (omit to skip waters)")
    p.add_argument("--lat-min", type=float, required=True)
    p.add_argument("--lat-max", type=float, required=True)
    p.add_argument("--lon-min", type=float, required=True)
    p.add_argument("--lon-max", type=float, required=True)
    p.add_argument("--start-year", type=int, default=2025)
    p.add_argument("--height", type=int, default=12, help="heightmap_height levels")
    p.add_argument("--rotation", default="clockwise",
                   choices=["clockwise", "counter-clockwise"],
                   help="heightmap rotation (must match manual editor flow)")
    p.add_argument("--save", default="output.scn", help="scenario filename in scenario dir")
    p.add_argument("--seed", type=int, default=20250924,
                   help="fixed generation seed for bit-identical rebuilds")
    p.add_argument("--industry-search-radius", type=int, default=32,
                   help="spiral fallback radius (tiles) for industry placement")
    p.add_argument("--out", default="build-manifest.json")
    args = p.parse_args(argv)

    if not (args.lat_min < args.lat_max and args.lon_min < args.lon_max):
        print("[build-manifest] bounds must satisfy lat_min < lat_max, lon_min < lon_max",
              file=sys.stderr)
        return 1

    manifest = {
        "heightmap": os.path.abspath(args.heightmap),
        "towns": os.path.abspath(args.towns),
        "industries": os.path.abspath(args.industries),
        "bounds": {
            "lat_min": args.lat_min,
            "lat_max": args.lat_max,
            "lon_min": args.lon_min,
            "lon_max": args.lon_max,
        },
        "start_year": args.start_year,
        "heightmap_height": args.height,
        "heightmap_rotation": args.rotation,
        "seed": args.seed,
        "industry_search_radius": args.industry_search_radius,
        "save": args.save,
    }
    if args.rivers:
        manifest["rivers"] = os.path.abspath(args.rivers)
    for key in ("heightmap", "towns", "industries"):
        if not os.path.exists(manifest[key]):
            print(f"[build-manifest] missing: {manifest[key]}", file=sys.stderr)
            return 1
    if args.rivers and not os.path.exists(manifest["rivers"]):
        print(f"[build-manifest] missing rivers: {manifest['rivers']}", file=sys.stderr)
        return 1
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print(f"[build-manifest] wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
