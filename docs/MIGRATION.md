# Migration — GTTD builder → HSB

This project generalizes `GermanyTransportTycoonDeluxe` patch 06
(`GTTD headless scenario builder`).

| GTTD (origin) | HSB (this repo) |
|---|---|
| `src/gttd_builder.{h,cpp}` | `builder/hsb_builder.{h,cpp}` (+ `patches/01-...diff`) |
| `GTTD_BUILD=<manifest>` | `HSB_BUILD=<manifest>` (`GTTD_BUILD` still works, deprecated) |
| `GttdBuildRequested/Scenario`, `Gttd*` | `HsbBuildRequested/Scenario`, `Hsb*` |
| `gttd:` log prefix | `hsb:` log prefix |
| hardcoded bounds 47–55.5N, 4–15.5E | `bounds` object in manifest (required) |
| `heightmap_rotation` fixed clockwise | `heightmap_rotation` in manifest (`clockwise` default) |
| industry spiral radius fixed 32 | `industry_search_radius` in manifest (default 32) |
| `scripts/build-manifest.py --size/--save/...` | same script with `--heightmap/--towns/--industries/--rivers/--bounds/...` |
| `scripts/build-heightmap.py` (Germany bounds, island checks) | `--lat-min/max --lon-min/max`, no hardcoded islands |
| `scripts/build-towns.py` (Germany bounds, `, Deutschland` geocode) | `--lat-min/max --lon-min/max`, `--geocode-suffix` |
| `scripts/validate-scenario.py` (GIST list, pinned-versions, rivers required) | `--bounds`, `--industry-types/--water-types`, `--cities/--rivers` optional |

## Migrating a GTTD manifest

Old:

```json
{"heightmap": "...", "towns": "...", "industries": "...",
 "rivers": "...", "start_year": 2025, "save": "germany.scn"}
```

New — add `bounds` (and optionally rename env var):

```json
{"heightmap": "...", "towns": "...", "industries": "...",
 "rivers": "...",
 "bounds": {"lat_min": 47.0, "lat_max": 55.5,
            "lon_min": 4.0, "lon_max": 15.5},
 "start_year": 2025, "save": "germany.scn"}
```

```sh
# easiest: regenerate instead of hand-editing
python3 scripts/build-manifest.py \
  --heightmap <png> --towns towns.json --industries industries.json --rivers rivers.json \
  --lat-min 47.0 --lat-max 55.5 --lon-min 4.0 --lon-max 15.5 \
  --start-year 2025 --save germany.scn --out /tmp/build.json
HSB_BUILD=/tmp/build.json ./build/upstream/openttd -e -c <personal-dir>/hsb-builder.cfg
```
