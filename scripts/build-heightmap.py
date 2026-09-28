#!/usr/bin/env python3
"""Build a heightmap PNG from public elevation tiles (Terrarium).

Pipeline: Terrarium elevation tiles (AWS elevation-tiles-prod, derived from
SRTM/ASTER-class DEMs) -> mosaic -> bilinear resample to a linear geographic
raster over --bounds -> normalize to 8-bit grayscale with sea=0 at the requested
OpenTTD power-of-2 size.

Deterministic: tiles fetched in sorted order into a local cache; PNG output
depends only on cached tiles + fixed constants.

Bounds example (Germany): --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5

Derived from the Terrarium elevation-tile pipeline.
This file is part of openTTD-HSB (GPLv2, see LICENSE.md).
"""
import argparse
import math
import os
import sys
import time
import urllib.request

import numpy as np
from PIL import Image

TILE_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
TILE_SIZE = 256
MAX_MOSAIC_PIXELS = 16 * 1024 * 1024

# Normalization: elevations clipped to [0, E_CLIP] then gamma-curved to 1..255.
# Sea (elev <= 0) is always gray 0. Gamma < 1 lifts mid-hills while
# compressing high peaks.
E_CLIP = 2500.0
GAMMA = 0.5


def lat_to_mercator_y(lat):
    rad = math.radians(lat)
    return (
        1.0 - math.log(math.tan(rad) + 1.0 / math.cos(rad)) / math.pi
    ) / 2.0


def lon_to_xtile(lon, z):
    return math.floor((lon + 180.0) / 360.0 * (1 << z))


def lat_to_ytile(lat, z):
    return math.floor(lat_to_mercator_y(lat) * (1 << z))


def xtile_to_lon(x, z):
    return x / (1 << z) * 360.0 - 180.0


def ytile_to_lat(y, z):
    n = math.pi - 2.0 * math.pi * y / (1 << z)
    return math.degrees(math.atan(math.sinh(n)))


def fetch_tile(z, x, y, cache_dir, retries=4):
    path = os.path.join(cache_dir, str(z), str(x), f"{y}.png")
    if os.path.exists(path):
        return path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    url = TILE_URL.format(z=z, x=x, y=y)
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "HSB-heightmap/1.0"})
            with urllib.request.urlopen(req, timeout=60) as resp, open(path, "wb") as f:
                f.write(resp.read())
            return path
        except Exception as e:  # noqa: BLE001 - retry transient network errors
            last = e
            time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"failed to fetch tile z={z} x={x} y={y}: {last}")


def decode_terrarium(path):
    img = np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)
    return img[:, :, 0] * 256.0 + img[:, :, 1] + img[:, :, 2] / 256.0 - 32768.0


def bilinear_resample(source, output_height, output_width, source_rows, source_cols):
    source_height, source_width = source.shape
    source_rows = np.asarray(source_rows, dtype=np.float64)
    source_cols = np.asarray(source_cols, dtype=np.float64)
    if source_rows.shape != (output_height,) or source_cols.shape != (output_width,):
        raise ValueError("source coordinate arrays must match output dimensions")

    source_cols = np.clip(source_cols, 0.0, source_width - 1.0)
    col0 = np.floor(source_cols).astype(np.intp)
    col1 = np.minimum(col0 + 1, source_width - 1)
    col_weights = source_cols - col0
    col_weights_remaining = 1.0 - col_weights
    result = np.empty((output_height, output_width), dtype=np.float64)

    for output_row, source_y in enumerate(source_rows):
        source_y = float(np.clip(source_y, 0.0, source_height - 1.0))
        row0 = min(max(math.floor(source_y), 0), source_height - 1)
        row1 = min(row0 + 1, source_height - 1)
        row_weight = source_y - row0
        top = (
            source[row0, col0] * col_weights_remaining
            + source[row0, col1] * col_weights
        )
        bottom = (
            source[row1, col0] * col_weights_remaining
            + source[row1, col1] * col_weights
        )
        result[output_row] = top + (bottom - top) * row_weight

    return result


