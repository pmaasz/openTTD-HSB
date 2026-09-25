/**
 * HSB headless scenario builder.
 *
 * Replays the Scenario Editor workflow deterministically, without GUI input:
 * heightmap -> waters (rivers/estuaries) -> town JSON import ->
 * industry placement -> start year -> save.
 * Every step reuses the exact engine functions the GUI calls.
 *
 * Driven by the HSB_BUILD environment variable (JSON manifest path).
 * Geographic bounds come from the manifest ("bounds"), so the same binary
 * builds any region — no recompilation per map.
 *
 * This file is part of openTTD-HSB.
 * Licensed under the GNU General Public License, version 2.
 * See LICENSE.md for details.
 */

#include "stdafx.h"

#include "hsb_builder.h"

#include "3rdparty/nlohmann/json.hpp"
#include "command_func.h"
#include "company_func.h"
#include "company_type.h"
#include "core/backup_type.hpp"
#include "debug.h"
#include "fileio_type.h"
#include "genworld.h"
#include "heightmap.h"
#include "industry_cmd.h"
#include "industrytype.h"
#include "map_func.h"
#include "openttd.h"
#include "saveload/saveload.h"
#include "settings_type.h"
#include "signs_base.h"
#include "strings_func.h"
#include "table/strings.h"
#include "tilearea_type.h"
#include "timer/timer_game_calendar.h"
#include "town.h"
#include "viewport_func.h"
#include "water_map.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string>

void SetStartingYear(TimerGameCalendar::Year year);

/* Defined in industry_gui.cpp without a header; same extern as industry_cmd.cpp uses. */
extern bool _ignore_industry_restrictions;

namespace {

/** Geographic bounds for lat/lon -> tile mapping (from manifest "bounds"). */
struct HsbBounds {
	double lat_min = 0.0;
	double lat_max = 0.0;
	double lon_min = 0.0;
	double lon_max = 0.0;
};

HsbBounds g_hsb_bounds;

/** Towns use the same spiral fallback radius as LoadTownData; industries are bigger. */
uint g_hsb_industry_radius = 32;
bool g_hsb_counter_clockwise = false;

bool IsPowerOfTwo(uint v)
{
	return v >= 64 && v <= MAX_MAP_SIZE && (v & (v - 1)) == 0;
}

std::string BasenameOf(const std::string &path)
{
	return std::filesystem::path(path).filename().string();
}

bool HsbCoordinateValid(double lat, double lon)
{
	return std::isfinite(lat) && std::isfinite(lon)
		&& g_hsb_bounds.lat_min <= lat && lat <= g_hsb_bounds.lat_max
		&& g_hsb_bounds.lon_min <= lon && lon <= g_hsb_bounds.lon_max;
}

/** Tile for a lat/lon position (clockwise heightmap mapping, square maps). */
TileIndex HsbTileFor(double lat, double lon)
{
	double x_prop = (g_hsb_bounds.lat_max - lat) / (g_hsb_bounds.lat_max - g_hsb_bounds.lat_min);
	double y_prop = (lon - g_hsb_bounds.lon_min) / (g_hsb_bounds.lon_max - g_hsb_bounds.lon_min);
	double tile_x = g_hsb_counter_clockwise ? 1.0 - y_prop : x_prop;
	double tile_y = g_hsb_counter_clockwise ? x_prop : y_prop;
	uint32_t tx = static_cast<uint32_t>(std::clamp(std::lround(tile_x * Map::MaxX()), 0L, static_cast<long>(Map::MaxX())));
	uint32_t ty = static_cast<uint32_t>(std::clamp(std::lround(tile_y * Map::MaxY()), 0L, static_cast<long>(Map::MaxY())));
	return TileXY(tx, ty);
}

void HsbPaintWater(TileIndex t, bool sea)
{
	if (!IsValidTile(t) || IsTileType(t, MP_WATER)) return;
	const uint x = TileX(t);
	const uint y = TileY(t);
	SetTileHeight(t, 0);
	if (x + 1 < Map::SizeX()) SetTileHeight(TileXY(x + 1, y), 0);
	if (y + 1 < Map::SizeY()) SetTileHeight(TileXY(x, y + 1), 0);
	if (x + 1 < Map::SizeX() && y + 1 < Map::SizeY()) SetTileHeight(TileXY(x + 1, y + 1), 0);
	if (sea) {
		MakeSea(t);
	} else {
		MakeRiver(t, 0);
	}
}

/** Resolve the manifest path: HSB_BUILD wins */
const char *HsbManifestPath()
{
	const char *p = std::getenv("HSB_BUILD");
	if (p != nullptr) return p;
	return nullptr;
}

bool HsbParseBounds(const nlohmann::json &manifest)
{
	if (!manifest.is_object() || !manifest.contains("bounds") || !manifest.at("bounds").is_object()) {
		Debug(misc, 0, "hsb: manifest needs a bounds object with lat_min, lat_max, lon_min, and lon_max");
		return false;
	}
	const auto &b = manifest.at("bounds");
	try {
		g_hsb_bounds.lat_min = b.at("lat_min").get<double>();
		g_hsb_bounds.lat_max = b.at("lat_max").get<double>();
		g_hsb_bounds.lon_min = b.at("lon_min").get<double>();
		g_hsb_bounds.lon_max = b.at("lon_max").get<double>();
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: bad bounds: {}", e.what());
		return false;
	}
	if (!(std::isfinite(g_hsb_bounds.lat_min) && std::isfinite(g_hsb_bounds.lat_max)
			&& std::isfinite(g_hsb_bounds.lon_min) && std::isfinite(g_hsb_bounds.lon_max))) {
		Debug(misc, 0, "hsb: bounds must be finite");
		return false;
	}
	if (!(g_hsb_bounds.lat_min < g_hsb_bounds.lat_max) || !(g_hsb_bounds.lon_min < g_hsb_bounds.lon_max)
			|| g_hsb_bounds.lat_min < -90.0 || g_hsb_bounds.lat_max > 90.0
			|| g_hsb_bounds.lon_min < -180.0 || g_hsb_bounds.lon_max > 180.0) {
		Debug(misc, 0, "hsb: bounds must be ordered WGS84 intervals");
		return false;
	}
	return true;
}

} // namespace

