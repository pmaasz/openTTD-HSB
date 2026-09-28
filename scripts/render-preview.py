#!/usr/bin/env python3
"""Render a placement preview: heightmap terrain + rivers + towns + industries.

Two jobs, one deterministic PNG.

1. A tracing guide. The heightmap is a grey raster nobody can read; overlay
   the town and industry anchors on coloured terrain and you can see at a
   glance that a town sits in a lake, that two anchors are 300 km apart, or
   that a river never reaches the coast.

2. A gate. It exits non-zero when a town anchor lands on water, so a mistake
   is caught before the engine is started instead of after. This is the check
   docs/SCENARIO.md used to leave to a human with an editor open.

Bounds are arguments, not constants: the same script previews any region (the
bounds must agree with the heightmap's, --lat-min and friends).

Deterministic: same inputs produce a byte-identical PNG.
"""
import argparse
import json
import math
import os
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

TILE_SIZE = 256
#: Terrain ramp: (grey level, (r, g, b)). Grey 0 is sea by definition
#: (see docs/FORMATS.md), so it is the water end of the scale.
STOPS = [
    (0, (26, 54, 134)),
    (1, (62, 120, 60)),
    (40, (96, 154, 74)),
    (110, (178, 158, 104)),
    (180, (150, 132, 118)),
    (255, (238, 238, 238)),
]
RIVER_COLOR = (90, 190, 235)
TOWN_COLOR = (220, 30, 30)
CITY_RING = (255, 255, 255)
INDUSTRY_COLOR = (0, 200, 220)


def fail(message):
    print(f"[render-preview] error: {message}", file=sys.stderr)
    return 2


def read_json(path, label):
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle, parse_constant=_reject_constant)
    except FileNotFoundError:
        raise ValueError(f"{label} not found: {path}")
    except json.JSONDecodeError as exc:
        raise ValueError(f"{label} is not valid JSON: {exc}")
    except OSError as exc:
        raise ValueError(f"{label} is unreadable: {exc}")


def _reject_constant(value):
    raise ValueError(f"non-finite JSON constant {value}")


def colormap(gray):
    """Map the 8-bit heightmap onto the terrain ramp."""
    out = np.zeros(gray.shape + (3,), dtype=np.uint8)
    for (g0, c0), (g1, c1) in zip(STOPS[:-1], STOPS[1:]):
        mask = (gray >= g0) & (gray <= g1)
        blend = np.zeros_like(gray, dtype=np.float64)
        if g1 > g0:
            blend[mask] = (gray[mask].astype(np.float64) - g0) / (g1 - g0)
        for channel in range(3):
            out[mask, channel] = np.round(
                c0[channel] + (c1[channel] - c0[channel]) * blend[mask]).astype(np.uint8)
    return out


