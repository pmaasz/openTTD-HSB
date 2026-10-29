# Changelog

## Unreleased

### Changed — OpenTTD 16.0-beta5

- Pinned upstream to `16.0-beta5` (`130bf62a`). The builder moved to the 16.0
  APIs: scoped `TileType`, `Axis`, `DiagDirection`, `Track`, `TransportType`,
  `RoadTramType`, `LevelMode`, `Commands`, `SaveLoadOperation`, `Subdirectory`,
  `GameMode`, `SwitchMode` and `Currency` enums, plus the changed
  `BuildBridge`, `BuildTunnel`, `TerraformLand` and `RemoveRail` command
  signatures.
- Dropped the counter-clockwise town mapping workaround: 16.0 fixes
  `TranslateCoordinates` upstream, so the patch has no `genworld.cpp` edit left.

### Added — OSM-derived data

- **`scripts/osm-reference.py`** extracts roads, railways and stations from
  Geofabrik state extracts (or a local `.osm.pbf`) into the shared reference
  format, using the same linear latitude mapping and transposed axes as
  `HsbTileForBounds()`.
- **`scripts/build-osm-network.py`** turns that reference into
  `autobahn-osm.json`, `rail-osm.json` and `stations-osm.json`, so real
  motorways, main lines and station positions can feed the optional network
  phase.
- **`--osm` / `--stations`** in `scripts/build-manifest.py` and
  `scripts/validate-scenario.py` (default `assets/geodata`), plus the optional
  `stations` manifest key. A station entry moves the rail search origin to the
  real OSM position; the nearest track within 14 tiles is still required, and a
  town without an entry keeps the old town-centre behaviour.
- **`tests/test_osm_network.py`** covers projection, network derivation,
  `--check`, manifest wiring and validation; CI runs it with the reference
  self-test.
- **`docs/OSM.md`** and **`docs/ATTRIBUTION.md`** document the pipeline and the
  ODbL attribution the derived files need.

### Added — the network phase

The builder can now build a transport network, not just terrain and towns. Every
sub-phase is opt-in per input file, so a manifest that names none builds exactly
what it built before: same save, same audit line, no extra keys written.

- **A road network.** Trunk corridors (`autobahn.json`), a town-to-town network
  (`roads.json`), a spur to every land industry, then a retry sweep, dead-end
  pruning, town-to-trunk joins and fragment stitching. Corridors are cut into
  straight pieces of at most `HSB_TUNE_ROAD_RUN` tiles per axis with one
  right-angle corner per piece — the geometry the player's autoroad tool
  produces, and the reason a diagonal run does not double in length and take the
  whole river, cliff band and town belt with it.
- **A rail network** (`rail.json`): main and branch lines, a station where a
  line meets a town, and a siding with a depot at every bulk industry worth rail.
- **Airports and harbours** (`airports.json`, `harbours.json`) at their real
  positions. The list is data rather than a rule about which cities deserve one:
  the earlier "most isolated big towns" heuristic put runways where no airport
  has ever been.
- **A fleet that moves.** A depot, two stops and a bus for the largest towns; a
  spur depot and a train for the biggest stations; real orders on both; and the
  start command pressed, because a vehicle with a perfect route that is still
  parked in its shed is a museum.
- **A start view.** The builder picks the town that best shows the network and
  writes it into the `.scn` as data, so the game opens on a station instead of
  the empty map corner — and it does so on a **stock, unpatched** engine, which
  reads the same saved fields.
- **`builder/hsb_builder_net.h`**, the pure half of the network phase: corner
  candidates, obstacle weights, chunk geometry, the tunable parser, the
  dump's character ranking and the manifest range table. No engine types, so all
  of it is unit tested; the C++ in `hsb_builder.cpp` is the thin layer that asks
  the engine what a tile is.
- **Router tunables from the environment** (`HSB_TUNE_ROAD_RUN`,
  `HSB_TUNE_ROAD_SNAP`, `HSB_TUNE_CORNER_FAN`, `HSB_TUNE_ROAD_TRACE`) plus
  `HSB_DUMP_NETWORK` and `HSB_FAST`. Defaults are the previously compiled-in
  values, so an empty environment routes what it always routed. The first line
  of every build names the configuration, because a log that does not say what
  produced it cannot be compared with another log.
- **A network audit line** (`hsb: network audit:`), and `run_builder`'s
  `network_*` fields. It answers three *different* questions because they were
  three different answers to "is the network finished?": component counts (is
  the trunk in pieces?), `towns_on_network` (is any road near each town on the
  main component?), and the intercity line (can a bus actually be driven there?).
  Only the last one is the question that matters, and the first two pass for a
  trunk that stops two tiles short of the kerb.