/**
 * Carve rivers/estuaries from the manifest's rivers file.
 * Waypoints are interpolated in lat/lon; brush width is in tiles.
 */
bool HsbCarveWaters(const nlohmann::json &manifest)
{
	if (!manifest.contains("rivers") || manifest.at("rivers").is_null()) return true;
	std::string rivers_path;
	try {
		rivers_path = manifest.at("rivers").get<std::string>();
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: bad rivers path: {}", e.what());
		return false;
	}
	if (rivers_path.empty()) return true;
	std::ifstream river_file(rivers_path);
	if (!river_file) {
		Debug(misc, 0, "hsb: cannot open {}", rivers_path);
		return false;
	}
	nlohmann::json rivers;
	try {
		river_file >> rivers;
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: rivers parse error: {}", e.what());
		return false;
	}
	if (!rivers.is_array()) {
		Debug(misc, 0, "hsb: rivers root must be an array");
		return false;
	}

	uint carved = 0;
	try {
		for (const auto &seg : rivers) {
			if (!seg.is_object() || !seg.contains("name") || !seg.at("name").is_string()
					|| !seg.contains("class") || !seg.at("class").is_string()
					|| !seg.contains("width") || !seg.at("width").is_number_integer()
					|| !seg.contains("waypoints") || !seg.at("waypoints").is_array()) {
				Debug(misc, 0, "hsb: invalid river segment schema");
				return false;
			}
			const std::string class_name = seg.at("class").get<std::string>();
			if (class_name != "river" && class_name != "sea") {
				Debug(misc, 0, "hsb: river class must be river or sea");
				return false;
			}
			const bool sea = class_name == "sea";
			const int width = seg.at("width").get<int>();
			if (width < 1 || width > 3) {
				Debug(misc, 0, "hsb: river width must be 1..3");
				return false;
			}
			const auto &wps = seg.at("waypoints");
			if (wps.size() < 2) {
				Debug(misc, 0, "hsb: river '{}' needs at least two waypoints", seg.at("name").get<std::string>());
				return false;
			}
			for (size_t i = 0; i + 1 < wps.size(); ++i) {
				if (!wps[i].is_array() || wps[i].size() != 2 || !wps[i + 1].is_array() || wps[i + 1].size() != 2) {
					Debug(misc, 0, "hsb: river waypoints must be [lat, lon] pairs");
					return false;
				}
				double lat0 = wps[i].at(0).get<double>(), lon0 = wps[i].at(1).get<double>();
				double lat1 = wps[i + 1].at(0).get<double>(), lon1 = wps[i + 1].at(1).get<double>();
				if (!HsbCoordinateValid(lat0, lon0) || !HsbCoordinateValid(lat1, lon1)) {
					Debug(misc, 0, "hsb: river waypoint is outside finite bounds");
					return false;
				}
				double dist = std::max(std::abs(lat1 - lat0), std::abs(lon1 - lon0));
				uint steps = static_cast<uint>(std::lround(dist / 0.002)) + 1;
				for (uint s = 0; s <= steps; ++s) {
					double lat = lat0 + (lat1 - lat0) * s / steps;
					double lon = lon0 + (lon1 - lon0) * s / steps;
					TileIndex c = HsbTileFor(lat, lon);
					int r = width / 2;
					for (int dx = (width == 2 ? 0 : -r); dx <= (width == 2 ? 1 : r); ++dx) {
						for (int dy = (width == 2 ? 0 : -r); dy <= (width == 2 ? 1 : r); ++dy) {
							TileIndex t = c + TileDiffXY(dx, dy);
							if (!IsValidTile(t)) continue;
							if (TileX(t) < 1 || TileY(t) < 1 || TileX(t) >= Map::SizeX() - 1 || TileY(t) >= Map::SizeY() - 1) continue;
							if (IsTileType(t, MP_WATER)) continue;
							HsbPaintWater(t, sea);
							++carved;
						}
					}
				}
			}
		}
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: invalid river data: {}", e.what());
		return false;
	}
	Debug(misc, 0, "hsb: carved {} water tiles", carved);
	FixSlopes();
	MarkWholeScreenDirty();
	return true;
}

