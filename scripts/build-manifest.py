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
import math
import os
import stat
import sys
import tempfile

from PIL import Image

errors = []


def fail(msg):
    errors.append(msg)
    print(f"[build-manifest] FAIL: {msg}", file=sys.stderr)


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


def validate_heightmap(path):
    if not is_regular_file(path, "heightmap"):
        return
    try:
        with Image.open(path) as image:
            valid = (image.format == "PNG" and image.mode == "L"
                     and image.size[0] == image.size[1]
                     and 64 <= image.size[0] <= 4096
                     and image.size[0] & (image.size[0] - 1) == 0)
            if not valid:
                fail(f"heightmap must be PNG, grayscale, square, power-of-2, "
                     f"and 64..4096 pixels, got {image.format} {image.mode} {image.size[0]}x{image.size[1]}")
    except (OSError, ValueError) as exc:
        fail(f"heightmap unreadable: {path}: {exc}")


def validate_bounds(lat_min, lat_max, lon_min, lon_max):
    bounds = (lat_min, lat_max, lon_min, lon_max)
    if not all(math.isfinite(value) for value in bounds):
        fail("WGS84 bounds must be finite")
        return
    if not (-90 <= lat_min <= 90 and -90 <= lat_max <= 90
            and -180 <= lon_min <= 180 and -180 <= lon_max <= 180):
        fail("WGS84 bounds require latitude in [-90, 90] and longitude in [-180, 180]")
        return
    if not (lat_min < lat_max and lon_min < lon_max):
        fail("bounds must satisfy lat_min < lat_max and lon_min < lon_max")


def validate_save_name(name):
    if (type(name) is not str or not name or not name.strip()
            or name in (".", "..") or "/" in name or "\\" in name
            or ":" in name or "\x00" in name or os.path.basename(name) != name):
        fail("--save must be a nonempty basename without path traversal")


def validate_output(path, reserved_paths):
    absolute = os.path.abspath(path)
    canonical = os.path.realpath(absolute)
    if any(canonical == os.path.realpath(reserved) for reserved in reserved_paths):
        fail(f"output path must differ from manifest inputs: {absolute}")
        return False
    if os.path.lexists(absolute):
        try:
            output_stat = os.stat(absolute)
        except (OSError, ValueError) as exc:
            fail(f"output path unreadable: {absolute}: {exc}")
            return False
        if not stat.S_ISREG(output_stat.st_mode):
            fail(f"output path is not a regular file: {absolute}")
            return False
    return True


def main(argv=None):
    errors.clear()
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

    validate_bounds(args.lat_min, args.lat_max, args.lon_min, args.lon_max)
    if not 0 <= args.start_year <= 5_000_000:
        fail("--start-year must be in 0..5000000")
    if not 1 <= args.height <= 255:
        fail("--height must be in 1..255")
    if not 0 <= args.seed < 0xFFFFFFFF:
        fail("--seed must be in 0..4294967294")
    if not 1 <= args.industry_search_radius <= 512:
        fail("--industry-search-radius must be in 1..512")
    validate_save_name(args.save)

    input_paths = {
        "heightmap": os.path.abspath(args.heightmap),
        "towns": os.path.abspath(args.towns),
        "industries": os.path.abspath(args.industries),
    }
    if args.rivers:
        input_paths["rivers"] = os.path.abspath(args.rivers)
    validate_output(args.out, set(input_paths.values()))
    for label, path in input_paths.items():
        is_regular_file(path, label)
    validate_heightmap(input_paths["heightmap"])

    if errors:
        print(f"[build-manifest] {len(errors)} FAILURE(S)", file=sys.stderr)
        return 1

    manifest = {
        "heightmap": input_paths["heightmap"],
        "towns": input_paths["towns"],
        "industries": input_paths["industries"],
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
        manifest["rivers"] = input_paths["rivers"]

    try:
        payload = json.dumps(manifest, indent=2, allow_nan=False)
    except (TypeError, ValueError) as exc:
        fail(f"manifest is not strict JSON: {exc}")
        print(f"[build-manifest] {len(errors)} FAILURE(S)", file=sys.stderr)
        return 1

    output_path = os.path.abspath(args.out)
    output_parent = os.path.dirname(output_path)
    temporary_path = None
    try:
        os.makedirs(output_parent, exist_ok=True)
        descriptor, temporary_name = tempfile.mkstemp(
            prefix=".build-manifest-", dir=output_parent)
        temporary_path = temporary_name
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, output_path)
        temporary_path = None
    except (OSError, UnicodeError, ValueError) as exc:
        fail(f"cannot write output {args.out}: {exc}")
        print(f"[build-manifest] {len(errors)} FAILURE(S)", file=sys.stderr)
        return 1
    finally:
        if temporary_path is not None:
            try:
                os.unlink(temporary_path)
            except FileNotFoundError:
                pass

    print(f"[build-manifest] wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
