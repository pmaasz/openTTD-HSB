# Changelog

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

## Unreleased

Changes after v0.1.0 will be collected here.