def linear_rows_to_mercator_rows(latitudes, z, y0, y1):
    latitudes = np.asarray(latitudes, dtype=np.float64)
    target_mercator_rows = np.fromiter(
        (lat_to_mercator_y(lat) for lat in latitudes),
        dtype=np.float64,
        count=latitudes.size,
    )
    mercator_top = y0 / (1 << z)
    mercator_bottom = (y1 + 1) / (1 << z)
    return ((mercator_top - target_mercator_rows)
            / (mercator_top - mercator_bottom))


def normalize(elev, e_clip=E_CLIP, gamma=GAMMA, sea_level=0.0):
    """Map elevations to 8-bit grey: sea exactly 0, land 1..255.

    `sea_level` is the elevation (metres) below which a pixel counts as water.
    SRTM and other DEMs do not put the coastline at exactly 0 m: large tidal
    flats and reclaimed land are recorded as land, which makes shallow lagoons
    and polders render as dry ground the engine then refuses to build on. A
    slightly negative sea level removes them; the master TIFF keeps the raw
    elevations so nothing is lost.
    """
    gray = np.zeros_like(elev, dtype=np.uint8)
    land = elev > sea_level
    span = e_clip - sea_level
    t = np.clip((elev[land] - sea_level) / span, 0.0, 1.0) ** gamma
    gray[land] = (1 + (254.0 * t)).astype(np.uint8)
    return gray


