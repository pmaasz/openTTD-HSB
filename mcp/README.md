# HSB MCP server — AI builds OpenTTD scenarios

`mcp/server.py` exposes `scripts/` to MCP clients (Model Context Protocol,
stdio transport) so an agent can turn a description ("a 2048 map of Bavaria
with 30 towns…") into a built `.scn`: author data files → heightmap → towns
→ industries/rivers → validate → manifest → headless builder run.

## Install

```sh
python3 -m venv .venv && .venv/bin/pip install -r mcp/requirements.txt
```

## Server configuration

| Variable | Meaning |
|---|---|
| `HSB_WORKDIR` | Filesystem confinement root. Defaults to the server process's current working directory. It must already exist. |
| `HSB_ENGINE` | Fixed patched OpenTTD executable used by `run_builder`. Required only for builder runs; must resolve to an executable regular file. A relative value resolves beneath `HSB_WORKDIR`. |

Every caller-supplied path must resolve beneath `HSB_WORKDIR`. Relative paths
resolve there rather than against the repository or the engine, and paths whose
symlinks escape the workdir are rejected. Helper scripts run with
`HSB_WORKDIR` as their current directory. The engine is server configuration,
not a `run_builder` argument, and may live outside the workdir.

## Client config (stdio)

```json
{
  "mcpServers": {
    "openttd-hsb": {
      "command": "/abs/path/openTTD-HSB/.venv/bin/python",
      "args": ["/abs/path/openTTD-HSB/mcp/server.py"],
      "cwd": "/abs/path/scenario-workdir",
      "env": {
        "HSB_WORKDIR": "/abs/path/scenario-workdir",
        "HSB_ENGINE": "/abs/path/openttd-build/openttd"
      }
    }
  }
}
```

If `HSB_WORKDIR` is omitted, the `cwd` above is the confinement root. The
configured engine must already be patched and built per
`docs/LOCAL-BUILD.md` (30–45 min once). Builder runs also need Xvfb and
`SDL_AUDIODRIVER=dummy` in the server environment.

## Tools

| Tool | Wraps |
|---|---|
| `build_heightmap` | `scripts/build-heightmap.py` (real DEM tiles) |
| `write_cities_csv` | authors `cities.csv` |
| `geocode_towns` | Nominatim lookup → CSV-ready rows; every query must return a row |
| `build_towns` | `scripts/build-towns.py` → `towns.json`; supports `sample_only` and `strict` |
| `write_industries` | authors `industries.json` |
| `write_rivers` | authors `rivers.json` |
| `validate` | `scripts/validate-scenario.py`; supports `scenario`, `strict`, and timeouts |
| `write_manifest` | `scripts/build-manifest.py`; forwards seed, terrain, rotation, search-radius, and timeout controls |
| `run_builder` | runs the fixed `HSB_ENGINE`; stages heightmap and towns automatically |

Resources: `hsb://formats` (file schemas), `hsb://workflow` (runbook).
The agent should read both before starting.

## Builder workflow

`run_builder` has this server-side signature:

```python
run_builder(manifest: str, config: str, timeout: int = 1800)
```

1. Put the builder config beneath `HSB_WORKDIR`; its parent is the personal
   directory.
2. Write the manifest with `write_manifest`. Its `save` value is a filename,
   not a path.
3. Call `run_builder(manifest, config)`. The server verifies that the manifest,
   config, and manifest-referenced source files are regular files beneath the
   workdir before executing anything.
4. The manifest's heightmap and towns are copied by basename through temporary
   files into `<config-parent>/scenario/heightmap` and atomically replaced there.
   Manifest industries and optional rivers stay at their confined source paths.
5. The server runs `HSB_BUILD=<manifest> <HSB_ENGINE> -e -c <config>` from
   `HSB_WORKDIR`. A successful engine exit must also report `hsb: saved`.

After building, pass the produced scenario path to `validate(scenario=...)`;
use `strict=True` when generated artifacts must exist.

## Smoke test (no network or real engine)

```sh
.venv/bin/python mcp/test_smoke.py
```

The test uses a temporary workdir and a fake executable. It covers data
authoring, path and symlink confinement, helper cwd, seed/control forwarding,
geocode failure handling, atomic builder staging, engine validation, MCP schema
validation, and tool registration/dispatch.
