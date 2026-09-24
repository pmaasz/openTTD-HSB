# HSB MCP server — AI builds OpenTTD scenarios

`mcp/server.py` exposes `scripts/` to MCP clients (Model Context Protocol,
stdio transport) so an agent can turn a description ("a 2048 map of Bavaria
with 30 towns…") into a built `.scn`: author data files → heightmap → towns
→ industries/rivers → validate → manifest → headless builder run.

## Install

```sh
python3 -m venv .venv && .venv/bin/pip install -r mcp/requirements.txt
```

## Client config (stdio)

```json
{
  "mcpServers": {
    "openttd-hsb": {
      "command": "/abs/path/openTTD-HSB/.venv/bin/python",
      "args": ["/abs/path/openTTD-HSB/mcp/server.py"],
      "cwd": "/abs/path/scenario-workdir"
    }
  }
}
```

Relative file paths resolve against `cwd` — point it at a workdir, not the
repo. The engine (`run_builder`) is NOT built here; it needs a patched +
compiled OpenTTD per `docs/LOCAL-BUILD.md` (30–45 min once), plus Xvfb and
`SDL_AUDIODRIVER=dummy` in the server environment.

## Tools

| Tool | Wraps |
|---|---|
| `build_heightmap` | `scripts/build-heightmap.py` (real DEM tiles) |
| `write_cities_csv` | authors `cities.csv` |
| `geocode_towns` | Nominatim lookup → CSV-ready rows |
| `build_towns` | `scripts/build-towns.py` → `towns.json` |
| `write_industries` | authors `industries.json` |
| `write_rivers` | authors `rivers.json` |
| `validate` | `scripts/validate-scenario.py` (fix failures first) |
| `write_manifest` | `scripts/build-manifest.py` (embeds bounds) |
| `run_builder` | `HSB_BUILD=… openttd -e -c …` (slow, needs engine) |

Resources: `hsb://formats` (file schemas), `hsb://workflow` (runbook).
The agent should read both before starting.

## Smoke test (no engine, no network)

```sh
.venv/bin/python mcp/test_smoke.py
```

Covers data authoring → towns → manifest → validate on a synthetic 64×64
map, plus MCP tool registration/dispatch.