def main(argv=None):
    p = argparse.ArgumentParser(description="Build heightmap PNG for HSB")
    p.add_argument("--lat-min", type=float, required=True)
    p.add_argument("--lat-max", type=float, required=True)
    p.add_argument("--lon-min", type=float, required=True)
    p.add_argument("--lon-max", type=float, required=True)
    p.add_argument("--zoom", type=int, default=8, help="terrain-tile zoom (8=preview, 9=full)")
    p.add_argument("--size", type=int, default=2048, help="output pixels (power of 2)")
    p.add_argument("--out", default="heightmap.png")
    p.add_argument("--master-out", default="heightmap-master.tif")
    p.add_argument("--cache-dir", default=os.path.expanduser("~/.cache/hsb/terrarium"))
    p.add_argument("--e-clip", type=float, default=E_CLIP)
    p.add_argument("--gamma", type=float, default=GAMMA)
    p.add_argument("--sea-level", type=float, default=0.0,
                   help="elevation in metres below which a pixel is water (use a "
                        "negative value to drop SRTM tidal flats and polders)")
    p.add_argument("--check-only", action="store_true", help="verify arguments and toolchain, write nothing")
    args = p.parse_args(argv)

    lat_min, lat_max, lon_min, lon_max = args.lat_min, args.lat_max, args.lon_min, args.lon_max
    bounds = (lat_min, lat_max, lon_min, lon_max)
    if (not all(math.isfinite(value) for value in bounds)
            or not (-90.0 < lat_min < lat_max < 90.0)
            or not (-180.0 < lon_min < lon_max < 180.0)):
        print("error: bounds must be finite WGS84 intervals inside (-90,90) and (-180,180)", file=sys.stderr)
        return 2

    if args.size & (args.size - 1) or not 64 <= args.size <= 4096:
        print(f"error: --size must be a power of 2 in 64..4096, got {args.size}", file=sys.stderr)
        return 2
    if not math.isfinite(args.e_clip) or args.e_clip <= 0:
        print("error: --e-clip must be finite and positive", file=sys.stderr)
        return 2
    if not math.isfinite(args.gamma) or args.gamma <= 0:
        print("error: --gamma must be finite and positive", file=sys.stderr)
        return 2
    if not math.isfinite(args.sea_level):
        print("error: --sea-level must be finite", file=sys.stderr)
        return 2
    if args.sea_level >= args.e_clip:
        print(f"error: --sea-level {args.sea_level} must be below --e-clip {args.e_clip}", file=sys.stderr)
        return 2
    if not 0 <= args.zoom <= 22:
        print(f"error: --zoom must be in 0..22, got {args.zoom}", file=sys.stderr)
        return 2

    if args.check_only:
        print(f"[build-heightmap] bounds={lat_min}-{lat_max}N {lon_min}-{lon_max}E "
              f"e_clip={args.e_clip} gamma={args.gamma} sea_level={args.sea_level}")
        print("[build-heightmap] toolchain OK (urllib+numpy+PIL)")
        return 0

    if os.path.abspath(args.out) == os.path.abspath(args.master_out):
        print("error: --out and --master-out must be different files", file=sys.stderr)
        return 2

    z = args.zoom
    x0, x1 = lon_to_xtile(lon_min, z), lon_to_xtile(lon_max, z)
    y0, y1 = lat_to_ytile(lat_max, z), lat_to_ytile(lat_min, z)
    nx, ny = x1 - x0 + 1, y1 - y0 + 1
    if nx <= 0 or ny <= 0 or nx * ny > 4096:
        print(f"error: requested mosaic needs {nx}x{ny} tiles; maximum is 4096", file=sys.stderr)
        return 2
    if nx * ny * TILE_SIZE * TILE_SIZE > MAX_MOSAIC_PIXELS:
        print(f"error: requested mosaic exceeds {MAX_MOSAIC_PIXELS} source pixels", file=sys.stderr)
        return 2
    print(f"[build-heightmap] zoom={z} tiles x={x0}..{x1} y={y0}..{y1} ({nx}x{ny} tiles)")

    mosaic = np.zeros((ny * TILE_SIZE, nx * TILE_SIZE), dtype=np.float64)
    for j in range(ny):
        for i in range(nx):
            path = fetch_tile(z, x0 + i, y0 + j, args.cache_dir)
            mosaic[j * TILE_SIZE:(j + 1) * TILE_SIZE, i * TILE_SIZE:(i + 1) * TILE_SIZE] = \
                decode_terrarium(path)
    print(f"[build-heightmap] elevation min={mosaic.min():.1f} max={mosaic.max():.1f} "
          f"mean={mosaic.mean():.1f} m")

    # Crop mosaic to exact lon/lat bounds (web-mercator pixel space).
    lon_left = xtile_to_lon(x0, z)
    lon_right = xtile_to_lon(x1 + 1, z)
    lat_top = ytile_to_lat(y0, z)
    lat_bottom = ytile_to_lat(y1 + 1, z)

    def frac_lon(lon):
        return (lon - lon_left) / (lon_right - lon_left)

    def frac_lat(lat):
        return (lat_top - lat) / (lat_top - lat_bottom)

    h, w = mosaic.shape
    cx0, cx1 = int(frac_lon(lon_min) * w), int(frac_lon(lon_max) * w)
    cy0, cy1 = int(frac_lat(lat_max) * h), int(frac_lat(lat_min) * h)
    cropped = mosaic[cy0:cy1, cx0:cx1]
    if cropped.size == 0:
        print("error: bounds resolve to fewer than one source pixel", file=sys.stderr)
        return 2
    print(f"[build-heightmap] cropped {cropped.shape[1]}x{cropped.shape[0]} px")

    if cropped.max() <= 0:
        print("error: cropped area is all sea — bounds or tiles wrong", file=sys.stderr)
        return 1

    output_fractions = np.arange(args.size, dtype=np.float64) / (args.size - 1)
    target_lons = lon_min + output_fractions * (lon_max - lon_min)
    source_cols = (target_lons - lon_left) / (lon_right - lon_left) * (w - 1)
    target_lats = lat_max - output_fractions * (lat_max - lat_min)
    source_rows = linear_rows_to_mercator_rows(
        target_lats, z, y0, y1) * (h - 1)
    linear = bilinear_resample(mosaic, args.size, args.size, source_rows, source_cols)

    gray = normalize(linear, e_clip=args.e_clip, gamma=args.gamma, sea_level=args.sea_level)
    sea_frac = float((gray == 0).mean())
    print(f"[build-heightmap] sea_fraction={sea_frac:.3f} "
          f"gray_levels={len(np.unique(gray))}")

    img = Image.fromarray(gray, mode="L")
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    img.save(args.out, format="PNG")
    print(f"[build-heightmap] wrote {args.out} ({args.size}x{args.size} L)")

    master = Image.fromarray(linear.astype(np.float32))
    os.makedirs(os.path.dirname(os.path.abspath(args.master_out)), exist_ok=True)
    master.save(args.master_out, format="TIFF")
    print(f"[build-heightmap] wrote {args.master_out} (float32 master)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
