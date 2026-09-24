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
 * Extracted and generalized from GermanyTransportTycoonDeluxe patch 06
 * (GTTD headless builder, 2026-09-24: 4096 map, 124 towns, 73 industries).
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

#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

bool IsPowerOfTwo(uint v)
{
	return v >= 64 && (v & (v - 1)) == 0;
}

std::string BasenameOf(const std::string &path)
{
	return std::filesystem::path(path).filename().string();
}

/** Tile for a lat/lon position (clockwise heightmap mapping, square maps). */
TileIndex HsbTileFor(double lat, double lon)
{
	uint32_t tx = static_cast<uint32_t>((g_hsb_bounds.lat_max - lat) / (g_hsb_bounds.lat_max - g_hsb_bounds.lat_min) * Map::SizeX());
	uint32_t ty = static_cast<uint32_t>((lon - g_hsb_bounds.lon_min) / (g_hsb_bounds.lon_max - g_hsb_bounds.lon_min) * Map::SizeY());
	return TileXY(std::min<uint32_t>(tx, Map::SizeX() - 1), std::min<uint32_t>(ty, Map::SizeY() - 1));
}

void HsbPaintWater(TileIndex t, bool sea)
{
	if (!IsValidTile(t)) return;
	if (IsTileType(t, MP_WATER)) return;
	SetTileHeight(t, 0);
	if (sea) {
		MakeSea(t);
	} else {
		MakeRiver(t, 0);
	}
}

/** Resolve the manifest path: HSB_BUILD wins, GTTD_BUILD is a deprecated alias. */
const char *HsbManifestPath()
{
	const char *p = std::getenv("HSB_BUILD");
	if (p != nullptr) return p;
	p = std::getenv("GTTD_BUILD");
	if (p != nullptr) {
		Debug(misc, 0, "hsb: GTTD_BUILD is deprecated, use HSB_BUILD");
		return p;
	}
	return nullptr;
}

