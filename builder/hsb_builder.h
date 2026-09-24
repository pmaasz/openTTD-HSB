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
 *   "seed": 20250924,                        // optional, fixed generation seed
 *   "industry_search_radius": 32,            // optional, spiral fallback tiles
 *   "save":       "output.scn"
 * }
 * heightmap/towns are resolved by basename through the engine file search
 * (scenario/heightmap in the personal dir); industries/rivers manifests are
 * read directly; save goes to the scenario dir.
 *
 * Run: HSB_BUILD=manifest.json openttd -e -c <config>
 * (config must activate the wanted NewGRFs first).
 *
 * Migration: GTTD_BUILD is accepted as a deprecated alias for HSB_BUILD.
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
