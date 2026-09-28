#ifndef HSB_BUILDER_H
#define HSB_BUILDER_H

/**
 * HSB headless scenario builder.
 *
 * Driven by the HSB_BUILD environment variable pointing at a JSON manifest:
 * {
 *   "heightmap":  "/abs/path/map-4096.png",
 *   "towns":      "/abs/path/towns.json",
 *   "industries": "/abs/path/industries.json",
 *   "rivers":     "/abs/path/rivers.json",   // optional, omit or null to skip
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
 *   "save":       "output.scn"
 * }
 * heightmap/towns are resolved by basename through the engine file search
 * (scenario/heightmap in the personal dir); industries/rivers manifests are
 * read directly; save goes to the scenario dir.
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
 *
 * Run: HSB_BUILD=manifest.json openttd -e -c <config>
 * (config must activate the wanted NewGRFs first).
 *
 * Old manifests without "bounds" are rejected with a clear error — add the
 * geographic bounds your heightmap/towns were built with (see
 * scripts/build-manifest.py).
 *
 * This file is part of openTTD-HSB.
 * Licensed under the GNU General Public License, version 2.
 * See LICENSE.md for details.
 */

bool HsbBuildRequested();
bool HsbBuildScenario();

#endif /* HSB_BUILDER_H */
