#!/usr/bin/env python3
"""Write the HSB scenario-builder manifest (absolute paths, this machine).

The headless builder (HSB_BUILD=manifest openttd -e) resolves heightmap +
towns by basename through the engine file search (scenario/heightmap in the
personal dir), reads industries.json (and optionally rivers.json and the
network files) directly, and saves the .scn into the scenario dir. See
docs/FORMATS.md and builder/hsb_builder.* (patches/01-headless-scenario-builder.diff).

Bounds (--lat-min/max, --lon-min/max) MUST match the bounds used to build
the heightmap (scripts/build-heightmap.py) and towns (scripts/build-towns.py):
they define the lat/lon -> tile mapping for industries, rivers and the network.

This file is part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
import argparse
import json
import math
import os
import re
import stat
import sys
import tempfile

from PIL import Image

errors = []

#: The engine's currency table only has three-letter uppercase codes
#: (src/currency.h). The builder rejects an unknown one rather than falling
#: back silently, so catch the obvious typos before the build does.
CURRENCY_RE = re.compile(r"[A-Z]{3}")

#: The builder's own limits for the network phase, mirrored from
#: builder/hsb_builder_net.h (kHsbNetworkMoney, kHsbBusTowns, kHsbTrains).
#: A limit that exists in the builder but not here is one a caller cannot
#: discover without reading the log.
NETWORK_MONEY_RANGE = (0, 4_000_000_000)
BUS_TOWNS_RANGE = (0, 1024)
TRAINS_RANGE = (0, 1024)

#: Optional network inputs, in the order the builder reads them.
NETWORK_FILES = ("roads", "autobahnen", "rail", "airports", "harbours")


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


def check_range(label, value, bounds):
    lo, hi = bounds
    if not lo <= value <= hi:
        fail(f"{label} must be in {lo}..{hi}, got {value}")
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
    p.add_argument("--snow-line", type=int, default=0,
                   help="snow_line_height levels; 0 derives it from snow coverage")
    p.add_argument("--rotation", default="clockwise",
                   choices=["clockwise", "counter-clockwise"],
                   help="heightmap rotation (must match manual editor flow)")
    p.add_argument("--save", default="output.scn", help="scenario filename in scenario dir")
    p.add_argument("--seed", type=int, default=20250924,
                   help="fixed generation seed for bit-identical rebuilds")
    p.add_argument("--industry-search-radius", type=int, default=32,
                   help="spiral fallback radius (tiles) for industry placement")
    p.add_argument("--max-loan", type=int, default=0,
                   help="difficulty.max_loan; 0 keeps the engine default")
    p.add_argument("--competitors", type=int, default=0,
                   help="company slots (0 = no AI; a scenario with none drops "
                        "the player onto the map as a spectator)")
    p.add_argument("--currency", default="",
                   help="ISO currency code from the engine table, e.g. EUR; "
                        "empty keeps whatever the -c config says")
    p.add_argument("--roads", default=None,
                   help="path to roads.json (network phase: omit to build no roads)")
    p.add_argument("--autobahnen", default=None,
                   help="path to autobahn.json (trunk corridors)")
    p.add_argument("--rail", default=None,
                   help="path to rail.json (main and branch lines)")
    p.add_argument("--airports", default=None,
                   help="path to airports.json (named airports at real positions)")
    p.add_argument("--harbours", default=None,
                   help="path to harbours.json (docks on the coast)")
    p.add_argument("--no-rail", action="store_true",
                   help="skip the rail phase even if other network files are "
                        "named; conflicts with --rail")
    p.add_argument("--no-fleet", action="store_true",
                   help="build the infrastructure but no rolling stock")
    p.add_argument("--network-money", type=int, default=0,
                   help="cash handed to the borrowed infrastructure company for "
                        "the build; 0 uses the builder default and it is handed "
                        "back before the save, so it is a scaffold, not a balance")
    p.add_argument("--bus-towns", type=int, default=None,
                   help="towns that get a depot, two stops and a bus")
    p.add_argument("--trains", type=int, default=None,
                   help="stations that get a spur depot and a train")
    p.add_argument("--out", default="build-manifest.json")
    args = p.parse_args(argv)

    validate_bounds(args.lat_min, args.lat_max, args.lon_min, args.lon_max)
    if not 0 <= args.start_year <= 5_000_000:
        fail("--start-year must be in 0..5000000")
    if not 1 <= args.height <= 255:
        fail("--height must be in 1..255")
    if not 0 <= args.snow_line <= 255:
        fail("--snow-line must be in 0..255")
    if not 0 <= args.seed < 0xFFFFFFFF:
        fail("--seed must be in 0..4294967294")
    if not 1 <= args.industry_search_radius <= 512:
        fail("--industry-search-radius must be in 1..512")
    if not 0 <= args.max_loan <= 4_000_000_000:
        fail("--max-loan must be in 0..4000000000")
    if not 0 <= args.competitors <= 15:
        fail("--competitors must be in 0..15")
    if args.currency and not CURRENCY_RE.fullmatch(args.currency):
        fail(f"--currency must be a 3-letter uppercase code, got {args.currency!r}")
    validate_save_name(args.save)
    check_range("--network-money", args.network_money, NETWORK_MONEY_RANGE)
    if args.bus_towns is not None:
        check_range("--bus-towns", args.bus_towns, BUS_TOWNS_RANGE)
    if args.trains is not None:
        check_range("--trains", args.trains, TRAINS_RANGE)

    network_wanted = {name: getattr(args, name) for name in NETWORK_FILES}
    if args.rail and args.no_rail:
        fail("--rail and --no-rail disagree; the builder refuses to name a rail "
             "file it will not build, because track without stations looks broken")

    input_paths = {
        "heightmap": os.path.abspath(args.heightmap),
        "towns": os.path.abspath(args.towns),
        "industries": os.path.abspath(args.industries),
    }
    if args.rivers:
        input_paths["rivers"] = os.path.abspath(args.rivers)
    for name, path in network_wanted.items():
        if path:
            input_paths[name] = os.path.abspath(path)

    #: The infrastructure company that owns the network takes one company slot.
    #: With zero or one slot the player opens the save as a spectator with
    #: nothing to build, which no exit code reveals - only a playthrough does.
    if any(network_wanted.values()) and args.competitors < 2:
        fail("--competitors must be at least 2 when a network is built: the "
             f"infrastructure company takes one slot and the player needs another "
             f"(got {args.competitors})")

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
        "competitors": args.competitors,
        "save": args.save,
    }
    if args.snow_line > 0:
        manifest["snow_line_height"] = args.snow_line
    if args.max_loan > 0:
        manifest["max_loan"] = args.max_loan
    if args.currency:
        manifest["currency"] = args.currency
    if args.rivers:
        manifest["rivers"] = input_paths["rivers"]
    for name, path in network_wanted.items():
        if path:
            manifest[name] = input_paths[name]
    if any(network_wanted.values()):
        # Only written when a network is actually built, so a manifest with no
        # network files is byte-identical to one from before the network phase
        # existed.
        manifest["with_rail"] = not args.no_rail
        manifest["with_fleet"] = not args.no_fleet
        if args.network_money > 0:
            manifest["network_money"] = args.network_money
        if args.bus_towns is not None:
            manifest["bus_towns"] = args.bus_towns
        if args.trains is not None:
            manifest["trains"] = args.trains

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
