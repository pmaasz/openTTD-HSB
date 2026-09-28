/**
 * HSB headless scenario builder.
 *
 * Replays the Scenario Editor workflow deterministically, without GUI input:
 * heightmap -> start year -> waters (rivers/estuaries) -> town JSON import ->
 * industry placement -> audit -> save. Every step reuses the exact engine
 * functions the GUI calls.
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
#include "hsb_builder_math.h"

#include "3rdparty/nlohmann/json.hpp"
#include "command_func.h"
#include "company_func.h"
#include "company_type.h"
#include "core/backup_type.hpp"
#include "currency.h"
#include "debug.h"
#include "fileio_type.h"
#include "genworld.h"
#include "heightmap.h"
#include "industry_cmd.h"
#include "industrytype.h"
#include "map_func.h"
#include "newgrf_storage.h"
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
#include <vector>

void SetStartingYear(TimerGameCalendar::Year year);

/* Defined in industry_gui.cpp without a header; same extern as industry_cmd.cpp uses. */
extern bool _ignore_industry_restrictions;

namespace {

HsbBounds g_hsb_bounds;

/** Towns use the same spiral fallback radius as LoadTownData; industries are bigger. */
uint g_hsb_industry_radius = 32;
bool g_hsb_counter_clockwise = false;

/** Tiles this builder carved, so the audit can ask whether a ship can reach them. */
std::vector<TileIndex> g_hsb_carved_water;

/**
 * RAII gameloop scope for NewGRF persistent storage.
 *
 * NewGRF storage writes only commit inside gameloop or command scope
 * (BasePersistentStorageArray::AreChangesPersistent). Command::Do() does not
 * enter command scope itself - it expects the ambient gameloop of normal play
 * / worldgen - and the editor runs outside both, so every industry/town
 * register write stays "temporary" and the scenario save reverts it to zero.
 * That is dead production for any register-driven industry set, and it is
 * invisible: the build log still says every industry was placed.
 *
 * This mirrors GenerateWorld (genworld.cpp) and the editor's own industry
 * generator (industry_gui.cpp), which both bracket content creation in
 * gameloop mode. Gameloop (not command) scope is used so the nested per-command
 * test/execute brackets inside Command::Do() cannot unbalance it: persistence
 * then only depends on testmode, which commands manage correctly.
 */
struct HsbGameloopScope {
	HsbGameloopScope() { BasePersistentStorageArray::SwitchMode(PSM_ENTER_GAMELOOP); }
	~HsbGameloopScope() { BasePersistentStorageArray::SwitchMode(PSM_LEAVE_GAMELOOP); }
};

std::string BasenameOf(const std::string &path)
{
	return std::filesystem::path(path).filename().string();
}

static TileIndex HsbTileClamped(int x, int y)
{
	return TileXY(Clamp<int>(x, 0, Map::MaxX()), Clamp<int>(y, 0, Map::MaxY()));
}

/** Tile for a lat/lon position (clockwise heightmap mapping, square maps). */
TileIndex HsbTileFor(double lat, double lon)
{
	const HsbTile tile = HsbTileForBounds(g_hsb_bounds, lat, lon,
		Map::MaxX(), Map::MaxY(), g_hsb_counter_clockwise);
	return TileXY(tile.x, tile.y);
}

/** Staircase walk between two tiles, calling step(tile) for both ends of every step. */
template <typename Tstep>
void HsbWalkSteps(TileIndex from, TileIndex to, Tstep step)
{
	HsbStaircaseWalk(TileX(from), TileY(from), TileX(to), TileY(to),
		[&](int x, int y, bool) { step(TileXY(x, y)); });
}

/**
 * Turn a tile into navigable water, and level it with its neighbours.
 *
 * The guard tests IsWaterTile, not IsTileType(MP_WATER). A tile that is
 * already water but carries a non-Clear WaterTileType - a Coast tile, which
 * the world generator hands out all along the shoreline - must be re-stamped,
 * because IsWaterTile is also the predicate every connectivity mask below
 * uses. Skipping it leaves a one-tile gap that a ship cannot cross and that no
 * amount of channel cutting can bridge: the tile is water (so nothing paints
 * it) and not Clear water (so nothing floods through it). Since the shoreline
 * is exactly where a river is supposed to end, that gap is not an edge case.
 */
void HsbPaintWater(TileIndex t, bool sea)
{
	if (!IsValidTile(t) || IsWaterTile(t)) return;
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

/**
 * Paint a brush of the given width centred on `centre`. Idempotent: water
 * already there is left alone, so the 4-connected walk may stamp overlapping
 * tiles without counting them twice.
 *
 * width is the brush size per axis as documented in docs/FORMATS.md: 1 -> 1x1,
 * 2 -> 2x2, 3 -> 3x3. A `width <= 2` test here instead of `width == 2` makes
 * width 1 paint a 2x2 square, which silently quadruples a narrow river and
 * makes the carved count disagree with the documented width.
 */
void HsbPaintBrush(TileIndex centre, int width, bool sea, uint &carved)
{
	const int r = width / 2;
	for (int dx = (width == 2 ? 0 : -r); dx <= (width == 2 ? 1 : r); ++dx) {
		for (int dy = (width == 2 ? 0 : -r); dy <= (width == 2 ? 1 : r); ++dy) {
			TileIndex t = centre + TileDiffXY(dx, dy);
			if (!IsValidTile(t)) continue;
			if (TileX(t) < 1 || TileY(t) < 1 || TileX(t) >= Map::SizeX() - 1 || TileY(t) >= Map::SizeY() - 1) continue;
			/* Count a tile only when this stroke actually changed it, so `carved`
			 * equals the number of tiles the builder is responsible for. */
			if (IsWaterTile(t)) continue;
			HsbPaintWater(t, sea);
			g_hsb_carved_water.push_back(t);
			++carved;
		}
	}
}

/** Resolve the manifest path from HSB_BUILD, or nullptr when unset. */
const char *HsbManifestPath()
{
	return std::getenv("HSB_BUILD");
}

bool HsbParseBounds(const nlohmann::json &manifest)
{
	if (!manifest.is_object() || !manifest.contains("bounds") || !manifest.at("bounds").is_object()) {
		Debug(misc, 0, "hsb: manifest needs a bounds object with lat_min, lat_max, lon_min, and lon_max");
		return false;
	}
	const auto &b = manifest.at("bounds");
	try {
		if (!b.at("lat_min").is_number() || !b.at("lat_max").is_number()
				|| !b.at("lon_min").is_number() || !b.at("lon_max").is_number()) {
			Debug(misc, 0, "hsb: bounds values must be numbers");
			return false;
		}
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

/** The four cardinal neighbours, as TileDiffXY offsets. */
constexpr int kHsbNeighbours[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

/** How far (Chebyshev, in tiles) a carved body may be from open water to still get a channel. */
constexpr int kHsbConnectRadius = 96;

/** Flood 4-connected over `mask`, starting at `sources`. Returns how many tiles were reached. */
uint HsbFloodCount(const std::vector<uint8_t> &mask, const std::vector<TileIndex> &sources,
	std::vector<uint8_t> &reached)
{
	std::vector<TileIndex> queue;
	queue.reserve(1 << 16);
	for (TileIndex start : sources) {
		if (!IsValidTile(start) || reached[start.base()] || !mask[start.base()]) continue;
		reached[start.base()] = 1;
		queue.push_back(start);
	}
	uint total = 0;
	while (!queue.empty()) {
		TileIndex cur = queue.back();
		queue.pop_back();
		++total;
		for (const auto &d : kHsbNeighbours) {
			TileIndex n = cur + TileDiffXY(d[0], d[1]);
			if (!IsValidTile(n) || reached[n.base()] || !mask[n.base()]) continue;
			reached[n.base()] = 1;
			queue.push_back(n);
		}
	}
	return total;
}

/**
 * Connect every carved river to water a ship can actually reach.
 *
 * The river waypoints end near the coast, but "near" is not "at": the carved
 * tiles are painted at height 0 while the coastline in the DEM sits a tile or
 * two inland, so a river authored to reach the sea can come out as a closed
 * pond. A canal no ship can enter is a picture of a canal, and the build log
 * reports success either way. So after carving, every body that does not touch
 * open water gets a channel cut to the nearest water a ship could already use.
 */
void HsbConnectCarvedWater()
{
	const size_t size = Map::Size();

	std::vector<uint8_t> carved(size, 0);
	for (TileIndex t : g_hsb_carved_water) carved[t.base()] = 1;
	std::vector<uint8_t> open_water(size, 0);
	uint open_total = 0;
	for (uint s = 0; s < size; ++s) {
		TileIndex probe(static_cast<TileIndex::BaseType>(s));
		if (IsWaterTile(probe) && !carved[s]) { open_water[s] = 1; ++open_total; }
	}
	if (open_total == 0) {
		Debug(misc, 0, "hsb: no open water to connect the carved rivers to");
		return;
	}

	uint bodies = 0, already = 0, connected = 0, gave_up = 0;
	std::vector<uint8_t> seen(size, 0);
	std::vector<TileIndex> queue;
	std::vector<TileIndex> body;
	queue.reserve(1 << 16);

	/* Iterate a snapshot: cutting a channel appends to g_hsb_carved_water, and a
	 * range-for over the live vector would keep reading through an iterator that
	 * reallocation already invalidated. That is how this ends in
	 * malloc_consolidate(): invalid chunk size. The channel tiles are the same
	 * body we just handled, so they must not be enumerated as new bodies either;
	 * they are only appended so the audit can count them as carved. */
	const std::vector<TileIndex> bodies_to_visit = g_hsb_carved_water;
	for (TileIndex start : bodies_to_visit) {
		if (seen[start.base()]) continue;
		body.clear();
		queue.clear();
		queue.push_back(start);
		seen[start.base()] = 1;
		while (!queue.empty()) {
			TileIndex cur = queue.back();
			queue.pop_back();
			body.push_back(cur);
			for (const auto &d : kHsbNeighbours) {
				TileIndex n = cur + TileDiffXY(d[0], d[1]);
				if (!IsValidTile(n) || seen[n.base()] || !carved[n.base()]) continue;
				seen[n.base()] = 1;
				queue.push_back(n);
			}
		}
		++bodies;

		bool touches = false;
		for (TileIndex t : body) {
			for (const auto &d : kHsbNeighbours) {
				TileIndex n = t + TileDiffXY(d[0], d[1]);
				if (IsValidTile(n) && open_water[n.base()]) { touches = true; break; }
			}
			if (touches) break;
		}
		if (touches) { ++already; continue; }

		/* Nearest open water to this body, scanning outwards over whole
		 * Chebyshev rings. Axis-only rays miss a mouth that is 60 tiles inland
		 * and 60 tiles along the coast, which is a perfectly ordinary estuary. */
		TileIndex from = TileIndex{};
		TileIndex to = TileIndex{};
		uint best = UINT_MAX;
		for (TileIndex t : body) {
			for (int r = 1; r <= kHsbConnectRadius && static_cast<uint>(r) < best; ++r) {
				for (int dy = -r; dy <= r; ++dy) {
					for (int dx = -r; dx <= r; ++dx) {
						if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
						TileIndex cand = HsbTileClamped(TileX(t) + dx, TileY(t) + dy);
						if (IsValidTile(cand) && open_water[cand.base()]) {
							from = t;
							to = cand;
							best = static_cast<uint>(r);
							break;
						}
					}
					if (best != UINT_MAX) break;
				}
				if (best != UINT_MAX) break;
			}
			if (best != UINT_MAX) break;
		}
		if (to == TileIndex{} || best == UINT_MAX) { ++gave_up; continue; }

		/* Walk the channel 4-connected, so it is one body and not a chain of
		 * corner-touching stamps. Count the body as connected only once the walk
		 * actually arrived: an unreachable canal is a picture of a canal, and
		 * reporting it as fixed is exactly the failure this step exists to stop.
		 * The endpoints are already water (one is the body, one is open water) and
		 * so are never painted, but they are still recorded as visited. */
		TileIndex arrived = from;
		HsbWalkSteps(from, to, [&](TileIndex t) {
			if (t != from && t != to && !carved[t.base()] && !open_water[t.base()]) {
				HsbPaintWater(t, true);
				g_hsb_carved_water.push_back(t);
				carved[t.base()] = 1;
			}
			arrived = t;
		});
		if (arrived == to) {
			++connected;
		} else {
			++gave_up;
		}
	}
	Debug(misc, 0, "hsb: carved water bodies {}, already open {}, connected {}, unreachable {}",
		bodies, already, connected, gave_up);
	if (gave_up > 0) {
		Debug(misc, 0, "hsb: {} carved body/bodies are still closed: no open water within {} tiles, "
			"or the channel could not be walked. Extend those river waypoints toward the coast.", gave_up,
			kHsbConnectRadius);
	}
}

/**
 * Measure what the build actually produced.
 *
 * A map that only *looks* right is worse than an obviously broken one: a town
 * in the middle of a lake, or a canal a ship cannot enter, and nothing in the
 * build log says so. One machine-readable line, so a CI gate or a script can
 * assert on the numbers instead of a human eyeballing the save.
 */
void HsbAuditBuild(uint towns_placed, uint industries_placed)
{
	const size_t size = Map::Size();
	std::vector<uint8_t> carved_flag(size, 0);
	uint carved_total = 0;
	for (TileIndex t : g_hsb_carved_water) {
		if (IsValidTile(t) && IsWaterTile(t)) { carved_flag[t.base()] = 1; ++carved_total; }
	}

	/* "Can a ship get there from the sea" - the question that matters for carved
	 * rivers. A multi-source flood from every water tile the builder did NOT
	 * carve: every carved tile it reaches is one a ship can actually use.
	 * Comparing against the single largest water body is wrong - it reported 0
	 * while the rivers were demonstrably connected. */
	std::vector<uint8_t> water(size, 0);
	uint water_total = 0;
	std::vector<TileIndex> open_sources;
	open_sources.reserve(1 << 16);
	for (uint s = 0; s < size; ++s) {
		TileIndex t(static_cast<TileIndex::BaseType>(s));
		if (!IsWaterTile(t)) continue;
		water[s] = 1;
		++water_total;
		if (!carved_flag[s]) open_sources.push_back(t);
	}
	std::vector<uint8_t> from_open(size, 0);
	const uint open_reached = HsbFloodCount(water, open_sources, from_open);
	uint carved_reachable = 0;
	for (uint s = 0; s < size; ++s) {
		if (carved_flag[s] && from_open[s]) ++carved_reachable;
	}
	const uint water_unreachable = water_total - open_reached;

	/* Towns the importer nudged onto land, and towns still standing in water. */
	uint towns_on_water = 0;
	for (const Town *town : Town::Iterate()) {
		if (town->xy != INVALID_TILE && IsWaterTile(town->xy)) ++towns_on_water;
	}

	/* Number of separate water bodies, to catch a river shredded into ponds. */
	uint water_bodies = 0, water_isolated = 0;
	{
		std::vector<uint8_t> seen(size, 0);
		std::vector<TileIndex> queue;
		queue.reserve(1 << 16);
		for (uint s = 0; s < size; ++s) {
			if (seen[s] || !water[s]) continue;
			++water_bodies;
			queue.clear();
			queue.push_back(TileIndex(static_cast<TileIndex::BaseType>(s)));
			seen[s] = 1;
			uint body = 0;
			while (!queue.empty()) {
				TileIndex cur = queue.back();
				queue.pop_back();
				++body;
				for (const auto &d : kHsbNeighbours) {
					TileIndex n = cur + TileDiffXY(d[0], d[1]);
					if (!IsValidTile(n) || seen[n.base()] || !water[n.base()]) continue;
					seen[n.base()] = 1;
					queue.push_back(n);
				}
			}
			if (body == 1) ++water_isolated;
		}
	}

	Debug(misc, 0, "hsb: audit: towns={} towns_on_water={} industries={} water={} sea_fraction={} "
		"water_bodies={} isolated_water_tiles={} water_unreachable={} carved={} carved_reachable={}",
		towns_placed, towns_on_water, industries_placed, water_total,
		size == 0 ? 0.0 : static_cast<double>(water_total) / static_cast<double>(size),
		water_bodies, water_isolated, water_unreachable, carved_total, carved_reachable);
}

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
	if (!std::filesystem::path(rivers_path).is_absolute()) {
		Debug(misc, 0, "hsb: rivers path must be absolute");
		return false;
	}
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
				if (!wps[i].is_array() || wps[i].size() != 2 || !wps[i + 1].is_array() || wps[i + 1].size() != 2
						|| !wps[i].at(0).is_number() || !wps[i].at(1).is_number()
						|| !wps[i + 1].at(0).is_number() || !wps[i + 1].at(1).is_number()) {
					Debug(misc, 0, "hsb: river waypoints must be numeric [lat, lon] pairs");
					return false;
				}
				double lat0 = wps[i].at(0).get<double>(), lon0 = wps[i].at(1).get<double>();
				double lat1 = wps[i + 1].at(0).get<double>(), lon1 = wps[i + 1].at(1).get<double>();
				if (!HsbCoordinateInBounds(g_hsb_bounds, lat0, lon0) || !HsbCoordinateInBounds(g_hsb_bounds, lat1, lon1)) {
					Debug(misc, 0, "hsb: river waypoint is outside finite bounds");
					return false;
				}
				double dist = std::max(std::abs(lat1 - lat0), std::abs(lon1 - lon0));
				uint steps = static_cast<uint>(std::lround(dist / 0.002)) + 1;
				/* Walk the segment 4-connected from the previous sample, so the
				 * carved water is one body instead of a chain of corner-touching
				 * stamps that a ship cannot enter. */
				TileIndex prev_water = TileIndex{};
				bool have_water = false;
				for (uint s = 0; s <= steps; ++s) {
					double lat = lat0 + (lat1 - lat0) * s / steps;
					double lon = lon0 + (lon1 - lon0) * s / steps;
					TileIndex c = HsbTileFor(lat, lon);
					if (have_water) {
						HsbWalkSteps(prev_water, c, [&](TileIndex centre) { HsbPaintBrush(centre, width, sea, carved); });
					} else {
						HsbPaintBrush(c, width, sea, carved);
					}
					prev_water = c;
					have_water = true;
				}
			}
		}
	} catch (nlohmann::json::exception &e) {
		Debug(misc, 0, "hsb: invalid river data: {}", e.what());
		return false;
	}
	Debug(misc, 0, "hsb: carved {} water tiles", carved);
	HsbConnectCarvedWater();
	FixSlopes();
	MarkWholeScreenDirty();
	return true;
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
			if (name.empty() || name.size() >= 32 || !names.insert(name).second || population == 0 || population > std::numeric_limits<uint>::max()
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

} // namespace

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
	std::string currency;
	int start_year;
	int heightmap_height;
	int snow_line_height;
	int competitors;
	int64_t industry_search_radius;
	int64_t max_loan;
	uint32_t generation_seed;
	try {
		if (!manifest.at("heightmap").is_string() || !manifest.at("towns").is_string()
				|| !manifest.at("industries").is_string() || !manifest.at("start_year").is_number_integer()
				|| !manifest.at("save").is_string()) {
			Debug(misc, 0, "hsb: manifest fields have invalid types");
			return false;
		}
		heightmap = manifest.at("heightmap").get<std::string>();
		towns = manifest.at("towns").get<std::string>();
		industries_path = manifest.at("industries").get<std::string>();
		start_year = manifest.at("start_year").get<int>();
		save = manifest.at("save").get<std::string>();
		if (manifest.contains("heightmap_height") && !manifest.at("heightmap_height").is_null()
				&& !manifest.at("heightmap_height").is_number_integer()) return false;
		if (manifest.contains("seed") && !manifest.at("seed").is_null()
				&& !manifest.at("seed").is_number_unsigned()) return false;
		if (manifest.contains("industry_search_radius") && !manifest.at("industry_search_radius").is_null()
				&& !manifest.at("industry_search_radius").is_number_integer()) return false;
		if (manifest.contains("heightmap_rotation") && !manifest.at("heightmap_rotation").is_null()
				&& !manifest.at("heightmap_rotation").is_string()) return false;
		if (manifest.contains("snow_line_height") && !manifest.at("snow_line_height").is_null()
				&& !manifest.at("snow_line_height").is_number_integer()) return false;
		if (manifest.contains("max_loan") && !manifest.at("max_loan").is_null()
				&& !manifest.at("max_loan").is_number_integer()) return false;
		if (manifest.contains("competitors") && !manifest.at("competitors").is_null()
				&& !manifest.at("competitors").is_number_integer()) return false;
		if (manifest.contains("currency") && !manifest.at("currency").is_null()
				&& !manifest.at("currency").is_string()) return false;
		heightmap_height = !manifest.contains("heightmap_height") || manifest.at("heightmap_height").is_null() ? 12 : manifest.at("heightmap_height").get<int>();
		snow_line_height = !manifest.contains("snow_line_height") || manifest.at("snow_line_height").is_null() ? 0 : manifest.at("snow_line_height").get<int>();
		max_loan = !manifest.contains("max_loan") || manifest.at("max_loan").is_null() ? 0 : manifest.at("max_loan").get<int64_t>();
		competitors = !manifest.contains("competitors") || manifest.at("competitors").is_null() ? 0 : manifest.at("competitors").get<int>();
		generation_seed = !manifest.contains("seed") || manifest.at("seed").is_null() ? 20250924 : manifest.at("seed").get<uint32_t>();
		industry_search_radius = !manifest.contains("industry_search_radius") || manifest.at("industry_search_radius").is_null() ? 32 : manifest.at("industry_search_radius").get<int64_t>();
		rotation = !manifest.contains("heightmap_rotation") || manifest.at("heightmap_rotation").is_null() ? "clockwise" : manifest.at("heightmap_rotation").get<std::string>();
		currency = !manifest.contains("currency") || manifest.at("currency").is_null() ? "" : manifest.at("currency").get<std::string>();
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
			|| snow_line_height < 0 || snow_line_height > static_cast<int>(MAX_TILE_HEIGHT)
			|| industry_search_radius < 1 || industry_search_radius > 512 || generation_seed == UINT32_MAX
			|| max_loan < 0 || max_loan > 4'000'000'000 || competitors < 0 || competitors > 15) {
		Debug(misc, 0, "hsb: manifest year, height, snow line, industry radius, loan, or competitors is out of range");
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
	/* Loan ceiling. 0 keeps the engine default; a scenario's newgame settings win
	 * over the player's config, so anything not pinned here is inherited from the
	 * builder's own openttd.cfg - which is machine state, not intent. */
	if (max_loan > 0) _settings_newgame.difficulty.max_loan = static_cast<uint32_t>(max_loan);
	/* Company slots. A scenario with zero slots drops the player onto the map
	 * as a spectator, so the count is pinned rather than inherited. */
	_settings_newgame.difficulty.max_no_competitors = static_cast<uint8_t>(competitors);
	if (competitors > 0) _settings_newgame.difficulty.competitors_interval = 1;
	/* Currency. Only pinned when the manifest asks for one: _currency_specs is the
	 * engine's table (0 = GBP, 2 = EUR), and an unknown code is a build error
	 * rather than a silent fallback, because a wrong currency in every player's
	 * save is invisible to every headless gate. */
	if (!currency.empty()) {
		bool found = false;
		for (uint8_t i = 0; i < CURRENCY_END; i++) {
			if (currency == _currency_specs[i].code) {
				_settings_newgame.locale.currency = i;
				found = true;
				break;
			}
		}
		if (!found) {
			Debug(misc, 0, "hsb: unknown currency code '{}'", currency);
			return false;
		}
	}
	/* Selective servicing OFF: with it on, industries never deliver to a station
	 * no vehicle has loaded at yet - a permanent deadlock for every new
	 * full-load route. */
	_settings_newgame.order.selectgoods = false;

	/* 1. Terrain: same as the GUI heightmap flow (dimensions from the PNG). */
	uint dim_x = 0, dim_y = 0;
	if (!GetHeightmapDimensions(DFT_HEIGHTMAP_PNG, BasenameOf(heightmap), &dim_x, &dim_y)) {
		Debug(misc, 0, "hsb: cannot read heightmap {}", heightmap);
		return false;
	}
	if (!HsbIsPowerOfTwo(dim_x, MIN_MAP_SIZE, MAX_MAP_SIZE)
			|| !HsbIsPowerOfTwo(dim_y, MIN_MAP_SIZE, MAX_MAP_SIZE) || dim_x != dim_y) {
		Debug(misc, 0, "hsb: heightmap must be square powers of 2 in 64..4096, got {}x{}", dim_x, dim_y);
		return false;
	}
	/* Newgame settings: GenerateWorld resets _settings_game from these, so
	 * rotation/height must be set here (not on _settings_game directly). */
	_settings_newgame.game_creation.map_x = static_cast<uint8_t>(std::bit_width(dim_x) - 1);
	_settings_newgame.game_creation.map_y = static_cast<uint8_t>(std::bit_width(dim_y) - 1);
	_settings_newgame.game_creation.heightmap_rotation = g_hsb_counter_clockwise ? HM_COUNTER_CLOCKWISE : HM_CLOCKWISE;
	_settings_newgame.game_creation.heightmap_height = static_cast<uint8_t>(heightmap_height);
	/* snow_line_height 0 means "derive from snow_coverage"; the engine clamps it
	 * to at least 2 (snow on the highest land only). */
	if (snow_line_height > 0) _settings_newgame.game_creation.snow_line_height = static_cast<uint8_t>(std::max(snow_line_height, 2));
	_file_to_saveload.name = BasenameOf(heightmap);
	_file_to_saveload.ftype = FiosType{FT_HEIGHTMAP, DFT_HEIGHTMAP_PNG};
	Debug(misc, 0, "hsb: generating {}x{} terrain, height {} rotation {}", dim_x, dim_y, heightmap_height, rotation);
	GenerateWorld(GWM_HEIGHTMAP, dim_x, dim_y);

	/* Date FIRST, before any content: industries and closures track unserved time
	 * from creation, so building content in year 1 and only then jumping to the
	 * scenario start year makes everything look decades stale and triggers rapid
	 * closures the moment the player opens the save. */
	SetStartingYear(TimerGameCalendar::Year(start_year));

	/* 1b. Waters: carve rivers/estuaries from rivers.json (before towns). */
	if (!HsbCarveWaters(manifest)) return false;

	/* 2. Towns + industries, bracketed in gameloop scope so NewGRF persistent
	 * storage writes actually commit to the save (see HsbGameloopScope). */
	HsbGameloopScope gameloop_scope;
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
			if (type.empty() || !HsbCoordinateInBounds(g_hsb_bounds, lat, lon)) {
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

	/* 4. Audit, then save. The audit runs before the save so a gate can assert on
	 * the numbers even when the save itself fails. */
	HsbAuditBuild(static_cast<uint>(towns_placed), placed);
	if (SaveOrLoad(save, SLO_SAVE, DFT_GAME_FILE, SCENARIO_DIR, false) != SL_OK) {
		Debug(misc, 0, "hsb: save failed");
		return false;
	}
	Debug(misc, 0, "hsb: saved {}", save);
	return true;
}
