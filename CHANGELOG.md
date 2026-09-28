# Changelog

## Unreleased

Correctness fixes and measurement, ported from the sibling
`germanyTransportTycoonDeluxe` builder, which found all of these the hard way.

### Fixed

- **River mouths were severed by a one-tile `Coast` water gap.** Every tile along
  the generated shoreline carries `WaterTileType::Coast`, and `IsWaterTile()`
  is false for it. The carve guard tested `IsTileType(MP_WATER)`, so it skipped
  those tiles, while the connectivity masks use `IsWaterTile` and so refused to
  flood through them: a tile that is simultaneously "water, nothing to paint"
  and "not water, cannot traverse". The shoreline is exactly where a river is
  supposed to end, so this cut every river mouth in the map off from the sea
  without any error. The carve guard and the masks now agree on one predicate.
  Found by the new audit line, not by inspection.
- **NewGRF persistent storage no longer reverts to zero on save.** Content
  creation (town import, industry placement) is now bracketed in an RAII
  gameloop scope, mirroring `GenerateWorld` and the editor's own industry
  generator. `Command::Do()` does not enter command scope itself — it expects
  the ambient gameloop — and the editor runs outside both, so every
  industry/town register write stayed "temporary". Invisible to the build log:
  the run reported every industry placed while the save carried dead
  production for any register-driven industry set.
- **The start year is now set before content, not after.** Industries and
  closures track unserved time from creation, so building in year 1 and only
  then jumping to the scenario start year made everything look decades stale
  and triggered rapid closures on the first frame a player opened the save.
- **Rivers are carved 4-connected.** A straight stamp between two waypoints
  left tiles touching only at their corners wherever the river ran diagonally:
  a chain of ponds that looks connected on the map and that no ship can enter.
  Segments are now walked as a staircase (`HsbStaircaseWalk`, unit-tested).
- **Carved rivers are connected to open water.** Every body that does not touch
  open water within 96 tiles gets a 4-connected channel cut to the nearest
  water a ship could already use. The DEM coastline often sits a tile or two
  inland, so an authored river that "reaches the sea" could come out a closed
  pond. The step reports `connected` only when the channel walk actually
  arrived, and says so when it cannot.
- **`--sea-level` for the heightmap build.** SRTM and similar DEMs do not put
  the coastline at exactly 0 m; tidal flats and polders render as dry ground
  the engine then refuses to build on. `docs/SCENARIO.md` used to call this a
  known quirk with no knob.
- **River `width: 1` carves one tile per axis again.** A `width <= 2` brush
  test made the narrowest rivers paint a 2×2 square, quadrupling them and
  making the carved count disagree with the documented width.

### Added

- **The build measures itself.** A successful run ends with one machine-readable
  `hsb: audit:` line: `towns`, `towns_on_water`, `industries`, `water`,
  `sea_fraction`, `water_bodies`, `isolated_water_tiles`, `water_unreachable`,
  `carved`, `carved_reachable`. `mcp/` parses it into `run_builder`'s `audit`
  field, so a gate can assert `towns_on_water == 0` and
  `carved_reachable == carved` instead of trusting the exit code.
- **`scripts/render-preview.py`**, a deterministic terrain + rivers + towns +
  industries PNG that doubles as a gate: it exits 1 and names the towns when an
  anchor lands on water. Replaces eyeballing a grey raster. Bounds are
  arguments, so it previews any region.
- **Scenario settings are pinned instead of inherited.** A scenario's newgame
  settings win over the player's config, so anything the manifest does not set
  was coming from the builder's own `openttd.cfg` — machine state, not intent.
  `max_loan`, `competitors`, `currency` and `snow_line_height` are new manifest
  fields; `competitors` is always written (0 means no company slots, which
  drops the player onto the map as a spectator), and an unknown currency code
  is a build error rather than a silent fallback. `order.selectgoods` is forced
  off (on = a permanent delivery deadlock for new full-load routes).
- **Catch2 tests** for the staircase walk (connectivity, monotonicity,
  degenerate cases) and for out-of-range scenario settings.
- `mcp/server.py`: `render_preview` tool, `build_heightmap(sea_level=…)`,
  `write_manifest(snow_line/max_loan/competitors/currency)`.

## v0.1.0 (release candidate)

The first reproducible release of the OpenTTD Headless Scenario Builder (HSB).
It pins OpenTTD 15.3 and turns the Scenario Editor workflow into a
manifest-driven build:

`heightmap → rivers/estuaries → towns → industries → start year → save → exit`

### Highlights

- Adds the pinned OpenTTD submodule and a generated, parity-checked builder patch.
- Builds linear geographic heightmap rasters from Terrarium elevation tiles.
- Standardizes town, industry, and river coordinates across Python and C++.
- Validates manifests, WGS84 bounds, map dimensions, schemas, paths, seeds, and
  save filenames before the engine starts.
- Carves navigable water, supports both rotations, verifies complete town
  imports, and tries industry anchors before spiral fallback.
- Adds a nine-tool MCP server with workdir confinement, fixed server-side engine
  configuration, atomic input staging, and reliable geocoding status.
- Adds Catch2/CTest builder tests, an incremental local test runner, ccache
  support, and a cached builder-test CI job.

### Requirements

- OpenTTD 15.3 at the commit recorded in `openttd-upstream.PIN`.
- Python 3.12 with `numpy`, `pillow`, and `mcp` for the MCP server.
- A patched and built OpenTTD binary for scenario generation.
- Xvfb plus `SDL_AUDIODRIVER=dummy` for headless engine runs.
- Network access for Terrarium tiles and Nominatim geocoding.

### Compatibility notes

- Manifests require a `bounds` object and absolute input paths.
- Heightmaps must be square grayscale PNGs in the 64–4096 pixel range.
- `save` is a filename, not a path; it is written below the engine scenario
  directory.
- MCP `run_builder` takes `manifest` and `config`; the executable is configured
  with `HSB_ENGINE`, and filesystem inputs are confined by `HSB_WORKDIR`.
- The MCP server exposes 9 tools in v0.1.0 and 10 from Unreleased on
  (`render_preview`); other tool signatures only gained optional parameters.
- This is the first release, so there is no prior-version migration path.

### Verification

The following local checks pass:

```sh
sh scripts/fetch-upstream.sh
sh scripts/gen-patch.sh --check
sh scripts/apply-patches.sh --check-only
python3 -m compileall scripts builder mcp
.venv/bin/python mcp/test_smoke.py
HSB_TEST_FAST=1 sh scripts/test-builder.sh
```

Run the complete builder suite with:

```sh
sh scripts/test-builder.sh
```

The complete `openttd_test` link requires a full engine build and may take
tens of minutes on modest hardware. The CI `builder-tests` job runs the same
Catch2/CTest suite with ccache enabled. GitHub-hosted minutes are account
dependent, so the local runner or a self-hosted runner is a valid fallback.

### Documentation

- `README.md` — quick start
- `docs/FORMATS.md` — manifest and input schemas
- `docs/SCENARIO.md` — scenario build runbook
- `docs/LOCAL-BUILD.md` — engine build, tests, and incremental workflow
- `mcp/README.md` — MCP configuration and tools