bool HsbSafeFilename(const std::string &filename)
{
	if (filename.empty() || filename == "." || filename == "..") return false;
	return filename.find('\0') == std::string::npos
		&& filename.find_first_of("/\\:") == std::string::npos
		&& std::filesystem::path(filename).filename().string() == filename;
}

bool HsbValidateTownData(const std::string &path, size_t &town_count)
{
	std::ifstream input(path);
	if (!input) {
		Debug(misc, 0, "hsb: cannot open towns data {}", path);
		return false;
	}
	nlohmann::json towns;
	try {
		input >> towns;
		if (!towns.is_array() || towns.empty()) {
			Debug(misc, 0, "hsb: towns root must be a nonempty array");
			return false;
		}
		std::set<std::string> names;
		for (const auto &town : towns) {
			if (!town.is_object() || !town.contains("name") || !town.at("name").is_string()
					|| !town.contains("population") || !town.at("population").is_number_integer()
					|| !town.contains("city") || !town.at("city").is_boolean()
					|| !town.contains("x") || !town.at("x").is_number()
					|| !town.contains("y") || !town.at("y").is_number()) {
				Debug(misc, 0, "hsb: invalid town entry schema");
				return false;
			}
			const std::string name = town.at("name").get<std::string>();
			const uint64_t population = town.at("population").get<uint64_t>();
			const double x = town.at("x").get<double>();
			const double y = town.at("y").get<double>();
			if (name.empty() || !names.insert(name).second || population == 0 || population > std::numeric_limits<uint>::max()
					|| !std::isfinite(x) || !std::isfinite(y) || x <= 0.0 || x >= 1.0 || y <= 0.0 || y >= 1.0) {
				Debug(misc, 0, "hsb: invalid town entry '{}'", name);
				return false;
			}
		}
		town_count = towns.size();
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: invalid towns data: {}", e.what());
		return false;
	}
	return true;
}

bool HsbBuildRequested()
{
	static bool done = false;
	if (done) return false;
	if (HsbManifestPath() == nullptr) return false;
	if (_game_mode != GM_EDITOR || _switch_mode != SM_NONE) return false;
	done = true;
	return true;
}