- **Pre-save gates.** Exactly one company, at least two company slots, at least
  one routed and running service, every designed industry still alive, and a
  start view chosen. Each is a scenario that looks finished and is not, and none
  is visible from the exit code.
- **Manifest keys** `roads`, `autobahnen`, `rail`, `airports`, `harbours`,
  `with_rail`, `with_fleet`, `build_on_slopes`, `autoslope`, `network_money`,
  `bus_towns`, `trains` — all validated before a byte of map is generated, with
  `scripts/build-manifest.py` and the MCP `write_manifest` tool refusing the
  same rules.
- **New upstream patches**: `HasRailTypeAvail` accepts the editor and worldgen
  company cases (without it the first rail tile of a scenario segfaults), and
  the start-view field in the saveload.

### Fixed

All of these were found by running the ported network phase end to end on a
256-tile map, not by reading the code.

- **`GetString()` on a bare `CMD_ERROR` took the process down.** A command that
  fails a plain precondition returns a `CommandCost` whose StringID is
  `INVALID_STRING_ID`, and `GetString` asserts on that. A build that merely
  could not place one industry died with a crash log instead of logging which
  one — and the industry placement loop called `GetString(res.GetErrorMessage())`
  on every failed spiral step. All four call sites go through `HsbWhy()` now,
  which answers "no reason given".
- **The refusal tallies were blind to the same thing.** `HsbNoteRoadFailure`
  returned early on an invalid StringID, so the most common possible refusal
  contributed nothing to the count. That is how "the road command refuses every
  tile" produced a log with nothing in it.
- **No road could ever be built.** `CmdBuildRoad` opens with `if
  ((Company::IsValidID(company) && town_id != TownID::Invalid()) || ...) return
  CMD_ERROR;` — a valid company may not name a town. The builder owns its
  network through a borrowed company and was passing a `TownID` on every tile.
  Ownership is now the separate step it was always documented to be
  (`HsbOwnRoadLeg`, the same two writes `CmdBuildRoad` makes), which is also why
  the ported code compiled at all: it only worked while `_current_company` was
  still `OWNER_NONE`.
- **The whole network phase ran with no company.** `AutoRestoreBackup
  backup_cur_company(_current_company, OWNER_NONE)` and two siblings are
  function-wide in the engine's other callers; here they restored at the end of
  `HsbBuildScenario`, so `_current_company` was `OWNER_NONE` and
  `_generating_world` was still true for every road, station, depot and vehicle
  below. The engine answered by creating a second company to own the fleet, so
  the save held a network nobody owned plus a company the player was handed.
  Scoped to the placement loop.
- **The construction settings never reached the game.** `build_on_slopes`,
  `autoslope`, `crossing_with_competitor` and `road_stop_on_town_road` were
  assigned to `_settings_newgame` *after* `GenerateWorld`, which is the only
  thing that copies them into `_settings_game`. A silent no-op, with 270 "Site
  unsuitable for tunnel entrance" refusals as the symptom and no error anywhere.
  They are set before world generation now, and the build logs the *live*
  settings rather than the newgame ones.
- **Handing the trunk to the towns.** Giving a whole 100 km corridor to the
  nearest town looked like generosity and made the entire network town road, so
  the railway met the company's own track with "must remove road first" 31 times
  and laid nothing. Ownership is now limited to the tiles inside the town's own
  claim: the approach is the town's (which is what makes the town grow along the
  road that reaches it) and the trunk stays the company's (which is what lets
  the rail cross it).
- **A refused build crashed on the way out.** `exit(1)` from `GameLoop` runs on
  the video thread, so the process unwound through `atexit` handlers with other
  threads still live: "pure virtual method called", then a crash log, on top of
  the real error. The hook uses `_exit(1)` with an explicit flush, which is what
  the engine's own `UserErrorI` does.
- **`gen-patch.sh` could not run in its own workflow.** After
  `apply-patches.sh`, the submodule has the previous patch applied and the four
  `hsb_builder*` files untracked, and the script refused that as a dirty
  worktree. It now reverses its own patch first. The alternative — telling people
  to `git checkout --` inside the submodule — silently yields a 0-byte source on
  an intent-to-add file, which only fails at link time.

Correctness fixes and measurement, ported from the sibling
`germanyTransportTycoonDeluxe` builder, which found all of these the hard way.

### Fixed (earlier in this release)

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

### Added (earlier in this release)

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
  `write_manifest(snow_line/max_loan/competitors/currency)`, and the network
  inputs on `write_manifest` and `validate`.

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
