#!/usr/bin/env python3
"""Build a heightmap PNG from public elevation tiles (Terrarium).

Pipeline: Terrarium elevation tiles (AWS elevation-tiles-prod, derived from
SRTM/ASTER-class DEMs) -> mosaic -> crop to --bounds -> normalize to 8-bit
grayscale with sea=0 -> resize to OpenTTD power-of-2 size.

Deterministic: tiles fetched in sorted order into a local cache; PNG output
depends only on cached tiles + fixed constants.

Bounds example (Germany): --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5

Generalized from GermanyTransportTycoonDeluxe scripts/build-heightmap.py.
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

# Normalization: elevations clipped to [0, E_CLIP] then gamma-curved to 1..255.
# Sea (elev <= 0) is always gray 0. Gamma < 1 lifts mid-hills while
# compressing high peaks.
E_CLIP = 2500.0
GAMMA = 0.5


def lon_to_xtile(lon, z):
    return math.floor((lon + 180.0) / 360.0 * (1 << z))


def lat_to_ytile(lat, z):
    rad = math.radians(lat)
    return math.floor(
        (1.0 - math.log(math.tan(rad) + 1.0 / math.cos(rad)) / math.pi)
        / 2.0
        * (1 << z)
    )


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


def normalize(elev, e_clip=E_CLIP, gamma=GAMMA):
    gray = np.zeros_like(elev, dtype=np.uint8)
    land = elev > 0
    t = np.clip(elev[land] / e_clip, 0.0, 1.0) ** gamma
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
    p.add_argument("--check-only", action="store_true", help="verify toolchain/connectivity, write nothing")
    args = p.parse_args(argv)

    lat_min, lat_max, lon_min, lon_max = args.lat_min, args.lat_max, args.lon_min, args.lon_max
    if not (lat_min < lat_max and lon_min < lon_max):
        print("error: need lat_min < lat_max and lon_min < lon_max", file=sys.stderr)
        return 2

    if args.size & (args.size - 1) or args.size < 64:
        print(f"error: --size must be a power of 2 >= 64, got {args.size}", file=sys.stderr)
        return 2

    if args.check_only:
        print(f"[build-heightmap] bounds={lat_min}-{lat_max}N {lon_min}-{lon_max}E "
              f"e_clip={args.e_clip} gamma={args.gamma}")
        print("[build-heightmap] toolchain OK (urllib+numpy+PIL)")
        return 0

    z = args.zoom
    x0, x1 = lon_to_xtile(lon_min, z), lon_to_xtile(lon_max, z)
    y0, y1 = lat_to_ytile(lat_max, z), lat_to_ytile(lat_min, z)
    nx, ny = x1 - x0 + 1, y1 - y0 + 1
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
    print(f"[build-heightmap] cropped {cropped.shape[1]}x{cropped.shape[0]} px")

    if cropped.max() <= 0:
        print("error: cropped area is all sea — bounds or tiles wrong", file=sys.stderr)
        return 1

    gray = normalize(cropped, e_clip=args.e_clip, gamma=args.gamma)
    sea_frac = float((gray == 0).mean())
    print(f"[build-heightmap] sea_fraction={sea_frac:.3f} "
          f"gray_levels={len(np.unique(gray))}")

    img = Image.fromarray(gray, mode="L").resize((args.size, args.size), Image.LANCZOS)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    img.save(args.out)
    print(f"[build-heightmap] wrote {args.out} ({args.size}x{args.size} L)")

    master = Image.fromarray(cropped.astype(np.float32))
    os.makedirs(os.path.dirname(os.path.abspath(args.master_out)), exist_ok=True)
    master.save(args.master_out)
    print(f"[build-heightmap] wrote {args.master_out} (float32 master)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