bool HsbParseBounds(const nlohmann::json &manifest)
{
	if (!manifest.count("bounds")) {
		Debug(misc, 0, "hsb: manifest needs a \"bounds\" object "
			"{\"lat_min\", \"lat_max\", \"lon_min\", \"lon_max\"} "
			"(see scripts/build-manifest.py --help)");
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
	if (!(g_hsb_bounds.lat_min < g_hsb_bounds.lat_max) || !(g_hsb_bounds.lon_min < g_hsb_bounds.lon_max)) {
		Debug(misc, 0, "hsb: bounds must satisfy lat_min < lat_max and lon_min < lon_max");
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
	if (!manifest.count("rivers") || manifest.at("rivers").is_null()) return true;
	const std::string rivers_path = manifest.at("rivers").get<std::string>();
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

	uint carved = 0;
	for (const auto &seg : rivers) {
		const bool sea = seg.at("class").get<std::string>() == "sea";
		const int width = seg.at("width").get<int>();
		const auto &wps = seg.at("waypoints");
		for (size_t i = 0; i + 1 < wps.size(); ++i) {
			double lat0 = wps[i].at(0).get<double>(), lon0 = wps[i].at(1).get<double>();
			double lat1 = wps[i + 1].at(0).get<double>(), lon1 = wps[i + 1].at(1).get<double>();
			double dist = std::max(std::abs(lat1 - lat0), std::abs(lon1 - lon0));
			uint steps = static_cast<uint>(dist / 0.002) + 1;
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
	Debug(misc, 0, "hsb: carved {} water tiles", carved);
	FixSlopes();
	MarkWholeScreenDirty();
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

	if (!HsbParseBounds(manifest)) return false;

	const std::string heightmap = manifest.at("heightmap").get<std::string>();
	const std::string towns = manifest.at("towns").get<std::string>();
	const std::string industries_path = manifest.at("industries").get<std::string>();
	const int start_year = manifest.at("start_year").get<int>();
	const std::string save = manifest.at("save").get<std::string>();
	const int heightmap_height = manifest.count("heightmap_height") ? manifest.at("heightmap_height").get<int>() : 12;
	/* Fixed seed => bit-identical rebuilds. Manifest may override. */
	const uint32_t generation_seed = manifest.count("seed") ? manifest.at("seed").get<uint32_t>() : 20250924;
	g_hsb_industry_radius = manifest.count("industry_search_radius") ? manifest.at("industry_search_radius").get<uint>() : 32;

	std::string rotation = "clockwise";
	if (manifest.count("heightmap_rotation") && !manifest.at("heightmap_rotation").is_null()) {
		rotation = manifest.at("heightmap_rotation").get<std::string>();
	}
	_settings_newgame.game_creation.generation_seed = generation_seed;

	/* 1. Terrain: same as the GUI heightmap flow (dimensions from the PNG). */
	uint dim_x = 0, dim_y = 0;
	if (!GetHeightmapDimensions(DFT_HEIGHTMAP_PNG, BasenameOf(heightmap), &dim_x, &dim_y)) {
		Debug(misc, 0, "hsb: cannot read heightmap {}", heightmap);
		return false;
	}
	if (!IsPowerOfTwo(dim_x) || !IsPowerOfTwo(dim_y)) {
		Debug(misc, 0, "hsb: heightmap dimensions must be powers of 2, got {}x{}", dim_x, dim_y);
		return false;
	}
	/* Newgame settings: GenerateWorld resets _settings_game from these, so
	 * rotation/height must be set here (not on _settings_game directly). */
	_settings_newgame.game_creation.map_x = static_cast<uint8_t>(std::bit_width(dim_x) - 1);
	_settings_newgame.game_creation.map_y = static_cast<uint8_t>(std::bit_width(dim_y) - 1);
	if (rotation == "clockwise") {
		_settings_newgame.game_creation.heightmap_rotation = HM_CLOCKWISE;
	} else if (rotation == "counter-clockwise" || rotation == "counter_clockwise" || rotation == "counterclockwise") {
		_settings_newgame.game_creation.heightmap_rotation = HM_COUNTER_CLOCKWISE;
	} else {
		Debug(misc, 0, "hsb: unknown heightmap_rotation '{}', want clockwise| counter-clockwise", rotation);
		return false;
	}
	_settings_newgame.game_creation.heightmap_height = heightmap_height;
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
	Debug(misc, 0, "hsb: towns placed: {}", towns_placed);
	if (towns_placed == 0) {
		Debug(misc, 0, "hsb: no towns placed, aborting");
		return false;
	}
	for (const Sign *s : Sign::Iterate()) {
		Debug(misc, 0, "hsb: town fallback sign: {} at {},{}", s->name, s->x, s->y);
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
	for (const auto &entry : ind_json) {
		++idx;
		const std::string type = entry.at("type").get<std::string>();
		const double lat = entry.at("lat").get<double>();
		const double lon = entry.at("lon").get<double>();
		const std::string near = entry.count("near") ? entry.at("near").get<std::string>() : type;
		auto it = by_name.find(type);
		if (it == by_name.end()) {
			Debug(misc, 0, "hsb: unknown industry type '{}' near {}", type, near);
			++failed;
			continue;
		}
		/* Same proportion convention as towns.json (x from north, y from west). */
		double x_prop = (g_hsb_bounds.lat_max - lat) / (g_hsb_bounds.lat_max - g_hsb_bounds.lat_min);
		double y_prop = (lon - g_hsb_bounds.lon_min) / (g_hsb_bounds.lon_max - g_hsb_bounds.lon_min);
		TileIndex target = TileXY(x_prop * Map::MaxX(), y_prop * Map::MaxY());
		Town *nearest = ClosestTownFromTile(target, UINT_MAX);
		std::string nearest_name = (nearest == nullptr) ? "<none>" : nearest->name;
		bool ok = false;
		std::map<std::string, uint> err_counts;
		for (TileIndex t : SpiralTileSequence(target, g_hsb_industry_radius, 0, 0)) {
			CommandCost res = Command<CMD_BUILD_INDUSTRY>::Do(DoCommandFlag::Execute, t, it->second, 0, false, idx);
			if (res.Succeeded()) {
				ok = true;
				break;
			}
			err_counts[GetString(res.GetErrorMessage())]++;
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