def write_atomic(img, path):
    """Write via a temp file in the destination directory, then rename.

    An interrupted render must not leave a half-written PNG that the next step
    happily consumes (build-manifest.py already writes atomically; this keeps
    that promise across the whole pipeline).
    """
    parent = os.path.dirname(os.path.abspath(path)) or "."
    os.makedirs(parent, exist_ok=True)
    handle, temp = tempfile.mkstemp(prefix=".render-preview-", dir=parent)
    os.close(handle)
    try:
        img.save(temp, format="PNG", optimize=True)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Render a placement preview for HSB")
    parser.add_argument("--heightmap", required=True)
    parser.add_argument("--towns", required=True)
    parser.add_argument("--industries", required=True)
    parser.add_argument("--rivers", help="optional rivers.json; drawn on top of the terrain")
    parser.add_argument("--lat-min", type=float, required=True)
    parser.add_argument("--lat-max", type=float, required=True)
    parser.add_argument("--lon-min", type=float, required=True)
    parser.add_argument("--lon-max", type=float, required=True)
    parser.add_argument("--size", type=int, default=1024, help="output pixels (power of 2)")
    parser.add_argument("--out", required=True)
    parser.add_argument("--palette", action="store_true",
                        help="quantize to 255 colours for a small committed file")
    parser.add_argument("--allow-town-on-water", action="store_true",
                        help="report town/water collisions but exit 0")
    args = parser.parse_args(argv)

    bounds = (args.lat_min, args.lat_max, args.lon_min, args.lon_max)
    if (not all(math.isfinite(value) for value in bounds)
            or not -90.0 <= args.lat_min < args.lat_max <= 90.0
            or not -180.0 <= args.lon_min < args.lon_max <= 180.0):
        return fail("bounds must be finite, ordered WGS84 intervals")
    if args.size & (args.size - 1) or not 64 <= args.size <= 4096:
        return fail(f"--size must be a power of 2 in 64..4096, got {args.size}")

    try:
        image = Image.open(args.heightmap)
    except FileNotFoundError:
        return fail(f"heightmap not found: {args.heightmap}")
    except OSError as exc:
        return fail(f"heightmap is unreadable: {exc}")
    if image.mode != "L":
        return fail(f"heightmap must be 8-bit grayscale (mode L), got {image.mode}")
    if image.width != image.height:
        return fail(f"heightmap must be square, got {image.width}x{image.height}")
    gray = np.asarray(image, dtype=np.uint8)

    try:
        towns = read_json(args.towns, "towns")
        industries = read_json(args.industries, "industries")
        rivers = read_json(args.rivers, "rivers") if args.rivers else []
    except ValueError as exc:
        return fail(str(exc))
    if not isinstance(towns, list) or not towns:
        return fail("towns root must be a nonempty array")
    if not isinstance(industries, list):
        return fail("industries root must be an array")
    if not isinstance(rivers, list):
        return fail("rivers root must be an array")

    # Terrain is quantised before the markers are drawn: a terrain-only palette
    # has no red or cyan in it and would swallow the town dots.
    preview = Image.fromarray(colormap(gray), mode="RGB").resize(
        (args.size, args.size), Image.LANCZOS)
    if args.palette:
        preview = preview.convert("P", palette=Image.Palette.ADAPTIVE, colors=255,
                                  dither=Image.Dither.NONE).convert("RGB")
    draw = ImageDraw.Draw(preview)
    scale = args.size / 2048.0

    def lon_to_x(lon):
        return (lon - args.lon_min) / (args.lon_max - args.lon_min) * (args.size - 1)

    def lat_to_y(lat):
        return (args.lat_max - lat) / (args.lat_max - args.lat_min) * (args.size - 1)

    rivers_drawn = 0
    for segment in rivers:
        if not isinstance(segment, dict):
            return fail("each river entry must be an object")
        waypoints = segment.get("waypoints")
        if not isinstance(waypoints, list) or len(waypoints) < 2:
            return fail("each river needs at least two waypoints")
        for waypoint in waypoints:
            if (not isinstance(waypoint, list) or len(waypoint) != 2
                    or not all(isinstance(value, (int, float)) and math.isfinite(value) for value in waypoint)):
                return fail("river waypoints must be finite [lat, lon] pairs")
        # The engine interpolates the segment at 0.002 degree steps, so sample it
        # the same way here; otherwise the drawn line and the carved river differ
        # wherever a segment is long.
        points = []
        for index in range(len(waypoints) - 1):
            lat0, lon0 = waypoints[index]
            lat1, lon1 = waypoints[index + 1]
            steps = max(int(round(max(abs(lat1 - lat0), abs(lon1 - lon0)) / 0.002)), 1)
            for step in range(steps + 1):
                fraction = step / steps
                points.append((lon_to_x(lon0 + (lon1 - lon0) * fraction),
                               lat_to_y(lat0 + (lat1 - lat0) * fraction)))
        width = max(int(round(int(segment.get("width", 1)) * scale * 1.5)), 1)
        draw.line(points, fill=RIVER_COLOR, width=width, joint="curve")
        rivers_drawn += 1

    # towns.json uses the engine's swapped axes: x is a row (lat), y a column (lon).
    small = np.asarray(Image.fromarray(gray, mode="L").resize(
        (args.size, args.size), Image.BILINEAR))
    on_water = []
    for town in towns:
        if not isinstance(town, dict):
            return fail("each town entry must be an object")
        try:
            col = int(round(town["y"] * (args.size - 1)))
            row = int(round(town["x"] * (args.size - 1)))
            population = int(town["population"])
        except (KeyError, TypeError, ValueError):
            return fail(f"town entry is missing name/population/x/y: {town}")
        if not 0 <= col < args.size or not 0 <= row < args.size:
            return fail(f"town {town.get('name')!r} is outside the map")
        if small[row, col] == 0:
            on_water.append(town.get("name", "?"))
        # Radius scales with population, but a 300-population village at --size
        # 512 is sub-pixel and disappears, so keep an absolute floor.
        radius = max(max(math.sqrt(population / 300.0) * 2, 2.5) * scale, 2.5)
        city = bool(town.get("city", False))
        draw.ellipse([col - radius, row - radius, col + radius, row + radius],
                     fill=TOWN_COLOR,
                     outline=CITY_RING if city else None,
                     width=max(1, int(2 * scale)) if city else 1)

    drawn_industries = 0
    for entry in industries:
        if not isinstance(entry, dict):
            return fail("each industry entry must be an object")
        try:
            lat, lon = float(entry["lat"]), float(entry["lon"])
        except (KeyError, TypeError, ValueError):
            return fail(f"industry entry is missing lat/lon: {entry}")
        if not (math.isfinite(lat) and math.isfinite(lon)):
            return fail(f"industry {entry.get('type')!r} has non-finite coordinates")
        half = max(5 * scale, 2.5)
        x, y = lon_to_x(lon), lat_to_y(lat)
        draw.rectangle([x - half, y - half, x + half, y + half],
                       fill=INDUSTRY_COLOR, outline=CITY_RING, width=max(1, int(scale)))
        drawn_industries += 1

    write_atomic(preview, args.out)
    print(f"[render-preview] towns={len(towns)} cities={sum(1 for t in towns if t.get('city'))} "
          f"industries={drawn_industries} rivers={rivers_drawn}")
    print(f"[render-preview] sea_fraction={float((gray == 0).mean()):.3f} "
          f"gray_levels={len(np.unique(gray))}")
    if on_water:
        print(f"[render-preview] ON WATER ({len(on_water)}): {', '.join(on_water)}",
              file=sys.stderr)
    print(f"[render-preview] wrote {args.out} ({args.size}x{args.size})")
    if on_water and not args.allow_town_on_water:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
