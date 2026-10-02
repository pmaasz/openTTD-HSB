#ifndef HSB_BUILDER_H
#define HSB_BUILDER_H

#include "tile_type.h"

/**
 * HSB headless scenario builder.
 *
 * Driven by the HSB_BUILD environment variable pointing at a JSON manifest:
 * {
 *   "heightmap":  "/abs/path/map-4096.png",
 *   "towns":      "/abs/path/towns.json",
 *   "industries": "/abs/path/industries.json",
 *   "rivers":     "/abs/path/rivers.json",   // optional, omit or null to skip
 *   "roads":      "/abs/path/roads.json",    // optional, see "Network phase"
 *   "autobahnen": "/abs/path/autobahn.json", // optional
 *   "rail":       "/abs/path/rail.json",     // optional
 *   "airports":   "/abs/path/airports.json", // optional
 *   "harbours":   "/abs/path/harbours.json", // optional
 *   "with_rail":  true,                      // optional, default true
 *   "with_fleet": true,                      // optional, default true
 *   "bounds":     {"lat_min": 47.0, "lat_max": 55.5,
 *                  "lon_min": 4.0,  "lon_max": 15.5},
 *   "start_year": 2025,
 *   "heightmap_height": 12,                  // optional, default 12
 *   "heightmap_rotation": "clockwise",       // optional: clockwise (default)
 *                                            // or counter-clockwise
 *   "snow_line_height": 10,                  // optional 0..255, 0 = derive from snow_coverage
 *   "seed": 20250924,                        // optional, fixed generation seed (not 0xffffffff)
 *   "industry_search_radius": 32,            // optional 1..512 spiral fallback tiles
 *   "max_loan": 1500000,                     // optional >= 0, 0 = engine default
 *   "competitors": 0,                        // optional 0..15 company slots
 *   "currency": "EUR",                       // optional ISO code from the engine table; unknown = error
 *   "network_money": 50000000,               // optional 0..4e9 cash for the infrastructure company
 *   "bus_towns": 24,                         // optional 0..1024 towns that get a bus service
 *   "trains": 12,                            // optional 0..1024 stations that get a train
 *   "save":       "output.scn"
 * }
 * heightmap/towns are resolved by basename through the engine file search
 * (scenario/heightmap in the personal dir); every other data file
 * (industries, rivers, roads, autobahnen, rail, airports, harbours) is read
 * directly; save goes to the scenario dir.
 *
 * Network phase (optional, all off when the files are absent):
 * The road and rail routers, airports, docks and the starting fleet live in the
 * same translation unit. They are strictly opt-in per input file, so a manifest
 * that only names a heightmap and towns builds exactly what it built before the
 * network phase existed. They need a company, because the engine asks
 * _current_company for rail types, for money and for the owner of a new
 * station - a scenario has none until it is loaded - so the build borrows one
 * (an AI-flagged company, so a player is not handed it on load) and keeps it in
 * the save, which means "competitors": 0 is a build error once the network is
 * built: the player would have no free slot to start in.
 *
 * Settings that are not pinned in the manifest are inherited from the -c config
 * file, which is machine state rather than intent. Everything that would be
 * silently wrong in every player's save if the config happened to differ
 * (start year, snow line, loan, company slots, currency, selective servicing)
 * is therefore either pinned by default or rejected when out of range.
 *
 * On success the build logs one machine-readable audit line, e.g.
 *   hsb: audit: towns=124 towns_on_water=0 industries=73 water=2810000
 *        sea_fraction=0.168 water_bodies=41 isolated_water_tiles=2
 *        water_unreachable=310 carved=18800 carved_reachable=18800
 *        roads=73226 road_components=1 largest_road=71890 towns_on_network=124
 *        intercity_towns=124 rail=259 rail_components=37 stations=61 depots=48
 *        airports=27 docks=21 vehicles=35 vehicles_routed=35
 *
 * Run: HSB_BUILD=manifest.json openttd -e -c <config>
 * (config must activate the wanted NewGRFs first).
 *
 * Old manifests without "bounds" are rejected with a clear error - add the
 * geographic bounds your heightmap/towns were built with (see
 * scripts/build-manifest.py).
 *
 * This file is part of openTTD-HSB.
 * Licensed under the GNU General Public License, version 2.
 * See LICENSE.md for details.
 */

bool HsbBuildRequested();
bool HsbBuildScenario();

/**
 * The tile a generated scenario should open on, or #INVALID_TILE.
 *
 * The headless builder has no window, so there is no viewport to read a position
 * from and the saved value is deliberately invalid. The player then starts on the
 * map corner - a field with a river and nothing on it. The builder sets this tile
 * so that the position is written into the .scn as data, which is the only side
 * that reaches the file: a stock, unpatched engine reads the same saved fields,
 * so the fix ships without needing a patched binary on the player's machine.
 * Declared here (rather than in saveload/misc_sl.cpp) so the assignment in the
 * builder and the read in the save code are one contract, not two declarations
 * that can drift apart.
 *
 * Defined by the saveload patch, so it is only available when that patch is
 * applied - which is the only way this file is ever built.
 */
extern TileIndex _hsb_start_view_tile;

#endif /* HSB_BUILDER_H */