bool HsbBuildScenario()
{
	const char *manifest_path = HsbManifestPath();
	if (manifest_path == nullptr) return false;

	std::ifstream manifest_file(manifest_path);
	if (!manifest_file) {
		Debug(misc, 0, "hsb: cannot open manifest {}", manifest_path);
		return false;
	}
	nlohmann::json manifest;
	try {
		manifest_file >> manifest;
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: manifest parse error: {}", e.what());
		return false;
	}
	if (!manifest.is_object() || !HsbParseBounds(manifest)) return false;

	std::string heightmap;
	std::string towns;
	std::string industries_path;
	std::string save;
	std::string rotation;
	int start_year;
	int heightmap_height;
	int64_t industry_search_radius;
	uint32_t generation_seed;
	try {
		heightmap = manifest.at("heightmap").get<std::string>();
		towns = manifest.at("towns").get<std::string>();
		industries_path = manifest.at("industries").get<std::string>();
		start_year = manifest.at("start_year").get<int>();
		save = manifest.at("save").get<std::string>();
		heightmap_height = !manifest.contains("heightmap_height") || manifest.at("heightmap_height").is_null() ? 12 : manifest.at("heightmap_height").get<int>();
		generation_seed = !manifest.contains("seed") || manifest.at("seed").is_null() ? 20250924 : manifest.at("seed").get<uint32_t>();
		industry_search_radius = !manifest.contains("industry_search_radius") || manifest.at("industry_search_radius").is_null() ? 32 : manifest.at("industry_search_radius").get<int64_t>();
		rotation = !manifest.contains("heightmap_rotation") || manifest.at("heightmap_rotation").is_null() ? "clockwise" : manifest.at("heightmap_rotation").get<std::string>();
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: invalid manifest fields: {}", e.what());
		return false;
	}
	if (heightmap.empty() || towns.empty() || industries_path.empty() || !HsbSafeFilename(save)
			|| !std::filesystem::path(heightmap).is_absolute()
			|| !std::filesystem::path(towns).is_absolute()
			|| !std::filesystem::path(industries_path).is_absolute()
			|| !HsbSafeFilename(BasenameOf(heightmap))
			|| !HsbSafeFilename(BasenameOf(towns))) {
		Debug(misc, 0, "hsb: manifest paths or save filename are invalid");
		return false;
	}
	if (start_year < 0 || start_year > 5'000'000 || heightmap_height < 1 || heightmap_height > static_cast<int>(MAX_TILE_HEIGHT)
			|| industry_search_radius < 1 || industry_search_radius > 512) {
		Debug(misc, 0, "hsb: manifest year, height, or industry radius is out of range");
		return false;
	}
	g_hsb_industry_radius = static_cast<uint>(industry_search_radius);
	if (rotation == "clockwise") {
		g_hsb_counter_clockwise = false;
	} else if (rotation == "counter-clockwise" || rotation == "counter_clockwise" || rotation == "counterclockwise") {
		g_hsb_counter_clockwise = true;
	} else {
		Debug(misc, 0, "hsb: unknown heightmap_rotation '{}', want clockwise or counter-clockwise", rotation);
		return false;
	}

	size_t expected_towns = 0;
	if (!HsbValidateTownData(towns, expected_towns)) return false;

	_settings_newgame.game_creation.generation_seed = generation_seed;
	_settings_newgame.game_creation.starting_year = TimerGameCalendar::Year(start_year);

	/* 1. Terrain: same as the GUI heightmap flow (dimensions from the PNG). */
	uint dim_x = 0, dim_y = 0;
	if (!GetHeightmapDimensions(DFT_HEIGHTMAP_PNG, BasenameOf(heightmap), &dim_x, &dim_y)) {
		Debug(misc, 0, "hsb: cannot read heightmap {}", heightmap);
		return false;
	}
	if (!IsPowerOfTwo(dim_x) || !IsPowerOfTwo(dim_y) || dim_x != dim_y) {
		Debug(misc, 0, "hsb: heightmap must be square powers of 2 in 64..4096, got {}x{}", dim_x, dim_y);
		return false;
	}
	/* Newgame settings: GenerateWorld resets _settings_game from these, so
	 * rotation/height must be set here (not on _settings_game directly). */
	_settings_newgame.game_creation.map_x = static_cast<uint8_t>(std::bit_width(dim_x) - 1);
	_settings_newgame.game_creation.map_y = static_cast<uint8_t>(std::bit_width(dim_y) - 1);
	_settings_newgame.game_creation.heightmap_rotation = g_hsb_counter_clockwise ? HM_COUNTER_CLOCKWISE : HM_CLOCKWISE;
	_settings_newgame.game_creation.heightmap_height = static_cast<uint8_t>(heightmap_height);
	_file_to_saveload.name = BasenameOf(heightmap);
	_file_to_saveload.ftype = FiosType{FT_HEIGHTMAP, DFT_HEIGHTMAP_PNG};
	Debug(misc, 0, "hsb: generating {}x{} terrain, height {} rotation {}", dim_x, dim_y, heightmap_height, rotation);
	GenerateWorld(GWM_HEIGHTMAP, dim_x, dim_y);

	/* 1b. Waters: carve rivers/estuaries from rivers.json (before towns). */
	if (!HsbCarveWaters(manifest)) return false;

	/* 2. Towns: the GUI "Load from file" path (our towns.json format). */
	_file_to_saveload.name = BasenameOf(towns);
	size_t towns_before = Town::GetNumItems();
	LoadTownData();
	size_t towns_placed = Town::GetNumItems() - towns_before;
	Debug(misc, 0, "hsb: towns placed: {}/{}", towns_placed, expected_towns);
	for (const Sign *s : Sign::Iterate()) {
		Debug(misc, 0, "hsb: town fallback sign: {} at {},{}", s->name, s->x, s->y);
	}
	if (towns_placed != expected_towns) {
		Debug(misc, 0, "hsb: town import incomplete, aborting");
		return false;
	}

	/* 3. Industries: resolve types by display name, place at anchors. */
	std::ifstream ind_file(industries_path);
	if (!ind_file) {
		Debug(misc, 0, "hsb: cannot open {}", industries_path);
		return false;
	}
	nlohmann::json ind_json;
	try {
		ind_file >> ind_json;
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: industries parse error: {}", e.what());
		return false;
	}
	if (!ind_json.is_array()) {
		Debug(misc, 0, "hsb: industries root must be an array");
		return false;
	}

	std::map<std::string, IndustryType> by_name;
	for (IndustryType it : _sorted_industry_types) {
		const IndustrySpec *spec = GetIndustrySpec(it);
		if (!spec->enabled) continue;
		by_name.emplace(GetString(spec->name), it);
	}
	Debug(misc, 0, "hsb: {} industry types available", by_name.size());
	for (const auto &named : by_name) {
		Debug(misc, 0, "hsb: available industry '{}'", named.first);
	}

	uint placed = 0, failed = 0, idx = 0;
	/* Mirror the editor GUI (industry_gui.cpp OnPlaceObject): build as
	 * OWNER_NONE with industry restrictions bypassed. */
	AutoRestoreBackup backup_cur_company(_current_company, OWNER_NONE);
	AutoRestoreBackup backup_generating_world(_generating_world, true);
	AutoRestoreBackup backup_ignore_restrictions(_ignore_industry_restrictions, true);
	try {
		for (const auto &entry : ind_json) {
			++idx;
			if (!entry.is_object() || !entry.contains("type") || !entry.at("type").is_string()
					|| !entry.contains("lat") || !entry.at("lat").is_number()
					|| !entry.contains("lon") || !entry.at("lon").is_number()
					|| (entry.contains("near") && !entry.at("near").is_string())) {
				Debug(misc, 0, "hsb: invalid industry entry schema");
				return false;
			}
			const std::string type = entry.at("type").get<std::string>();
			const double lat = entry.at("lat").get<double>();
			const double lon = entry.at("lon").get<double>();
			const std::string near = entry.contains("near") ? entry.at("near").get<std::string>() : type;
			if (type.empty() || !HsbCoordinateValid(lat, lon)) {
				Debug(misc, 0, "hsb: invalid industry type or coordinates");
				return false;
			}
			auto it = by_name.find(type);
			if (it == by_name.end()) {
				Debug(misc, 0, "hsb: unknown industry type '{}' near {}", type, near);
				++failed;
				continue;
			}
			TileIndex target = HsbTileFor(lat, lon);
			Town *nearest = ClosestTownFromTile(target, UINT_MAX);
			std::string nearest_name = (nearest == nullptr) ? "<none>" : nearest->name;
			bool ok = false;
			std::map<std::string, uint> err_counts;
			CommandCost anchor_result = Command<CMD_BUILD_INDUSTRY>::Do(DoCommandFlag::Execute, target, it->second, 0, false, idx);
			if (anchor_result.Succeeded()) {
				ok = true;
			} else {
				err_counts[GetString(anchor_result.GetErrorMessage())]++;
			}
			if (!ok) {
				for (TileIndex t : SpiralTileSequence(target, g_hsb_industry_radius, 0, 0)) {
					CommandCost res = Command<CMD_BUILD_INDUSTRY>::Do(DoCommandFlag::Execute, t, it->second, 0, false, idx);
					if (res.Succeeded()) {
						ok = true;
						break;
					}
					err_counts[GetString(res.GetErrorMessage())]++;
				}
			}
			if (ok) {
				++placed;
			} else {
				std::string top;
				for (const auto &kv : err_counts) {
					if (!top.empty()) top += " / ";
					top += std::to_string(kv.second) + "x " + kv.first;
					if (top.size() > 200) break;
				}
				Debug(misc, 0, "hsb: could not place '{}' near {} (tile {},{} nearest town {}): {}", type, near,
					TileX(target), TileY(target), nearest_name, top);
				++failed;
			}
		}
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: invalid industry entry: {}", e.what());
		return false;
	}
	Debug(misc, 0, "hsb: industries placed {}/{}, failed {}", placed, ind_json.size(), failed);
	if (failed > 0) return false;

	/* 4. Start year + save. */
	SetStartingYear(TimerGameCalendar::Year(start_year));
	if (SaveOrLoad(save, SLO_SAVE, DFT_GAME_FILE, SCENARIO_DIR, false) != SL_OK) {
		Debug(misc, 0, "hsb: save failed");
		return false;
	}
	Debug(misc, 0, "hsb: saved {}", save);
	return true;
}
