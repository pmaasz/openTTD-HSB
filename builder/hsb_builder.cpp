/**
 * HSB headless scenario builder.
 *
 * Replays the Scenario Editor workflow deterministically, without GUI input:
 * heightmap -> start year -> waters (rivers/estuaries) -> town JSON import ->
 * industry placement -> network (roads, rail, airports, docks, a starting
 * fleet) -> audit -> save. Every step reuses the exact engine functions the GUI
 * calls.
 *
 * Driven by the HSB_BUILD environment variable (JSON manifest path).
 * Geographic bounds come from the manifest ("bounds"), so the same binary
 * builds any region - no recompilation per map. The network routers read their
 * tunables from HSB_TUNE_* instead, for the same reason.
 *
 * This file is part of openTTD-HSB.
 * Licensed under the GNU General Public License, version 2.
 * See LICENSE.md for details.
 */

#include "stdafx.h"

#include "hsb_builder.h"
#include "hsb_builder_math.h"
#include "hsb_builder_net.h"

#include "3rdparty/nlohmann/json.hpp"
#include "airport.h"
#include "command_func.h"
#include "company_base.h"
#include "company_func.h"
#include "company_type.h"
#include "core/backup_type.hpp"
#include "currency_func.h"
#include "debug.h"
#include "depot_base.h"
#include "direction_func.h"
#include "engine_base.h"
#include "engine_func.h"
#include "fileio_type.h"
#include "genworld.h"
#include "heightmap.h"
#include "industry_cmd.h"
#include "industrytype.h"
#include "landscape_cmd.h"
#include "map_func.h"
#include "newgrf_roadstop.h"
#include "newgrf_station.h"
#include "newgrf_storage.h"
#include "openttd.h"
#include "order_cmd.h"
#include "rail.h"
#include "rail_cmd.h"
#include "rail_map.h"
#include "road_cmd.h"
#include "road_func.h"
#include "road_map.h"
#include "road_type.h"
#include "saveload/saveload.h"
#include "saveload/saveload_func.h"
#include "settings_type.h"
#include "signs_base.h"
#include "station_base.h"
#include "station_cmd.h"
#include "station_type.h"
#include "strings_func.h"
#include "table/strings.h"
#include "terraform_cmd.h"
#include "tile_map.h"
#include "tilearea_type.h"
#include "tilearea_spiral.h"
#include "timer/timer_game_calendar.h"
#include "town.h"
#include "town_map.h"
#include "tree_cmd.h"
#include "tree_map.h"
#include "tunnelbridge_cmd.h"
#include "tunnelbridge_map.h"
#include "vehicle_base.h"
#include "vehicle_cmd.h"
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

/* Defined in company_cmd.cpp; declared extern in openttd.cpp for the same reason. */
extern Company *DoStartupNewCompany(bool is_ai, CompanyID company = CompanyID::Invalid());

namespace {

HsbBounds g_hsb_bounds;

/** Towns use the same spiral fallback radius as LoadTownData; industries are bigger. */
uint g_hsb_industry_radius = 32;
bool g_hsb_counter_clockwise = false;

/** Network-phase settings, all validated in HsbParseNetworkSettings. */
struct HsbNetworkSettings {
	std::string roads;
	std::string autobahnen;
	std::string rail;
	std::string airports;
	std::string harbours;
	std::string stations;
	bool with_rail = true;
	bool with_fleet = true;
	/** Construction difficulty settings, pinned rather than inherited. See the
	 *  HsbBuildScenario comment that explains what each one gates. */
	bool build_on_slopes = true;
	bool autoslope = true;
	Money money = 0;
	uint bus_towns = static_cast<uint>(kHsbBusTowns.fallback);
	uint trains = static_cast<uint>(kHsbTrains.fallback);
	/** The borrowed infrastructure company, or COMPANY_NO_COMPANY when the network
	 * is off. Set once DoStartupNewCompany has run. */
	CompanyID network_company = CompanyID::Invalid();
};
HsbNetworkSettings g_hsb_net;

/** Real station position from stations.json, keyed by town name. */
struct HsbStationAnchor {
	double lat = 0.0;
	double lon = 0.0;
	std::string name;
};
std::map<std::string, HsbStationAnchor> g_hsb_station_anchors;

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

/* ---------------------------------------------------------------------------
 * Router tunables, read once from the environment.
 *
 * These numbers decide how the trunk road comes out, and each of them used to
 * be a value nobody could re-run with a different one: a constant is fixed when
 * the engine is compiled, so the only way to try 64 tiles per chunk instead of 48
 * was to edit this file, rebuild, wait out the build, read the log - and the next
 * value needed all of that again. The defaults are the values that were compiled
 * in before any of this existed, so a build with an empty environment routes
 * exactly the road it always routed. Same idea as HSB_FAST in openttd.cpp, and
 * for the same reason: a test aid that is a no-op unless the environment asks
 * for it.
 * ------------------------------------------------------------------------- */

/** One tuning integer from the environment, or the default, with the reason logged. */
int HsbTuneIntEnv(const char *name, int def, int lo, int hi)
{
	const char *raw = std::getenv(name);
	bool clamped = false;
	const int value = HsbTuneInt(raw, def, lo, hi, &clamped);
	if (raw == nullptr || *raw == '\0') return value;
	if (clamped) {
		Debug(Facility::Misc, Severity::Critical, "hsb: tune {} = {} is outside {}..{}, clamped to {}", name,
			std::strtol(raw, nullptr, 10), lo, hi, value);
	} else if (value == def && std::strtol(raw, nullptr, 10) == 0 && def != 0) {
		Debug(Facility::Misc, Severity::Critical, "hsb: tune {} = '{}' is not usable, using {}", name, raw, def);
	}
	return value;
}

/** How far a corner or a per-piece target may be nudged onto buildable ground. */
uint HsbRoadSnap() { static const int v = HsbTuneIntEnv("HSB_TUNE_ROAD_SNAP", 8, 1, 64); return static_cast<uint>(v); }

/** Longest straight run before a line turns, in tiles. HsbRouteRail chunks on the
 *  same number, so this one moves the rail as well as the road. */
int HsbRoadRun() { static const int v = HsbTuneIntEnv("HSB_TUNE_ROAD_RUN", 64, 1, 512); return v; }

/** How far the corner of a chunk may be moved perpendicular to its first leg.
 *  See HsbCornerFanOffsets for what the default ladder means and why the order
 *  is part of the result. */
const std::vector<int> &HsbCornerFan()
{
	static const std::vector<int> fan = HsbCornerFanOffsets(std::getenv("HSB_TUNE_CORNER_FAN"), 8);
	return fan;
}

/** Verbose trace of the first few failing links, so a failure can be read off the
 *  log instead of guessed at. Read on first use rather than in a static
 *  initialiser, so a build that never asks for a trace never touches the
 *  environment. 0 turns the trace off, which is what a parameter sweep wants:
 *  three stack dumps of unrelated chunks per build, in every log, is noise
 *  nobody reads. */
uint &HsbRoadTraceLeft()
{
	static uint left = static_cast<uint>(HsbTuneIntEnv("HSB_TUNE_ROAD_TRACE", 3, 0, 1000));
	return left;
}

/**
 * The one line that says which configuration produced this build.
 *
 * A parameter sweep lines up dozens of builds side by side, and the only thing
 * that tells them apart is what the builder said it was about to do before it
 * did it. A log that does not name its configuration cannot be compared with
 * another log, and comparing them is the entire reason these numbers exist.
 */
void HsbLogTuning()
{
	std::string fan_text;
	for (const int off : HsbCornerFan()) {
		if (!fan_text.empty()) fan_text += ",";
		fan_text += std::to_string(off);
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: tune ROAD_RUN={} ROAD_SNAP={} ROAD_TRACE={} CORNER_FAN={}",
		HsbRoadRun(), HsbRoadSnap(), HsbRoadTraceLeft(), fan_text);
}

/* ---------------------------------------------------------------------------
 * The road and rail types this world actually has.
 *
 * A NewGRF that brings its own rolling stock also brings its own rail type, and
 * with it the base RAILTYPE_RAIL stops being a valid parameter -
 * CmdBuildRailroadTrack segfaults on it (ValParamRailType reads through a null
 * spec). So the first type the world really has is picked once, here, and
 * everything uses that.
 * ------------------------------------------------------------------------- */

RoadType HsbRoadType() { return ROADTYPE_ROAD; }

RailType HsbRailType()
{
	static RailType chosen = RAILTYPE_RAIL;
	static bool done = false;
	if (done) return chosen;
	done = true;
	for (const RailType rt : GetRailTypes(false)) {
		if (ValParamRailType(rt)) { chosen = rt; break; }
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: rail type chosen {}; world offers {}, current company has {}",
		static_cast<uint>(chosen), static_cast<uint>(GetRailTypes(false).base()),
		Company::IsValidID(_current_company) ? static_cast<uint>(Company::Get(_current_company)->avail_railtypes.base()) : 0);
	return chosen;
}

/**
 * An airport type and layout that this world actually has.
 *
 * A NewGRF brings its own airport, so the base spec at index 0 can be unavailable
 * or have a different number of layouts, and CmdBuildAirport checks both
 * (`as->IsAvailable()`, `layout >= as->layouts.size()`). Asking for layout 1 of a
 * one-layout airport is refused on every tile of the map, which is why this asks
 * the spec rather than assuming.
 */
uint8_t HsbAirportType()
{
	static uint8_t chosen = 0;
	static bool done = false;
	if (done) return chosen;
	done = true;
	for (uint8_t t = 0; t < NUM_AIRPORTS; ++t) {
		const AirportSpec *as = AirportSpec::Get(t);
		if (as != nullptr && as->IsAvailable() && !as->layouts.empty()) { chosen = t; break; }
	}
	const AirportSpec *as = AirportSpec::Get(chosen);
	Debug(Facility::Misc, Severity::Critical, "hsb: airport type {} with {} layouts", chosen, as == nullptr ? 0 : as->layouts.size());
	return chosen;
}

uint8_t HsbAirportLayout()
{
	const AirportSpec *as = AirportSpec::Get(HsbAirportType());
	if (as == nullptr || as->layouts.empty()) return 0;
	return static_cast<uint8_t>(as->layouts.size() - 1);
}

static TileIndex HsbTileClamped(int x, int y)
{
	return TileXY(Clamp<int>(x, 0, Map::MaxX()), Clamp<int>(y, 0, Map::MaxY()));
}

/**
 * The human-readable reason a command was refused, or a placeholder.
 *
 * Not every refusal carries a message: a command that fails a plain precondition
 * returns a bare CMD_ERROR, whose StringID is INVALID_STRING_ID. GetString()
 * asserts on that and takes the process down, so a build that merely could not
 * place one industry died instead of logging which one. "no reason given" is the
 * honest answer, and it is what makes such a refusal countable.
 */
std::string HsbWhy(CommandCost res)
{
	const StringID why = res.GetErrorMessage();
	return why == INVALID_STRING_ID ? std::string("no reason given") : GetString(why);
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
 * The guard tests IsWaterTile, not IsTileType(TileType::Water). A tile that is
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
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest needs a bounds object with lat_min, lat_max, lon_min, and lon_max");
		return false;
	}
	const auto &b = manifest.at("bounds");
	try {
		if (!b.at("lat_min").is_number() || !b.at("lat_max").is_number()
				|| !b.at("lon_min").is_number() || !b.at("lon_max").is_number()) {
			Debug(Facility::Misc, Severity::Critical, "hsb: bounds values must be numbers");
			return false;
		}
		g_hsb_bounds.lat_min = b.at("lat_min").get<double>();
		g_hsb_bounds.lat_max = b.at("lat_max").get<double>();
		g_hsb_bounds.lon_min = b.at("lon_min").get<double>();
		g_hsb_bounds.lon_max = b.at("lon_max").get<double>();
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: bad bounds: {}", e.what());
		return false;
	}
	if (!(std::isfinite(g_hsb_bounds.lat_min) && std::isfinite(g_hsb_bounds.lat_max)
			&& std::isfinite(g_hsb_bounds.lon_min) && std::isfinite(g_hsb_bounds.lon_max))) {
		Debug(Facility::Misc, Severity::Critical, "hsb: bounds must be finite");
		return false;
	}
	if (!(g_hsb_bounds.lat_min < g_hsb_bounds.lat_max) || !(g_hsb_bounds.lon_min < g_hsb_bounds.lon_max)
			|| g_hsb_bounds.lat_min < -90.0 || g_hsb_bounds.lat_max > 90.0
			|| g_hsb_bounds.lon_min < -180.0 || g_hsb_bounds.lon_max > 180.0) {
		Debug(Facility::Misc, Severity::Critical, "hsb: bounds must be ordered WGS84 intervals");
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
		Debug(Facility::Misc, Severity::Critical, "hsb: no open water to connect the carved rivers to");
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
	Debug(Facility::Misc, Severity::Critical, "hsb: carved water bodies {}, already open {}, connected {}, unreachable {}",
		bodies, already, connected, gave_up);
	if (gave_up > 0) {
		Debug(Facility::Misc, Severity::Critical, "hsb: {} carved body/bodies are still closed: no open water within {} tiles, "
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
void HsbAuditNetwork();

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

	/* One machine-readable line for the terrain/water build; the network phase logs
	 * its own line right after, so a gate can assert on either without the other
	 * having run. */
	Debug(Facility::Misc, Severity::Critical, "hsb: audit: towns={} towns_on_water={} industries={} water={} sea_fraction={} "
		"water_bodies={} isolated_water_tiles={} water_unreachable={} carved={} carved_reachable={}",
		towns_placed, towns_on_water, industries_placed, water_total,
		size == 0 ? 0.0 : static_cast<double>(water_total) / static_cast<double>(size),
		water_bodies, water_isolated, water_unreachable, carved_total, carved_reachable);
	HsbAuditNetwork();
}

/** Number of 4-connected components of `is_target`, and the biggest one's size. */
template <typename Tpredicate>
uint HsbLargestComponent(const std::vector<uint8_t> &mask, Tpredicate is_target, uint &isolated)
{
	const size_t size = mask.size();
	std::vector<uint8_t> seen(size, 0);
	std::vector<TileIndex> queue;
	queue.reserve(1 << 16);
	isolated = 0;
	uint best = 0;
	for (uint s = 0; s < size; ++s) {
		if (seen[s] || !mask[s]) continue;
		queue.clear();
		queue.push_back(TileIndex(static_cast<TileIndex::BaseType>(s)));
		seen[s] = 1;
		uint component = 0;
		while (!queue.empty()) {
			const TileIndex cur = queue.back();
			queue.pop_back();
			++component;
			for (const auto &d : kHsbNeighbours) {
				TileIndex n = cur + TileDiffXY(d[0], d[1]);
				if (!IsValidTile(n) || seen[n.base()] || !mask[n.base()]) continue;
				seen[n.base()] = 1;
				queue.push_back(n);
			}
		}
		if (component == 1) ++isolated;
		if (component > best) best = component;
	}
	return best;
}

/** Flood fill over `mask` from one source, in place. Returns tiles reached. */
uint HsbFloodMask(const std::vector<uint8_t> &mask, std::vector<uint8_t> &reached, TileIndex start)
{
	std::vector<TileIndex> queue;
	queue.reserve(1 << 16);
	uint total = 0;
	if (!IsValidTile(start) || reached[start.base()] || !mask[start.base()]) return 0;
	reached[start.base()] = 1;
	queue.push_back(start);
	while (!queue.empty()) {
		const TileIndex cur = queue.back();
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
 * Measure what the network phase actually produced.
 *
 * A skeleton that only *looks* connected is worse than no skeleton at all: a
 * player follows a road to a town and cannot get there, and nothing in the build
 * log says so. The measures here are deliberately three different questions,
 * because they were three different answers to the same "is the network
 * finished?" and only one of them was the right one:
 *
 *  - `road_components` / `largest_road`: is the trunk in pieces?
 *  - `towns_on_network`: is any road within a town's radius on the biggest
 *    component? Cheap, and it passes for a trunk that stops two tiles short of
 *    the kerb, so it is not enough on its own.
 *  - `intercity_towns`: can a bus from the biggest town actually be driven to the
 *    others? A flood from the seed town over every road tile, town streets
 *    included. This is the number that decides whether the buses are intercity
 *    transport or a municipal shuttle.
 */
void HsbAuditNetwork()
{
	const size_t size = Map::Size();
	const auto is_road = [](TileIndex t) { return IsValidTile(t) && IsNormalRoadTile(t); };
	const auto is_rail = [](TileIndex t) { return IsValidTile(t) && IsPlainRailTile(t); };

	std::vector<uint8_t> road(size, 0), rail(size, 0);
	uint roads_total = 0, road_orphans = 0, rails_total = 0;
	for (uint s = 0; s < size; ++s) {
		TileIndex t(static_cast<TileIndex::BaseType>(s));
		if (is_road(t)) { road[s] = 1; ++roads_total; }
		if (is_rail(t)) { rail[s] = 1; ++rails_total; }
	}
	for (uint s = 0; s < size; ++s) {
		TileIndex t(static_cast<TileIndex::BaseType>(s));
		if (!is_road(t)) continue;
		bool neighbour = false;
		for (const auto &d : kHsbNeighbours) {
			if (is_road(t + TileDiffXY(d[0], d[1]))) { neighbour = true; break; }
		}
		if (!neighbour) ++road_orphans;
	}

	/* Component counts come from one label pass each: the number of components
	 * is the number of labels, so a flood per component would be quadratic. */
	auto label_components = [&](const std::vector<uint8_t> &mask, uint &largest) {
		std::vector<uint32_t> label(size, 0);
		uint32_t next = 0;
		uint best = 0;
		std::vector<TileIndex> queue;
		queue.reserve(1 << 16);
		for (uint s = 0; s < size; ++s) {
			TileIndex start(static_cast<TileIndex::BaseType>(s));
			if (label[s] || !mask[s]) continue;
			++next;
			uint count = 0;
			queue.clear();
			queue.push_back(start);
			label[s] = next;
			while (!queue.empty()) {
				const TileIndex cur = queue.back();
				queue.pop_back();
				++count;
				for (const auto &d : kHsbNeighbours) {
					TileIndex n = cur + TileDiffXY(d[0], d[1]);
					if (!IsValidTile(n) || label[n.base()] || !mask[n.base()]) continue;
					label[n.base()] = next;
					queue.push_back(n);
				}
			}
			if (count > best) best = count;
		}
		largest = best;
		return next;
	};
	uint largest_road = 0, largest_rail = 0;
	const uint road_components = label_components(road, largest_road);
	const uint rail_components = label_components(rail, largest_rail);

	/* The mask of the largest road component, so "is this town on the network"
	 * is one lookup rather than a flood per town. */
	std::vector<uint8_t> road_on_largest(size, 0);
	{
		std::vector<uint8_t> seen(size, 0);
		uint best = 0;
		std::vector<TileIndex> queue;
		queue.reserve(1 << 16);
		for (uint s = 0; s < size; ++s) {
			TileIndex start(static_cast<TileIndex::BaseType>(s));
			if (seen[s] || !road[s]) continue;
			queue.clear();
			queue.push_back(start);
			seen[s] = 1;
			uint component = 0;
			std::vector<TileIndex> members;
			while (!queue.empty()) {
				const TileIndex cur = queue.back();
				queue.pop_back();
				++component;
				members.push_back(cur);
				for (const auto &d : kHsbNeighbours) {
					TileIndex n = cur + TileDiffXY(d[0], d[1]);
					if (!IsValidTile(n) || seen[n.base()] || !road[n.base()]) continue;
					seen[n.base()] = 1;
					queue.push_back(n);
				}
			}
			if (component > best) {
				best = component;
				for (TileIndex t : members) road_on_largest[t.base()] = 1;
			}
		}
	}

	/* Radius 8 covers the roads the game generates inside a town. */
	const int kTownRadius = static_cast<int>(kHsbTownRoadRadius.fallback);
	uint towns_total = 0, towns_on_network = 0, towns_reported = 0;
	for (const Town *town : Town::Iterate()) {
		if (town->xy == INVALID_TILE) continue;
		++towns_total;
		bool on_network = false;
		for (int dy = -kTownRadius; dy <= kTownRadius && !on_network; ++dy) {
			for (int dx = -kTownRadius; dx <= kTownRadius; ++dx) {
				if (road_on_largest[HsbTileClamped(TileX(town->xy) + dx, TileY(town->xy) + dy).base()]) {
					on_network = true;
					break;
				}
			}
		}
		if (on_network) {
			++towns_on_network;
			continue;
		}
		/* A town on an island is not a defect, and the difference matters when
		 * reading "118/124": say which towns are off the network, and whether
		 * any land is between them and the mainland. */
		if (towns_reported < static_cast<uint>(kHsbAuditReportLimit.fallback)) {
			uint land_neighbours = 0;
			for (int dy = -6; dy <= 6; ++dy) {
				for (int dx = -6; dx <= 6; ++dx) {
					const TileIndex probe = HsbTileClamped(TileX(town->xy) + dx, TileY(town->xy) + dy);
					if (IsValidTile(probe) && !IsWaterTile(probe) && road_on_largest[probe.base()]) ++land_neighbours;
				}
			}
			Debug(Facility::Misc, Severity::Critical, "hsb: town not on the road network: {} ({},{}) pop {} nearby network tiles {}",
				town->name, TileX(town->xy), TileY(town->xy), town->cache.population, land_neighbours);
			++towns_reported;
		}
	}

	/* "Can you drive from one town to another" - the question that decides whether
	 * the buses are intercity transport or a municipal shuttle. */
	uint intercity_towns = 0, intercity_from = 0;
	{
		const Town *seed = nullptr;
		for (const Town *t : Town::Iterate()) {
			if (t->xy == INVALID_TILE) continue;
			if (seed == nullptr || t->cache.population > seed->cache.population) seed = t;
		}
		if (seed != nullptr) {
			intercity_from = seed->cache.population;
			std::vector<uint8_t> reach(size, 0);
			/* Start from every road tile in the seed town, not one: a town centre
			 * is not itself a road tile, and a single start can be in a dead-end
			 * pocket. */
			for (int dy = -10; dy <= 10; ++dy) {
				for (int dx = -10; dx <= 10; ++dx) {
					const TileIndex t = HsbTileClamped(TileX(seed->xy) + dx, TileY(seed->xy) + dy);
					if (!is_road(t) || reach[t.base()]) continue;
					reach[t.base()] = 1;
					HsbFloodMask(road, reach, t);
				}
			}
			for (const Town *t : Town::Iterate()) {
				if (t->xy == INVALID_TILE) continue;
				for (int dy = -10; dy <= 10; ++dy) {
					bool found = false;
					for (int dx = -10; dx <= 10; ++dx) {
						if (reach[HsbTileClamped(TileX(t->xy) + dx, TileY(t->xy) + dy).base()]) { found = true; break; }
					}
					if (found) { ++intercity_towns; break; }
				}
			}
		}
	}

	/* Stations, depots, airports, docks and the fleet. */
	uint stations = 0, stations_with_rail = 0, bus_stops = 0, docks = 0, airports = 0;
	for (const Station *st : Station::Iterate()) {
		++stations;
		if (st->facilities.Test(StationFacility::Train)) ++stations_with_rail;
		if (st->facilities.Test(StationFacility::BusStop)) ++bus_stops;
		if (st->facilities.Test(StationFacility::Dock)) ++docks;
		if (st->facilities.Test(StationFacility::Airport)) ++airports;
	}
	uint depots = 0;
	for ([[maybe_unused]] const Depot *d : Depot::Iterate()) ++depots;
	uint vehicles = 0, primaries = 0, vehicles_routed = 0, vehicles_parked = 0;
	for (const Vehicle *v : Vehicle::Iterate()) {
		++vehicles;
		if (!v->IsPrimaryVehicle()) continue;
		++primaries;
		if (v->orders != nullptr && v->GetNumOrders() > 0) ++vehicles_routed;
		if (v->IsStoppedInDepot()) ++vehicles_parked;
	}

	Debug(Facility::Misc, Severity::Critical, "hsb: network audit: roads={} orphan_roads={} road_components={} largest_road={} "
		"towns={} towns_on_network={} rail={} rail_components={} largest_rail={} "
		"stations={} stations_with_rail={} bus_stops={} depots={} airports={} docks={} "
		"vehicles={} consists={} vehicles_routed={} vehicles_parked={}",
		roads_total, road_orphans, road_components, largest_road,
		towns_total, towns_on_network, rails_total, rail_components, largest_rail,
		stations, stations_with_rail, bus_stops, depots, airports, docks,
		vehicles, primaries, vehicles_routed, vehicles_parked);
	Debug(Facility::Misc, Severity::Critical, "hsb: intercity road: a bus from the largest town (pop {}) reaches {} of {} towns",
		intercity_from, intercity_towns, towns_total);

	/* Where the gap is: for the ten biggest towns, how far the nearest corridor is.
	 *
	 * "Nearest" is any road tile, not a town-owned one. The editor never
	 * simulates, so a freshly imported town has no streets of its own and a
	 * town-street search reports 999 for every town on the map - a number that
	 * says "the diagnostic is measuring the wrong thing" rather than "the trunk
	 * never comes near". Small means a missing stub; large means the corridor
	 * does not reach the town at all. */
	{
		std::vector<const Town *> big;
		for (const Town *t : Town::Iterate()) {
			if (t->xy == INVALID_TILE) continue;
			big.push_back(t);
		}
		std::sort(big.begin(), big.end(), [](const Town *a, const Town *b) { return a->cache.population > b->cache.population; });
		for (size_t i = 0; i < big.size() && i < 10; ++i) {
			const Town *t = big[i];
			int best = -1;
			for (int r = 1; r <= 64 && best < 0; ++r) {
				for (int dy = -r; dy <= r; ++dy) {
					for (int dx = -r; dx <= r; ++dx) {
						if (HsbChebyshev(dx, dy) != r) continue;
						const TileIndex c = HsbTileClamped(TileX(t->xy) + dx, TileY(t->xy) + dy);
						if (IsValidTile(c) && (IsNormalRoadTile(c) || IsLevelCrossing(c))) { best = r; break; }
					}
				}
			}
			Debug(Facility::Misc, Severity::Critical, "hsb: nearest road to {} ({},{}) is {} tiles away (pop {})",
				t->name, TileX(t->xy), TileY(t->xy), best, t->cache.population);
		}
	}
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
		Debug(Facility::Misc, Severity::Critical, "hsb: bad rivers path: {}", e.what());
		return false;
	}
	if (rivers_path.empty()) return true;
	if (!std::filesystem::path(rivers_path).is_absolute()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: rivers path must be absolute");
		return false;
	}
	std::ifstream river_file(rivers_path);
	if (!river_file) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot open {}", rivers_path);
		return false;
	}
	nlohmann::json rivers;
	try {
		river_file >> rivers;
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: rivers parse error: {}", e.what());
		return false;
	}
	if (!rivers.is_array()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: rivers root must be an array");
		return false;
	}

	uint carved = 0;
	try {
		for (const auto &seg : rivers) {
			if (!seg.is_object() || !seg.contains("name") || !seg.at("name").is_string()
					|| !seg.contains("class") || !seg.at("class").is_string()
					|| !seg.contains("width") || !seg.at("width").is_number_integer()
					|| !seg.contains("waypoints") || !seg.at("waypoints").is_array()) {
				Debug(Facility::Misc, Severity::Critical, "hsb: invalid river segment schema");
				return false;
			}
			const std::string class_name = seg.at("class").get<std::string>();
			if (class_name != "river" && class_name != "sea") {
				Debug(Facility::Misc, Severity::Critical, "hsb: river class must be river or sea");
				return false;
			}
			const bool sea = class_name == "sea";
			const int width = seg.at("width").get<int>();
			if (width < 1 || width > 3) {
				Debug(Facility::Misc, Severity::Critical, "hsb: river width must be 1..3");
				return false;
			}
			const auto &wps = seg.at("waypoints");
			if (wps.size() < 2) {
				Debug(Facility::Misc, Severity::Critical, "hsb: river '{}' needs at least two waypoints", seg.at("name").get<std::string>());
				return false;
			}
			for (size_t i = 0; i + 1 < wps.size(); ++i) {
				if (!wps[i].is_array() || wps[i].size() != 2 || !wps[i + 1].is_array() || wps[i + 1].size() != 2
						|| !wps[i].at(0).is_number() || !wps[i].at(1).is_number()
						|| !wps[i + 1].at(0).is_number() || !wps[i + 1].at(1).is_number()) {
					Debug(Facility::Misc, Severity::Critical, "hsb: river waypoints must be numeric [lat, lon] pairs");
					return false;
				}
				double lat0 = wps[i].at(0).get<double>(), lon0 = wps[i].at(1).get<double>();
				double lat1 = wps[i + 1].at(0).get<double>(), lon1 = wps[i + 1].at(1).get<double>();
				if (!HsbCoordinateInBounds(g_hsb_bounds, lat0, lon0) || !HsbCoordinateInBounds(g_hsb_bounds, lat1, lon1)) {
					Debug(Facility::Misc, Severity::Critical, "hsb: river waypoint is outside finite bounds");
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
		Debug(Facility::Misc, Severity::Critical, "hsb: invalid river data: {}", e.what());
		return false;
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: carved {} water tiles", carved);
	HsbConnectCarvedWater();
	FixSlopes();
	MarkWholeScreenDirty();
	return true;
}

bool HsbValidateTownData(const std::string &path, size_t &town_count)
{
	std::ifstream input(path);
	if (!input) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot open towns data {}", path);
		return false;
	}
	nlohmann::json towns;
	try {
		input >> towns;
		if (!towns.is_array() || towns.empty()) {
			Debug(Facility::Misc, Severity::Critical, "hsb: towns root must be a nonempty array");
			return false;
		}
		std::set<std::string> names;
		for (const auto &town : towns) {
			if (!town.is_object() || !town.contains("name") || !town.at("name").is_string()
					|| !town.contains("population") || !town.at("population").is_number_integer()
					|| !town.contains("city") || !town.at("city").is_boolean()
					|| !town.contains("x") || !town.at("x").is_number()
					|| !town.contains("y") || !town.at("y").is_number()) {
				Debug(Facility::Misc, Severity::Critical, "hsb: invalid town entry schema");
				return false;
			}
			const std::string name = town.at("name").get<std::string>();
			const uint64_t population = town.at("population").get<uint64_t>();
			const double x = town.at("x").get<double>();
			const double y = town.at("y").get<double>();
			if (name.empty() || name.size() >= 32 || !names.insert(name).second || population == 0 || population > std::numeric_limits<uint>::max()
					|| !std::isfinite(x) || !std::isfinite(y) || x <= 0.0 || x >= 1.0 || y <= 0.0 || y >= 1.0) {
				Debug(Facility::Misc, Severity::Critical, "hsb: invalid town entry '{}'", name);
				return false;
			}
		}
		town_count = towns.size();
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: invalid towns data: {}", e.what());
		return false;
	}
	return true;
}

/* ---------------------------------------------------------------------------
 * The network phase: roads, rail, airports, docks, and a fleet that moves.
 *
 * Everything above builds *content*: terrain, water, towns, industries. What
 * makes a map feel like a country that is already running - and what a manual
 * sends a new player to do in their first ten minutes - is a transport network
 * with rolling stock on it, so that is what this builds.
 *
 * Every sub-phase is opt-in per input file, so a manifest that names only a
 * heightmap and towns builds exactly what it built before any of this existed.
 *
 * Two rules learned elsewhere in this builder and applied up front:
 *  - Commands run with DoCommandFlag::Execute and nothing else. Auto means *test*,
 *    and a tested build refuses the site the test just approved - which is how
 *    the rail bridge, the station platforms and four airports went missing in the
 *    sibling builder.
 *  - An order to a station or depot is checked against its owner, so the company
 *    that owns the network has to own the fleet. That is why the infrastructure
 *    company survives into the save, and why a manifest that builds the network
 *    must leave a free company slot for the player.
 * ------------------------------------------------------------------------- */

/** Refusal reasons, counted. "190 segments failed" says nothing; the reasons with
 *  their counts say whether the network can be built at all, and which structure
 *  is missing. */
std::map<StringID, uint> g_hsb_road_failures;
std::map<std::string, uint> g_hsb_rail_failures;

/** The tally key for a refusal that carried no message at all. Counted rather than
 *  dropped: a command that fails a bare precondition returns CMD_ERROR, whose
 *  StringID is INVALID_STRING_ID, and skipping those is how "the road command
 *  refuses every tile because it will not name a town" produced a network with
 *  zero roads and a log with nothing in it. */
static constexpr StringID kNoReason = INVALID_STRING_ID;

void HsbNoteRoadFailure(CommandCost res, const char *what, TileIndex t)
{
	const StringID why = res.GetErrorMessage();
	if (g_hsb_road_failures.find(why) == g_hsb_road_failures.end()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: first road {} refusal: ({},{}) tile type {} slope {}: {}", what,
			TileX(t), TileY(t), GetTileType(t), GetTileSlope(t),
			why == kNoReason ? "<no reason given>" : GetString(why));
	}
	g_hsb_road_failures[why]++;
}

void HsbNoteRailFailure(CommandCost res, TileIndex t, const char *from = "?")
{
	const StringID why = res.GetErrorMessage();
	/* Why a tile was refused is only half the story: the same message means
	 * something different on water than on a junction. Counting both is what
	 * turns thousands of identical lines of log into one number worth acting on. */
	const std::string key = std::string(from) + ": " +
		(why == kNoReason ? std::string("<no reason given>") : GetString(why)) +
		" [tile type " + std::to_string(to_underlying(GetTileType(t))) + "]";
	if (g_hsb_rail_failures.find(key) == g_hsb_rail_failures.end()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: first rail refusal at ({},{}) type {} slope {}: {}",
			TileX(t), TileY(t), GetTileType(t), GetTileSlope(t),
			why == kNoReason ? "<no reason given>" : GetString(why));
	}
	g_hsb_rail_failures[key]++;
}

/** Whether the manifest names any network input at all. */
bool HsbNetworkIsRequested()
{
	return !g_hsb_net.roads.empty() || !g_hsb_net.autobahnen.empty() || !g_hsb_net.rail.empty() ||
	       !g_hsb_net.airports.empty() || !g_hsb_net.harbours.empty();
}

/**
 * Read one optional absolute input path.
 *
 * Absent and explicit null both mean "this phase is off"; an empty string is
 * rejected rather than treated as absent, because a script that builds a path
 * and gets it wrong should say so rather than silently skip a phase.
 */
bool HsbOptionalPath(const nlohmann::json &manifest, const char *key, std::string &out)
{
	if (!manifest.contains(key) || manifest.at(key).is_null()) return true;
	if (!manifest.at(key).is_string()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest '{}' must be a string path or null", key);
		return false;
	}
	const std::string path = manifest.at(key).get<std::string>();
	if (path.empty()) return true;
	if (!std::filesystem::path(path).is_absolute() || !HsbSafeFilename(BasenameOf(path))) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest '{}' must be an absolute path to a plain filename", key);
		return false;
	}
	out = path;
	return true;
}

/** Read one optional integer, range-checked against a documented table. */
bool HsbOptionalRange(const nlohmann::json &manifest, const char *key, const HsbRange &range, int64_t &out)
{
	out = range.fallback;
	if (!manifest.contains(key) || manifest.at(key).is_null()) return true;
	if (!manifest.at(key).is_number_integer()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest '{}' must be an integer or null", key);
		return false;
	}
	const int64_t value = manifest.at(key).get<int64_t>();
	if (!HsbInRange(range, value)) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest '{}' is {}, want {}..{}", key, value, range.lo, range.hi);
		return false;
	}
	out = value;
	return true;
}

/** Read one optional boolean. */
bool HsbOptionalBool(const nlohmann::json &manifest, const char *key, bool &out)
{
	if (!manifest.contains(key) || manifest.at(key).is_null()) return true;
	if (!manifest.at(key).is_boolean()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest '{}' must be true, false or null", key);
		return false;
	}
	out = manifest.at(key).get<bool>();
	return true;
}

/**
 * Validate and read every network key, before a byte of map is generated.
 *
 * The whole point of validating here rather than at the call site is that the
 * failures are cheap to find: a relative path, a waypoint outside the bounds or
 * a bus count of 9999 is one line in a log at second one, instead of an hour of
 * routing and then a refusal to save.
 */
bool HsbParseNetworkSettings(const nlohmann::json &manifest, int competitors)
{
	try {
		if (!HsbOptionalPath(manifest, "roads", g_hsb_net.roads)) return false;
		if (!HsbOptionalPath(manifest, "autobahnen", g_hsb_net.autobahnen)) return false;
		if (!HsbOptionalPath(manifest, "rail", g_hsb_net.rail)) return false;
		if (!HsbOptionalPath(manifest, "airports", g_hsb_net.airports)) return false;
		if (!HsbOptionalPath(manifest, "harbours", g_hsb_net.harbours)) return false;
		if (!HsbOptionalPath(manifest, "stations", g_hsb_net.stations)) return false;
		if (!HsbOptionalBool(manifest, "with_rail", g_hsb_net.with_rail)) return false;
		if (!HsbOptionalBool(manifest, "with_fleet", g_hsb_net.with_fleet)) return false;
		if (!HsbOptionalBool(manifest, "build_on_slopes", g_hsb_net.build_on_slopes)) return false;
		if (!HsbOptionalBool(manifest, "autoslope", g_hsb_net.autoslope)) return false;
		int64_t money = 0, bus_towns = 0, trains = 0;
		if (!HsbOptionalRange(manifest, "network_money", kHsbNetworkMoney, money)) return false;
		if (!HsbOptionalRange(manifest, "bus_towns", kHsbBusTowns, bus_towns)) return false;
		if (!HsbOptionalRange(manifest, "trains", kHsbTrains, trains)) return false;
		g_hsb_net.money = static_cast<Money>(money);
		g_hsb_net.bus_towns = static_cast<uint>(bus_towns);
		g_hsb_net.trains = static_cast<uint>(trains);
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: invalid network manifest fields: {}", e.what());
		return false;
	}

	/* Three cross-field rules, all of which are a scenario that is wrong in a
	 * way only a player would notice. */
	if (!g_hsb_net.rail.empty() && !g_hsb_net.with_rail) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest names a rail file but with_rail is false; drop one or the other. "
			"Track without stations and depots looks broken.");
		return false;
	}
	if (!g_hsb_net.stations.empty() && (g_hsb_net.rail.empty() || !g_hsb_net.with_rail)) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest names stations but no rail line to put them on; drop stations or name rail");
		return false;
	}
	if (g_hsb_net.with_fleet && HsbNetworkIsRequested() && !g_hsb_net.with_rail && g_hsb_net.rail.empty()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest asks for a fleet with no rail and no roads; there would be nothing to run on");
		return false;
	}
	if (HsbNetworkIsRequested() && competitors < 2) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest asks for a network (competitors {}) but the infrastructure company "
			"takes one of those slots; the player needs at least 2. Raise --competitors.", competitors);
		return false;
	}
	return true;
}

void HsbReportRoadFailures()
{
	if (g_hsb_road_failures.empty()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: road refusals: none");
		return;
	}
	std::vector<std::pair<uint, StringID>> by_count;
	for (const auto &kv : g_hsb_road_failures) by_count.emplace_back(kv.second, kv.first);
	std::sort(by_count.begin(), by_count.end(), [](const auto &l, const auto &r) { return l.first > r.first; });
	for (const auto &kv : by_count) {
		Debug(Facility::Misc, Severity::Critical, "hsb: road refusal x{}: {}", kv.first,
			kv.second == kNoReason ? std::string("<no reason given>") : GetString(kv.second));
	}
	for (const auto &kv : g_hsb_rail_failures) {
		Debug(Facility::Misc, Severity::Critical, "hsb: rail refusal x{}: {}", kv.second, kv.first);
	}
}

/**
 * Log how many industries are alive at a given point in the build.
 *
 * The builder reports "industries placed 73/73, failed 0" and the save then
 * contains 62, so a later construction phase is eating industries the placement
 * just created. Nothing else in the build says so: a demolished industry is not
 * an error, and the road that bulldozed one looks like a perfectly good road.
 * Counting at each phase is the only way to see which phase did it.
 */
uint HsbCountIndustries()
{
	uint n = 0;
	for ([[maybe_unused]] const Industry *i : Industry::Iterate()) ++n;
	return n;
}

void HsbLogIndustries(const char *where)
{
	Debug(Facility::Misc, Severity::Critical, "hsb: industries alive: {} {}", HsbCountIndustries(), where);
}

/** Road this build created, so a corridor that cannot be finished can be taken
 *  back. Road that was already there is never in here: a shared tile belongs to
 *  the network, not to the segment that happened to find it first. */
std::vector<TileIndex> g_hsb_road_new_tiles;
std::vector<TileIndex> g_hsb_rail_new_tiles;

/** Remove road created since `mark`, newest first. Bridges and tunnels stay:
 *  they are structure, not route, and a bridge with no road on it is inert. */
void HsbUndoRoadFrom(size_t mark)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoWater};
	for (size_t i = g_hsb_road_new_tiles.size(); i > mark; --i) {
		const TileIndex t = g_hsb_road_new_tiles[i - 1];
		if (!IsValidTile(t) || !IsNormalRoadTile(t)) continue;
		Command<Commands::BuildRoad>::Do(kExec, t, RoadBits{}, HsbRoadType(), DisallowedRoadDirections{}, TownID::Invalid());
	}
	if (g_hsb_road_new_tiles.size() > mark) g_hsb_road_new_tiles.resize(mark);
}

/** Take back rail created since `mark`, newest first. */
void HsbUndoRailFrom(size_t mark)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoWater};
	for (size_t i = g_hsb_rail_new_tiles.size(); i > mark; --i) {
		const TileIndex t = g_hsb_rail_new_tiles[i - 1];
		if (IsValidTile(t) && IsPlainRailTile(t)) {
			Command<Commands::RemoveRail>::Do(kExec, t, Track::X);
			Command<Commands::RemoveRail>::Do(kExec, t, Track::Y);
		}
	}
	if (g_hsb_rail_new_tiles.size() > mark) g_hsb_rail_new_tiles.resize(mark);
}

/**
 * Is there a level bridge to be had from this water tile, heading this way?
 *
 * No commands: both the road and the rail planner call this for every water
 * tile they consider, and asking the engine instead would rebuild the map
 * inside a search.
 *
 * The water run is measured first, then flat banks are looked for past each end.
 * The first version felt outwards for at most eight tiles on each side, accepted
 * a bank on any ground at all, and issued one bridge command between two
 * adjacent bank candidates. A river wider than the search, or with a bank that
 * is not clear ground, was simply unbridged.
 */
bool HsbWaterSpannable(TileIndex t, int dx, int dy, TileIndex *out_lo = nullptr, TileIndex *out_hi = nullptr)
{
	if (dx == 0 && dy == 0) return false;
	const auto at = [&](int step) {
		return HsbTileClamped(TileX(t) + dx * step, TileY(t) + dy * step);
	};
	int back = 0, fwd = 0;
	while (back < 128 && IsWaterTile(at(-(back + 1)))) ++back;
	while (fwd < 128 && IsWaterTile(at(fwd + 1))) ++fwd;
	if (back + fwd == 0) return false;

	/* A level bridge wants flat ground at both heads, so look for exactly that
	 * and walk outwards until the terrain offers some. */
	const auto flat_bank = [&](int step) {
		const TileIndex c = at(step);
		if (!IsValidTile(c) || IsWaterTile(c) || GetTileSlope(c) != SLOPE_FLAT) return INVALID_TILE;
		const TileType tt = GetTileType(c);
		return (tt == TileType::Clear || tt == TileType::Trees || IsPlainRailTile(c) || IsNormalRoadTile(c)) ? c : INVALID_TILE;
	};
	TileIndex lo = INVALID_TILE, hi = INVALID_TILE;
	for (int k = 1; k <= 24 && (lo == INVALID_TILE || hi == INVALID_TILE); ++k) {
		if (lo == INVALID_TILE) lo = flat_bank(-(back + k));
		if (hi == INVALID_TILE) hi = flat_bank(fwd + k);
	}
	if (lo == INVALID_TILE || hi == INVALID_TILE) return false;
	if (lo > hi) std::swap(lo, hi);
	if (out_lo != nullptr) *out_lo = lo;
	if (out_hi != nullptr) *out_hi = hi;
	return true;
}

/**
 * Refuse to bridge over ground the builder is not willing to lose.
 *
 * The banks are found by walking outwards past the water run, so the gap between
 * the run and the bank is not necessarily empty - and the bridge command fills
 * every tile between the two heads, whatever is there. A factory in that gap was
 * converted and closed, silently, on a line that then failed anyway.
 */
bool HsbBridgeSpanIsClear(TileIndex lo, TileIndex hi, int dx, int dy)
{
	for (TileIndex c = lo; c != hi;) {
		const TileType tt = GetTileType(c);
		if (!IsWaterTile(c) && tt != TileType::Clear && tt != TileType::Trees) {
			Debug(Facility::Misc, Severity::Critical, "hsb: bridge from ({},{}) to ({},{}) would cross tile type {}; not building it",
				TileX(lo), TileY(lo), TileX(hi), TileY(hi), static_cast<int>(tt));
			return false;
		}
		c = HsbTileClamped(TileX(c) + dx, TileY(c) + dy);
	}
	return true;
}

/** Span water for a road leg running along `axis`. */
bool HsbSpanWater(TileIndex t, Axis axis)
{
	constexpr DoCommandFlags kStruct{DoCommandFlag::Execute};
	const int dx = (axis == Axis::X) ? 1 : 0;
	const int dy = (axis == Axis::X) ? 0 : 1;
	TileIndex lo{}, hi{};
	if (!HsbWaterSpannable(t, dx, dy, &lo, &hi)) return false;
	if (!HsbBridgeSpanIsClear(lo, hi, dx, dy)) {
		HsbNoteRoadFailure(CommandCost(STR_ERROR_NO_SUITABLE_ROAD), "span", t);
		return false;
	}
	if (!Command<Commands::BuildBridge>::Do(kStruct, lo, hi, TransportType::Road, 0, INVALID_RAILTYPE, HsbRoadType()).Succeeded()) {
		HsbNoteRoadFailure(CommandCost(STR_ERROR_NO_SUITABLE_ROAD), "span", t);
		return false;
	}
	return true;
}

/**
 * Build the given road bits on one tile of a leg, doing whatever it takes.
 *
 * The engine ORs the new bits into whatever the tile already has (CmdBuildRoad
 * ends in SetRoadBits(existing | pieces)), so a corner needs no special case:
 * the two legs each add their axis and the tile ends up a crossing. That also
 * means the road submenu's geometry falls out of the router for free.
 *
 * Three refusals are worth handling here, in this order, because each is a
 * structure rather than a mistake:
 *  - a tile that already carries road is the state we asked for,
 *  - water needs a bridge, and a bridge cannot start and end on the same tile,
 *  - a slope needs a tunnel, unless the scenario allows building on slopes
 *    (it does - see the newgame settings), in which case the road command levels
 *    the ground itself and there is nothing to do.
 *
 * The industry check is explicit and local rather than left to DoCommandFlag::
 * Auto, because Commands::LandscapeClear has no industry check at all: the road
 * command converts an industry tile whether or not Auto is set, Auto only
 * decides who pays for it. A corridor that no longer fits is a routing problem
 * the audit can report, not eleven silently missing industries.
 */
bool HsbBuildRoadTile(TileIndex t, RoadBits bits, bool dry_run = false)
{
	/* dry_run asks the same questions with the same flags minus Execute, so the
	 * engine validates the command and changes nothing. That is the whole point: a
	 * separate predicate would drift from the action, and a predicate that
	 * disagrees with the action is worse than no predicate at all. */
	constexpr DoCommandFlags kExecLive{DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoWater};
	constexpr DoCommandFlags kStructLive{DoCommandFlag::Execute, DoCommandFlag::Auto};
	constexpr DoCommandFlags kExecDry{DoCommandFlag::Auto, DoCommandFlag::NoWater};
	constexpr DoCommandFlags kStructDry{DoCommandFlag::Auto};
	const DoCommandFlags kExec = dry_run ? kExecDry : kExecLive;
	const DoCommandFlags kStruct = dry_run ? kStructDry : kStructLive;
	const Axis axis = (bits == ROAD_X) ? Axis::X : Axis::Y;

	/* TownID::Invalid() always, never a town.
	 *
	 * CmdBuildRoad opens with `if ((Company::IsValidID(company) && town_id !=
	 * TownID::Invalid()) || ...) return CMD_ERROR;` - a valid company may not name a
	 * town. The builder owns the network through a borrowed company, so passing one
	 * is refused on every tile with a bare CMD_ERROR: no message, no reason, and a
	 * tally that cannot see it because a refusal with no StringID is skipped.
	 *
	 * That is why ownership is a separate step (HsbOwnRoadLeg) doing the same two
	 * writes CmdBuildRoad makes: build the road as the company, then hand the tiles
	 * to the town so the town grows along it. */

	/* Checked before anything else, not inside build(), because the levelling and
	 * tunnelling attempts below run first and carry Auto too: guarding only the
	 * road build still lets a bulldozer through the back door. */
	if (IsTileType(t, TileType::Industry)) {
		HsbNoteRoadFailure(CommandCost(STR_ERROR_OBJECT_IN_THE_WAY), "industry", t);
		return false;
	}

	StringID first_why = INVALID_STRING_ID;
	auto build = [&]() {
		const bool was_road = IsNormalRoadTile(t);
		CommandCost res = Command<Commands::BuildRoad>::Do(kExec, t, bits, HsbRoadType(), DisallowedRoadDirections{}, TownID::Invalid());
		/* "Already built" is not the same as "a road vehicle can use this".
		 * Trusting the reply is how a leg reported success across a gap it never
		 * closed: stitching routed between two pieces two tiles apart, was told
		 * yes, and found them still separate hundreds of times over.
		 *
		 * Not trusting it naively is worse, and was: the first fix demanded
		 * IsNormalRoadTile, which a LEVEL CROSSING fails - TileType::Road, but not normal
		 * road. Rail crosses the road network constantly here, so every crossing
		 * became a wall and the trunk was cut at each one. The predicate is
		 * therefore "carries road traffic": normal road or a crossing. */
		if (res.Succeeded()) {
			if (!was_road && IsNormalRoadTile(t)) g_hsb_road_new_tiles.push_back(t);
			return true;
		}
		if (res.GetErrorMessage() == STR_ERROR_ALREADY_BUILT &&
				(IsNormalRoadTile(t) || IsLevelCrossing(t))) return true;
		if (first_why == INVALID_STRING_ID) first_why = res.GetErrorMessage();
		return false;
	};
	if (build()) return true;

	if (GetTileType(t) == TileType::Water) {
		if (!HsbSpanWater(t, axis)) return false;
		if (build()) return true;
	}

	if (GetTileSlope(t) != SLOPE_FLAT) {
		/* A road that would run across the contour is refused even with
		 * build_on_slopes, and a tunnel needs an entrance the terrain has not got.
		 * Levelling the tile is what a player does with the terraform tool, and it
		 * is what makes the road go through rather than around. Also without Auto:
		 * levelling under a house or industry is the same silent demolition as
		 * bulldozing it. */
		CommandCost lev = std::get<0>(Command<Commands::TerraformLand>::Do(kExec, t, ComplementSlope(GetTileSlope(t)), true));
		if (lev.Succeeded() && build()) return true;
		CommandCost tun = Command<Commands::BuildTunnel>::Do(kStruct, t, TransportType::Road, INVALID_RAILTYPE, HsbRoadType());
		if (tun.Succeeded() && build()) return true;
		HsbNoteRoadFailure(tun, "tunnel", t);
	}

	HsbNoteRoadFailure(CommandCost(first_why), "tile", t);
	return false;
}

/**
 * One straight road leg: a run of tiles along one axis.
 *
 * The geometry is the player's autoroad tool's - one straight leg per axis, one
 * right-angle corner - but the tiles are built one at a time rather than with
 * CmdBuildLongRoad. That command is atomic, so a single refusal anywhere leaves
 * the whole leg unbuilt and the corridor cut; one bad tile should cost one tile,
 * and the bridge or tunnel that tile needs gets built around it.
 */
bool HsbBuildRoadLeg(TileIndex from, TileIndex to, Axis axis, bool dry_run = false)
{
	if (from == to) return true;
	if (TileX(from) != TileX(to) && TileY(from) != TileY(to)) return false;

	const RoadBits bits = axis == Axis::X ? ROAD_X : ROAD_Y;
	TileIndex t = from;
	for (uint guard = 0; guard < Map::Size(); ++guard) {
		if (!IsValidTile(t)) return false;
		if (!HsbBuildRoadTile(t, bits, dry_run)) return false;
		if (t == to) break;
		t += TileOffsByDiagDir(AxisToDiagDir(axis));
	}
	return true;
}

/**
 * Could this leg be built, without building any of it?
 *
 * The answer has to be "yes" *before* the first tile is laid. HsbRouteRoad tries
 * up to 36 corner candidates per chunk, and each one used to build its first leg
 * and only then discover the second would not go - so a blocked corner left up
 * to 36 stubs radiating from one target, drawn on the map as a fan of parallel
 * roads leading nowhere.
 *
 * Asking is not an optimisation here, it is the only correct order. The builder
 * does have an undo (HsbUndoRoadFrom walks g_hsb_road_new_tiles back to a mark),
 * but it is keyed on failure, and the case that littered the map with stubs is a
 * leg that fails and then *recovers*: the router falls back to the planner,
 * reports success, and so nothing ever asks for the undo. Build first, ask
 * afterwards, and the stub is already on the map with nobody left to remove it.
 */
bool HsbRoadLegBuildable(TileIndex from, TileIndex to, Axis axis)
{
	return HsbBuildRoadLeg(from, to, axis, true);
}

/**
 * Build a straight leg, going one lane sideways if the direct line is blocked.
 *
 * The router's geometry is a dogleg: at most one corner. That is fine for
 * crossing a river and useless for a factory in the way, because getting past an
 * obstacle and back needs two corners, not one. Measured: the stitching pass
 * found the two nearest road pieces two tiles apart, in the same column, both
 * flat - the single tile between them is an industry, refused on purpose - and
 * all 80 nearest candidates were refused the same way. The road network was in
 * hundreds of pieces, and every one of the barriers was something worth
 * protecting.
 *
 * So when the direct line fails, the leg is retried one, two or three lanes to
 * the side, and the two ends are joined to that lane with short cross legs. That
 * is the U-shape the corner machinery cannot express, built by hand.
 */
bool HsbBuildRoadLegOrShoulder(TileIndex from, TileIndex to, Axis axis)
{
	if (HsbBuildRoadLeg(from, to, axis)) return true;

	/* Perpendicular step: across the leg for an X leg, along it for a Y leg. */
	const DiagDirection side_dir = (axis == Axis::X) ? DiagDirection::SW : DiagDirection::NW;
	const Axis cross = (axis == Axis::X) ? Axis::Y : Axis::X;
	for (const int off : {1, -1, 2, -2, 3, -3}) {
		const TileIndexDiff offset = TileOffsByDiagDir(side_dir) * off;
		const TileIndex a = from + offset;
		const TileIndex b = to + offset;
		if (!IsValidTile(a) || !IsValidTile(b)) continue;
		/* The shoulder must be parallel: a shift of one tile keeps a column a
		 * column and a row a row, which is what makes the two cross legs
		 * straight. */
		if (TileX(a) != TileX(b) || TileY(a) != TileY(b)) continue;
		if (HsbBuildRoadLeg(a, b, axis) &&
				HsbBuildRoadLeg(from, a, cross) &&
				HsbBuildRoadLeg(b, to, cross)) {
			Debug(Facility::Misc, Severity::Critical, "hsb: road leg ({},{})->({},{}) went {} lane(s) to the side",
			      TileX(from), TileY(from), TileX(to), TileY(to), off);
			return true;
		}
	}
	return false;
}

/**
 * Hand a leg's tiles to a town, so the town grows along the road.
 *
 * The engine's road command is called with OWNER_NONE by anything that is not a
 * company's own road, and the per-tile call this replaced passed a TownID. That
 * ownership is what makes a town sprawl along its road instead of ignoring it, so
 * it is set afterwards - the same two writes CmdBuildRoad makes.
 */
void HsbOwnRoadLeg(TileIndex from, TileIndex to, Axis axis, TownID tid)
{
	if (tid == TownID::Invalid()) return;
	const DiagDirection dir = AxisToDiagDir(axis);
	TileIndex t = from;
	for (uint guard = 0; guard < Map::Size(); ++guard) {
		if (!IsValidTile(t)) break;
		/* Only inside the town's own claim, and only the last stretch out to it.
		 *
		 * Handing the *whole* leg to the town looks like generosity and is a
		 * disaster: the nearest town to a 100 km corridor is the same town for all
		 * of it, so the entire trunk became town road. The railway then met the
		 * company's own network with "must remove road first" 31 times and laid no
		 * track at all, because a level crossing needs a road the company does not
		 * own. The trunk has to stay the company's - that is what makes the rail
		 * cross it - and the town's approach has to be the town's, which is what
		 * makes the town grow along the road that reaches it.
		 *
		 * The two together are exactly the tiles inside the town's claim. */
		const bool in_claim = GetTownIndex(t) == tid;
		if (in_claim && IsTileType(t, TileType::Road) && !IsRoadDepot(t)) {
			SetRoadOwner(t, RoadTramType::Road, OWNER_TOWN);
		} else if (in_claim && MayHaveRoad(t)) {
			SetRoadOwner(t, RoadTramType::Road, OWNER_TOWN);
		} else if (IsNormalRoadTile(t)) {
			/* Company road, but tagged with the town so the railway can plan a
			 * crossing against it and the town knows the road serves it. */
			SetTownIndex(t, tid);
		}
		if (t == to) break;
		t += TileOffsByDiagDir(dir);
	}
}

/** Cardinal road neighbours, the test for a dead end (0 or 1). */
uint HsbRoadNeighbours(TileIndex t)
{
	constexpr int kNeighbours[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	uint n = 0;
	for (const auto &d : kNeighbours) {
		TileIndex n2 = t + TileDiffXY(d[0], d[1]);
		if (IsValidTile(n2) && IsNormalRoadTile(n2)) ++n;
	}
	return n;
}

int HsbManhattanTiles(TileIndex a, TileIndex b)
{
	return HsbManhattan(static_cast<int>(TileX(a)), static_cast<int>(TileY(a)),
	                    static_cast<int>(TileX(b)), static_cast<int>(TileY(b)));
}

/**
 * Can this tile carry a road, allowing for a bridge or a levelled slope?
 *
 * Deliberately more generous than "plain land": a road crosses a river on a
 * bridge and cuts through a slope, and a search that treats either as a wall
 * sends every corridor the long way round or gives up. What it must still refuse
 * is the ground no road can ever go on - a house, an industry, a station -
 * because those come back as a build that dies halfway along a path.
 */
bool HsbCarriesRoad(TileIndex t)
{
	if (!IsValidTile(t)) return false;
	switch (GetTileType(t)) {
		case TileType::Clear:
		case TileType::Trees:
		case TileType::Road:
			return true;
		case TileType::Water: {
			/* Span-able if two buildable banks sit on one axis within reach. */
			const auto is_bank = [](TileIndex c) {
				if (!IsValidTile(c)) return false;
				const TileType tt = GetTileType(c);
				return tt == TileType::Clear || tt == TileType::Trees || IsNormalRoadTile(c);
			};
			for (const Axis axis : {Axis::X, Axis::Y}) {
				for (int reach = 1; reach <= 8; ++reach) {
					for (const int side : {1, -1}) {
						const int s1 = side * reach, s2 = side * (reach + 1);
						TileIndex a = HsbTileClamped(TileX(t) + (axis == Axis::X ? s1 : 0),
						                              TileY(t) + (axis == Axis::X ? 0 : s1));
						TileIndex b = HsbTileClamped(TileX(t) + (axis == Axis::X ? s2 : 0),
						                              TileY(t) + (axis == Axis::X ? 0 : s2));
						if (is_bank(a) && is_bank(b)) return true;
					}
				}
			}
			return false;
		}
		case TileType::TunnelBridge:
			/* Only a road bridge carries the road. Where the rail network already
			 * bridged the river the tile is a rail bridge, and the road command
			 * answers "must demolish bridge first" - so treat it as the wall it is
			 * and route round. */
			return GetTunnelBridgeTransportType(t) == TransportType::Road;
		default:
			return false;
	}
}

bool HsbWalkRoad(TileIndex from, TileIndex to);

/**
 * Plan a path of road from A to B, then build it, re-planning if the build
 * disagrees with the plan.
 *
 * The plan is made on HsbCarriesRoad, which is a model of the map rather than the
 * map itself, so it can be wrong: a river it thinks it can span, a slope it thinks
 * it can level. The build is the truth, so when the two disagree the offending
 * tile is marked and the path is planned again. Twenty-four attempts is plenty -
 * each one removes a tile the previous plan was wrong about.
 *
 * The greedy walk this replaces gave up as soon as it was boxed in, which on a
 * map with towns on it was most of the time: an industry one tile ahead with
 * houses around it has no sidestep, and a search simply goes round.
 */
bool HsbPlanAndBuildRoad(TileIndex from, TileIndex to, TownID tid)
{
	if (from == to) return true;
	constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

	std::vector<uint8_t> banned(Map::Size(), 0);
	for (uint attempt = 0; attempt < 24; ++attempt) {
		std::vector<uint32_t> prev(Map::Size(), UINT32_MAX);
		std::vector<TileIndex> queue;
		queue.reserve(65536);
		prev[from.base()] = from.base();
		queue.push_back(from);

		bool found = false;
		for (size_t head = 0; head < queue.size() && head <= HsbSearchNodeBudget; ++head) {
			const TileIndex cur = queue[head];
			if (cur == to) { found = true; break; }
			for (const auto &d : kSteps) {
				TileIndex n = cur + TileDiffXY(d[0], d[1]);
				if (!IsValidTile(n) || prev[n.base()] != UINT32_MAX || banned[n.base()]) continue;
				if (!HsbCarriesRoad(n)) continue;
				prev[n.base()] = cur.base();
				queue.push_back(n);
			}
		}
		if (!found) {
			/* No path at all in the model. The walk takes a different view of the
			 * ground and sometimes gets through where the model says no. */
			return HsbWalkRoad(from, to);
		}

		std::vector<TileIndex> path;
		for (TileIndex t = to; t != from; t = TileIndex(static_cast<TileIndex::BaseType>(prev[t.base()]))) {
			path.push_back(t);
		}
		/* Build from the far end back, so each tile already carries the road on
		 * the side the path leaves by. */
		const size_t mark = g_hsb_road_new_tiles.size();
		bool ok = true;
		TileIndex bad = INVALID_TILE;
		for (size_t i = path.size(); i > 0 && ok; --i) {
			const TileIndex t = path[i - 1];
			const TileIndex next = i < path.size() ? path[i] : from;
			if (!HsbBuildRoadTile(t, TileY(next) == TileY(t) ? ROAD_X : ROAD_Y)) {
				bad = t;
				ok = false;
			}
		}
		if (ok) {
			/* The detour is the town's approach just as much as the straight leg is,
			 * so it gets the same ownership pass - inside the town's claim only, for
			 * the reason HsbOwnRoadLeg gives. */
			for (TileIndex t : path) {
				if (tid == TownID::Invalid()) break;
				if (IsNormalRoadTile(t) && GetTownIndex(t) == tid) SetRoadOwner(t, RoadTramType::Road, OWNER_TOWN);
			}
			return true;
		}
		HsbUndoRoadFrom(mark);
		/* Ban the whole path, not just the tile that failed. A plan that dies a
		 * third of the way along was wrong about everything after the failing tile
		 * too, and banning one tile per attempt ran out of attempts long before it
		 * found its way round a mountain range. The endpoints stay: they are where
		 * the corridor is going, and the caller snapped them. */
		for (TileIndex t : path) {
			if (t == from || t == to) continue;
			banned[t.base()] = 1;
		}
		if (!IsValidTile(bad) || bad == from) return HsbWalkRoad(from, to);
	}
	return HsbWalkRoad(from, to);
}

/**
 * Fallback: walk tile by tile toward the target, building what each step needs.
 *
 * This is the geometry the straight router exists to avoid, so it only runs where
 * the terrain refuses a straight piece - a river the banks will not span, a cliff
 * band, the middle of a town. Steps are always orthogonal: a diagonal step would
 * leave two tiles sharing a corner, which is the defect this whole rework is
 * about.
 */
bool HsbWalkRoad(TileIndex from, TileIndex to)
{
	TileIndex cur = from;
	auto try_step = [&](TileIndex next) -> bool {
		if (next == cur || !IsValidTile(next)) return false;
		const RoadBits bits = TileY(next) == TileY(cur) ? ROAD_X : ROAD_Y;
		return HsbBuildRoadTile(cur, bits) && HsbBuildRoadTile(next, bits);
	};

	for (uint guard = 0; guard < 4096; ++guard) {
		if (cur == to) return true;
		const int dx = (TileX(to) > TileX(cur)) - (TileX(to) < TileX(cur));
		const int dy = (TileY(to) > TileY(cur)) - (TileY(to) < TileY(cur));
		if (dx != 0) {
			TileIndex next = TileXY(static_cast<int>(TileX(cur)) + dx, TileY(cur));
			if (try_step(next)) { cur = next; continue; }
		}
		if (dy != 0) {
			TileIndex next = TileXY(TileX(cur), static_cast<int>(TileY(cur)) + dy);
			if (try_step(next)) { cur = next; continue; }
		}
		/* Blocked ahead: sidestep. A detour is allowed, and a generous one: the
		 * only place a road genuinely wants to go round something is the Alps. */
		bool moved = false;
		for (const int d : {1, -1}) {
			for (const int along : {0, 1}) {
				TileIndex cand = along == 0 ? HsbTileClamped(static_cast<int>(TileX(cur)) + d, TileY(cur))
				                            : HsbTileClamped(TileX(cur), static_cast<int>(TileY(cur)) + d);
				if (!HsbDetourAllowed(HsbManhattanTiles(cur, to), HsbManhattanTiles(cand, to), 64)) continue;
				if (GetTileType(cand) != TileType::Clear && !IsWaterTile(cand) && !IsTileType(cand, TileType::Trees)) continue;
				if (try_step(cand)) { cur = cand; moved = true; break; }
			}
			if (moved) break;
		}
		if (!moved) {
			if (HsbRoadTraceLeft() > 0) {
				Debug(Facility::Misc, Severity::Critical, "hsb:   walk boxed in at ({},{}) target ({},{}) distance {} type {} slope {}",
					TileX(cur), TileY(cur), TileX(to), TileY(to), HsbManhattanTiles(cur, to),
					GetTileType(cur), GetTileSlope(cur));
			}
			return false;   // boxed in
		}
	}
	return false;
}

/**
 * Are two tiles joined by road, within a radius?
 *
 * Every "the router said yes" in this build turned out to be worth checking, and
 * twice it was not true: a leg reported success across a gap it never closed (that
 * one was STR_ERROR_ALREADY_BUILT lying), and then a chunk reported success having
 * built nothing at all, because HsbRoutePiece skips both of its legs when the
 * corner happens to land on the end it started from. The symptom was always the
 * same and never in the log: hundreds of road components, and a bus that reached
 * one town out of 124.
 *
 * So the router is checked rather than believed. The search is bounded because it
 * runs per route: the chunk router advances `HsbRoadRun` tiles at a time, so the
 * interesting radius is the Manhattan distance plus a margin for the corner.
 *
 * @param max_radius give up the search past this many steps; 0 means no extra cap
 */
bool HsbRoadConnected(TileIndex a, TileIndex b, uint max_radius)
{
	if (a == b) return true;
	if (!IsValidTile(a) || !IsValidTile(b)) return false;
	const auto walkable = [](TileIndex t) { return IsValidTile(t) && (IsNormalRoadTile(t) || IsLevelCrossing(t)); };
	if (!walkable(a) || !walkable(b)) return false;
	const uint radius = HsbConnectRadius(static_cast<uint>(HsbManhattanTiles(a, b)), max_radius);

	std::map<TileIndex, uint> seen;
	std::vector<TileIndex> queue;
	queue.push_back(a);
	seen[a] = 0;
	for (size_t head = 0; head < queue.size(); ++head) {
		const TileIndex cur = queue[head];
		const uint d = seen[cur];
		if (cur == b) return true;
		if (d >= radius) continue;
		for (const DiagDirection dir : {DiagDirection::SW, DiagDirection::SE, DiagDirection::NW, DiagDirection::NE}) {
			const TileIndex n = cur + TileOffsByDiagDir(dir);
			if (seen.count(n) != 0 || !walkable(n)) continue;
			seen[n] = d + 1;
			queue.push_back(n);
		}
	}
	return false;
}

/**
 * One piece of a corridor: at most two straight legs joined by a corner.
 *
 * `via` is the corner tile, `axis1` the leg before it and `axis2` the leg after.
 * Either leg can be empty, which is how a piece that only needs to run along one
 * axis is expressed. When the straight way is refused the tile-by-tile planner
 * takes over, so a piece always connects if it can connect at all.
 */
bool HsbRoutePiece(TileIndex from, TileIndex via, TileIndex to, Axis axis1, Axis axis2, TownID tid)
{
	/* A degenerate chunk - the corner landed back on the end it started from -
	 * builds nothing at all. Reporting success for that is a lie the caller cannot
	 * see, and it is checked here so the chunk at least says "no road needed"
	 * rather than "road built". */
	if (via == from && via == to) return from == to;

	/* Both legs, or neither.
	 *
	 * This used to build the first leg and then try the second, so a candidate that
	 * failed on leg 2 fell back to the planner with leg 1 already down and no way to
	 * take it back. HsbRouteRoad runs up to 36 candidates per chunk, so a corner
	 * nobody could get round left up to 36 stubs fanning out from one point.
	 *
	 * Asking first costs one extra pass over the leg for the candidate that
	 * eventually wins and saves the road for all the ones that do not. */
	const bool first_ok = (via == from) || HsbRoadLegBuildable(from, via, axis1);
	const bool second_ok = first_ok && ((via == to) || HsbRoadLegBuildable(via, to, axis2));
	if (!first_ok || !second_ok) {
		return HsbPlanAndBuildRoad(from, to, tid);
	}
	if (via != from) HsbBuildRoadLeg(from, via, axis1);
	if (via != to) HsbBuildRoadLeg(via, to, axis2);
	if (via != from) HsbOwnRoadLeg(from, via, axis1, tid);
	if (via != to) HsbOwnRoadLeg(via, to, axis2, tid);
	return true;
}

/**
 * Nearest tile that can take a road without a structure being built first.
 *
 * A corridor aims at a town centre, and a town centre is solid with houses - a
 * road cannot be built there at all. A through-road reaches the edge of a town and
 * stops, and the town then grows along it from there, so snapping the endpoint
 * outwards is both buildable and what a real town's approach looks like. The same
 * helper nudges the corner and the per-piece target of a corridor, which are raw
 * coordinates and can land on a house or on water.
 */
TileIndex HsbSnapBuildable(TileIndex t, uint radius, bool *moved = nullptr)
{
	if (moved != nullptr) *moved = false;
	auto buildable = [](TileIndex c) {
		if (!IsValidTile(c)) return false;
		switch (GetTileType(c)) {
			case TileType::Clear:
			case TileType::Trees:
			case TileType::Road:
				return true;
			default:
				return false;
		}
	};
	if (buildable(t)) return t;
	for (TileIndex c : SpiralTileSequence(t, radius, 0, 0)) {
		if (!buildable(c)) continue;
		if (moved != nullptr) *moved = true;
		return c;
	}
	return t;
}

/** What a straight leg has to overcome, to choose a corner before building. */
uint HsbLegObstacles(TileIndex from, TileIndex to, Axis axis)
{
	uint cost = 0;
	const DiagDirection dir = AxisToDiagDir(axis);
	TileIndex t = from;
	for (uint guard = 0; guard < Map::Size(); ++guard) {
		if (!IsValidTile(t)) break;
		cost += HsbObstacleCost(GetTileType(t) == TileType::Water, GetTileSlope(t) != SLOPE_FLAT,
		                        GetTileType(t) == TileType::Industry);
		if (t == to) break;
		t += TileOffsByDiagDir(dir);
	}
	return cost;
}

/**
 * Route a road from A to B the way the player's autoroad tool routes one: long
 * straight runs joined by right-angle corners.
 *
 * Within a piece both corner orders are costed (water, slopes and industries
 * weighted, see HsbObstacleCost) and the cheaper is built; a tie goes to the
 * horizontal-first order, which is the left-to-right run the tool gives.
 */
bool HsbRouteRoad(TileIndex a, TileIndex b, TownID tid)
{
	if (a == b) return true;
	if (!IsValidTile(a) || !IsValidTile(b)) return false;

	/* Everything this segment builds is remembered, so a segment that cannot be
	 * finished leaves nothing behind. Rolling back per piece would keep the
	 * completed pieces and put a road in the middle of nowhere. */
	const size_t segment_mark = g_hsb_road_new_tiles.size();

	const int x0 = static_cast<int>(TileX(a));
	const int y0 = static_cast<int>(TileY(a));
	const int tx = static_cast<int>(TileX(b));
	const int ty = static_cast<int>(TileY(b));

	/* Already axis-aligned: one straight run, no corner. */
	if (x0 == tx || y0 == ty) {
		const Axis axis = y0 == ty ? Axis::X : Axis::Y;
		if (HsbBuildRoadLegOrShoulder(a, b, axis)) {
			HsbOwnRoadLeg(a, b, axis, tid);
			return true;
		}
		const bool planned = HsbPlanAndBuildRoad(a, b, tid);
		if (!planned) HsbUndoRoadFrom(segment_mark);
		return planned;
	}

	/* The corner stays where the geometry puts it - a straight leg needs both ends
	 * on one row or one column - and only the target is nudged onto buildable
	 * ground, along the axis the second leg runs on. */
	const auto buildable_at = [&](int x, int y) {
		switch (GetTileType(HsbTileClamped(x, y))) {
			case TileType::Clear:
			case TileType::Trees:
			case TileType::Road:
				return true;
			default:
				return false;
		}
	};

	int x = x0;
	int y = y0;
	for (uint guard = 0; guard < 4096; ++guard) {
		if (x == tx && y == ty) break;
		const int run = HsbRoadRun();
		const int nx = HsbRunEnd(x, tx, run);
		const int ny = HsbRunEnd(y, ty, run);
		const TileIndex here = HsbTileClamped(x, y);

		struct Candidate {
			TileIndex via;
			TileIndex end;
			Axis first_axis;
			Axis second_axis;
			uint cost;
		};
		std::vector<HsbCorner> corners = HsbCornerCandidates(x, y, nx, ny, HsbCornerFan(),
			HsbRoadSnap(), buildable_at);
		std::vector<Candidate> cands;
		cands.reserve(corners.size());
		for (const auto &corner : corners) {
			cands.push_back({
				HsbTileClamped(corner.via_x, corner.via_y),
				HsbTileClamped(corner.end_x, corner.end_y),
				corner.first == HsbLegAxis::x ? Axis::X : Axis::Y,
				corner.second == HsbLegAxis::x ? Axis::X : Axis::Y,
				0});
		}
		for (auto &c : cands) {
			c.cost = HsbLegObstacles(here, c.via, c.first_axis) + HsbLegObstacles(c.via, c.end, c.second_axis);
		}
		/* Stable, so a tie goes to the candidate that was pushed first: the two
		 * geometric corners, in horizontal-first order. */
		std::stable_sort(cands.begin(), cands.end(),
			[](const Candidate &a, const Candidate &b) { return a.cost < b.cost; });

		bool ok = false;
		for (const auto &c : cands) {
			if (HsbRoutePiece(here, c.via, c.end, c.first_axis, c.second_axis, tid)) {
				ok = true;
				break;
			}
		}
		if (!ok) {
			if (HsbRoadTraceLeft() > 0) {
				Debug(Facility::Misc, Severity::Critical, "hsb: trace link ({},{})->({},{}) chunk here=({},{}) cost range {}-{}",
					TileX(a), TileY(a), TileX(b), TileY(b), TileX(here), TileY(here),
					cands.empty() ? 0u : cands.front().cost, cands.empty() ? 0u : cands.back().cost);
				for (int d = -3; d <= 3; ++d) {
					const TileIndex probe = HsbTileClamped(TileX(here) + d, TileY(here));
					Debug(Facility::Misc, Severity::Critical, "hsb:   row tile ({},{}) type {} slope {} roadbits {}",
						TileX(probe), TileY(probe), GetTileType(probe), GetTileSlope(probe),
						IsNormalRoadTile(probe) ? static_cast<int>(GetRoadBits(probe, RoadTramType::Road).base()) : -1);
				}
				--HsbRoadTraceLeft();
			}
			HsbUndoRoadFrom(segment_mark);
			return false;
		}

		x = nx;
		y = ny;
	}
	if (x != tx || y != ty) return false;

	/* Checked, not believed.
	 *
	 * The stub router asked for a chunk, believed the reply, and reported success
	 * having joined nothing: the last piece of a corridor can land two tiles from
	 * where it started and still answer "built". Nothing downstream can see that -
	 * the caller only gets a bool - so the only place to catch it is here, where
	 * the two ends are still in hand.
	 *
	 * The radius is the whole segment's length plus a margin, because a chunk that
	 * silently failed leaves the two ends up to that far apart. */
	if (!HsbRoadConnected(a, b, static_cast<uint>(HsbManhattanTiles(a, b)) + 8)) {
		Debug(Facility::Misc, Severity::Critical, "hsb: route ({},{})->({},{}) reported success but the two ends are not joined by road",
			TileX(a), TileY(a), TileX(b), TileY(b));
		return false;
	}
	return true;
}

/**
 * Join every town's streets to the trunk road.
 *
 * The audit asked "can a bus from the largest town reach the others" and said one
 * of 124, while "towns_on_network" said 111 of 124. Both are true and they measure
 * different things: the first floods over road, the second only asks whether any
 * road tile within eight of a town belongs to the largest component, so a trunk
 * road that stops two tiles short of the kerb scores a town as connected while
 * nothing can be driven to it.
 *
 * The link router picks an endpoint by snapping to something buildable, and
 * "buildable" includes open ground, so a route that fails to reach the town still
 * leaves its leg lying in a field, and nothing checked. This pass does: for every
 * town, find its own road, find the nearest road that is not the town's, and route
 * between them.
 */
void HsbJoinTownsToTrunk()
{
	/* Town streets are OWNER_TOWN road; the network the builder lays is the
	 * company. That difference is the whole test. */
	const auto is_town_road = [](TileIndex t) {
		return IsValidTile(t) && IsNormalRoadTile(t) && GetRoadOwner(t, RoadTramType::Road) == OWNER_TOWN;
	};
	const auto is_other_road = [](TileIndex t) {
		return IsValidTile(t) && IsNormalRoadTile(t) && GetRoadOwner(t, RoadTramType::Road) != OWNER_TOWN;
	};

	uint towns_joined = 0, towns_already = 0, towns_failed = 0, towns_nothing = 0;
	for (Town *town : Town::Iterate()) {
		if (town->xy == INVALID_TILE) continue;

		TileIndex mine = INVALID_TILE, theirs = INVALID_TILE;
		for (TileIndex t : SpiralTileSequence(town->xy, 14, 0, 0)) {
			if (mine == INVALID_TILE && is_town_road(t)) mine = t;
			if (theirs == INVALID_TILE && is_other_road(t)) theirs = t;
			if (mine != INVALID_TILE && theirs != INVALID_TILE) break;
		}
		if (mine == INVALID_TILE || theirs == INVALID_TILE) { ++towns_nothing; continue; }

		/* Is the town's own road already against the network the builder laid? The
		 * test is whether a trunk road touches a town road orthogonally (corner-touching
		 * is not drivable). */
		bool touching = false;
		for (const auto &k : kNeighbours) {
			if (is_other_road(mine + TileDiffXY(k[0], k[1]))) { touching = true; break; }
		}
		if (touching) { ++towns_already; continue; }

		if (HsbRouteRoad(mine, theirs, town->index)) {
			++towns_joined;
		} else {
			++towns_failed;
			Debug(Facility::Misc, Severity::Critical, "hsb: could not join {} ({},{}) to the trunk road", town->name, TileX(town->xy), TileY(town->xy));
		}
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: town-to-trunk joins: {} newly routed, {} already touching, {} refused, {} with no road nearby",
	      towns_joined, towns_already, towns_failed, towns_nothing);
}

/**
 * Stitch the road network into one piece.
 *
 * With the towns attached a bus still reached one town of 124, and the audit said
 * hundreds of road components with a largest of a few thousand tiles. So the towns
 * are no longer the problem: the trunk is in pieces, and the links between pieces
 * are the ones that never got built.
 *
 * One breadth-first sweep from the largest component, over land, gives every tile
 * its distance to that component. The road tiles not in it, cheapest first, are
 * then candidate joins. Each is tried in order; the first that actually merges two
 * components wins and the sweep restarts from the bigger component. The loop only
 * stops when a whole pass of candidates fails, or when there is one component left.
 *
 * "Actually merges" is checked, not believed, and the map is re-labelled after
 * every attempt so a router that lies again cannot pass for progress.
 */
void HsbJoinRoadFragments()
{
	const size_t size = Map::Size();
	constexpr int kNeighbours[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	const auto is_road = [](TileIndex t) { return IsValidTile(t) && IsNormalRoadTile(t); };
	const auto is_land = [](TileIndex t) { return IsValidTile(t) && !IsWaterTile(t); };

	/* Label every road component and report the largest. */
	auto label_components = [&](std::vector<uint32_t> &label, uint &largest, uint32_t &largest_id) {
		label.assign(size, 0);
		largest = 0;
		largest_id = 0;
		uint32_t next = 0;
		std::vector<TileIndex> queue;
		queue.reserve(1 << 16);
		for (uint s = 0; s < size; ++s) {
			const TileIndex start(static_cast<TileIndex::BaseType>(s));
			if (label[s] || !is_road(start)) continue;
			++next;
			uint count = 0;
			queue.clear();
			queue.push_back(start);
			label[s] = next;
			while (!queue.empty()) {
				const TileIndex cur = queue.back();
				queue.pop_back();
				++count;
				for (const auto &d : kNeighbours) {
					TileIndex n = cur + TileDiffXY(d[0], d[1]);
					if (!IsValidTile(n) || label[n.base()] || !is_road(n)) continue;
					label[n.base()] = next;
					queue.push_back(n);
				}
			}
			if (count > largest) { largest = count; largest_id = next; }
		}
		return next;
	};

	std::vector<uint32_t> label;
	uint largest = 0;
	uint32_t largest_id = 0;
	uint components = label_components(label, largest, largest_id);
	const uint components_before = components;
	Debug(Facility::Misc, Severity::Critical, "hsb: road components before stitching: {} (largest {} tiles)", components_before, largest);
	if (components <= 1) return;

	uint joins = 0, lies = 0, rounds = 0;
	constexpr int kMaxRounds = 400;
	constexpr size_t kMaxCandidates = 80;
	for (int round = 0; round < kMaxRounds; ++round) {
		++rounds;
		/* Distance from the largest component, over land. */
		std::vector<uint32_t> dist(size, 0xFFFFFFFFu);
		std::vector<TileIndex> queue;
		queue.reserve(1 << 16);
		for (uint s = 0; s < size; ++s) {
			const TileIndex t(static_cast<TileIndex::BaseType>(s));
			if (!is_road(t) || label[s] != largest_id) continue;
			dist[s] = 0;
			queue.push_back(t);
		}
		while (!queue.empty()) {
			const TileIndex cur = queue.back();
			queue.pop_back();
			const uint32_t d = dist[cur.base()];
			for (const auto &k : kNeighbours) {
				TileIndex n = cur + TileDiffXY(k[0], k[1]);
				if (!is_land(n) || dist[n.base()] != 0xFFFFFFFFu) continue;
				dist[n.base()] = d + 1;
				queue.push_back(n);
			}
		}

		/* Candidate joins, nearest first. The single closest pair is not enough: it
		 * is often the one a factory, a tunnel or a wide river sits in the way of,
		 * and stopping there leaves hundreds of fragments for want of trying the
		 * second. */
		std::vector<std::pair<uint32_t, TileIndex>> candidates;
		for (uint s = 0; s < size; ++s) {
			const TileIndex t(static_cast<TileIndex::BaseType>(s));
			if (!is_road(t) || label[s] == largest_id) continue;
			if (dist[s] == 0xFFFFFFFFu) continue;
			candidates.emplace_back(dist[s], t);
		}
		if (candidates.empty()) break;
		std::sort(candidates.begin(), candidates.end());
		if (candidates.size() > kMaxCandidates) candidates.resize(kMaxCandidates);

		bool merged = false;
		uint refused = 0;
		for (const auto &cand : candidates) {
			const TileIndex foreign = cand.second;
			TileIndex mine = foreign;
			bool walked = true;
			for (uint32_t d = cand.first; d > 0; --d) {
				TileIndex step = INVALID_TILE;
				for (const auto &k : kNeighbours) {
					const TileIndex n = mine + TileDiffXY(k[0], k[1]);
					if (IsValidTile(n) && dist[n.base()] == d - 1) { step = n; break; }
				}
				if (step == INVALID_TILE) { walked = false; break; }
				mine = step;
			}
			if (!walked || !is_road(mine)) continue;

			const Town *near = ClosestTownFromTile(foreign, UINT_MAX);
			const TownID tid = (near == nullptr) ? TownID::Invalid() : near->index;
			const uint before = components;
			const bool routed = HsbRouteRoad(mine, foreign, tid);
			components = label_components(label, largest, largest_id);
			if (!routed) { ++refused; continue; }
			if (components >= before) {
				++lies;
				Debug(Facility::Misc, Severity::Critical, "hsb: stitch at ({},{})-({},{}) reported success but the pieces are still separate",
				      TileX(mine), TileY(mine), TileX(foreign), TileY(foreign));
				continue;
			}
			merged = true;
			break;
		}
		if (!merged) {
			Debug(Facility::Misc, Severity::Critical, "hsb: stitching gave up: {} of {} candidates refused", refused, candidates.size());
			break;
		}
		++joins;
		if (components <= 1) break;
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: road fragments stitched: {} joins over {} rounds ({} routes reported success but merged nothing), "
		"{} -> {} components, largest {} tiles",
		joins, rounds, lies, components_before, components, largest);
}

/**
 * Prune road that dead-ends nowhere.
 *
 * A branch of road with a free end connects nothing, and the player cannot tell
 * it from a road that simply was not finished - so it is removed. A road that ends
 * *at* a town or an industry is a legitimate terminus and stays, as does anything
 * on a through route.
 */
uint HsbPruneDeadEnds()
{
	const auto near_settlement = [](TileIndex t) {
		for (const Town *town : Town::Iterate()) {
			if (town->xy == INVALID_TILE) continue;
			if (std::abs(static_cast<int>(TileX(t)) - static_cast<int>(TileX(town->xy))) <= 6 &&
				std::abs(static_cast<int>(TileY(t)) - static_cast<int>(TileY(town->xy))) <= 6) return true;
		}
		for (const Industry *ind : Industry::Iterate()) {
			if (ind->location.tile == INVALID_TILE) continue;
			if (std::abs(static_cast<int>(TileX(t)) - static_cast<int>(TileX(ind->location.tile))) <= 3 &&
				std::abs(static_cast<int>(TileY(t)) - static_cast<int>(TileY(ind->location.tile))) <= 3) return true;
		}
		return false;
	};
	uint pruned = 0;
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoWater};
	std::vector<uint8_t> done(Map::Size(), 0);
	for (uint s = 0; s < Map::Size(); ++s) {
		TileIndex start(static_cast<TileIndex::BaseType>(s));
		if (!IsValidTile(start) || !IsNormalRoadTile(start) || done[s]) continue;
		if (HsbRoadNeighbours(start) != 1) continue;
		if (near_settlement(start)) continue;

		/* Walk the branch, remembering it, until it reaches a junction, a
		 * settlement, or another branch. */
		std::vector<TileIndex> branch;
		TileIndex cur = start;
		bool keep = false;
		for (uint guard = 0; guard < 4096; ++guard) {
			branch.push_back(cur);
			done[cur.base()] = 1;
			if (near_settlement(cur)) { keep = true; break; }
			if (HsbRoadNeighbours(cur) != 1) break;   // reached a through tile
			TileIndex next{};
			bool have_next = false;
			for (const auto &k : kNeighbours) {
				TileIndex n = cur + TileDiffXY(k[0], k[1]);
				if (!IsValidTile(n) || !IsNormalRoadTile(n)) continue;
				if (std::find(branch.begin(), branch.end(), n) != branch.end()) continue;
				next = n;
				have_next = true;
				break;
			}
			if (!have_next) break;
			cur = next;
		}
		if (keep) continue;
		/* Remove from the far end back, so each removal sees a free end. */
		for (auto it = branch.rbegin(); it != branch.rend(); ++it) {
			if (!IsNormalRoadTile(*it)) continue;
			if (Command<Commands::BuildRoad>::Do(kExec, *it, RoadBits{}, HsbRoadType(), DisallowedRoadDirections{}, TownID::Invalid()).Succeeded()) {
				++pruned;
			}
		}
	}
	return pruned;
}

/* ---------------------------------------------------------------------------
 * Rail: the same straight-run geometry, with rails.
 *
 * The rail autorail tool is the same idea as the road autoroad tool - one straight
 * leg per axis, one right-angle corner - and the same reasons apply: a track laid
 * tile by tile toward its target is a staircase that no railway looks like. The
 * engine orients the track to the slope it lands on, so a leg on a hillside is a
 * diagonal line, which is correct for rail and looks right.
 * ------------------------------------------------------------------------- */

/** Span water with a rail bridge, bank to bank, as HsbSpanWater does for road. */
bool HsbSpanWaterRail(TileIndex t, TileIndex next)
{
	/* A bridge is one command from bank to bank, and it fills every tile between.
	 * Bridging tile by tile is what left "bridge heads not at the same level" as the
	 * top rail refusal: each tile picked its own pair of heads, twelve tiles apart at
	 * most, and a river wider than that never got an approach at all. Here the whole
	 * water run on the line is measured first, then one command spans it. */
	constexpr DoCommandFlags kStruct{DoCommandFlag::Execute};
	const int dx = TileX(next) - TileX(t);
	const int dy = TileY(next) - TileY(t);
	TileIndex lo{}, hi{};
	if (!HsbWaterSpannable(t, dx, dy, &lo, &hi)) return false;
	if (!HsbBridgeSpanIsClear(lo, hi, dx, dy)) return false;
	return Command<Commands::BuildBridge>::Do(kStruct, lo, hi, TransportType::Rail, 0, HsbRailType(), INVALID_ROADTYPE).Succeeded();
}

/** Build rails on one tile, doing what it takes. */
bool HsbBuildRailTile(TileIndex t, Track along, Track across, TileIndex next = INVALID_TILE)
{
	/* Same two-flag arrangement as HsbBuildRoadTile, for the same reason: Auto lets
	 * the command demolish whatever is in the way, and a rail line routed across a
	 * factory would take the factory with it. The probe answers "could this tile be
	 * built", the real build answers "can it be built without destroying
	 * something". */
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoWater};
	constexpr DoCommandFlags kStruct{DoCommandFlag::Execute, DoCommandFlag::Auto};
	const Axis axis = (along == Track::X) ? Axis::X : Axis::Y;

	/* Checked before anything else, not inside build(), because the levelling and
	 * tunnelling attempts below run first and carry Auto too. */
	if (IsTileType(t, TileType::Industry)) {
		HsbNoteRailFailure(CommandCost(STR_ERROR_OBJECT_IN_THE_WAY), t, "industry in the way");
		return false;
	}

	auto build = [&]() {
		const bool was_rail = IsPlainRailTile(t);
		CommandCost res = Command<Commands::BuildRailLong>::Do(kExec, t, t, HsbRailType(), along, true, false);
		/* As on the road side: "already built" covers a bridge or a crossing, so the
		 * tile is checked instead of the reply believed. */
		if (res.Succeeded()) {
			if (!was_rail && IsPlainRailTile(t)) g_hsb_rail_new_tiles.push_back(t);
			return true;
		}
		if (res.GetErrorMessage() == STR_ERROR_ALREADY_BUILT && IsPlainRailTile(t) && HasTrack(t, along)) return true;
		HsbNoteRailFailure(res, t);
		return false;
	};
	/* Another line may already be here - including a junction this network made
	 * itself, which is most of them. That is not an obstacle: if the track we want is
	 * already laid, the job is done. Without this a junction refuses every later line
	 * that wants to run over it ("must remove railway track first", thousands of
	 * times), because the engine sees plain track and stops. */
	if (IsPlainRailTile(t) && HasTrack(t, along)) return true;

	if (build()) return true;
	/* A junction needs both tracks, which is what lets a train change line. */
	if (across != Track::Begin && IsPlainRailTile(t)) {
		Command<Commands::BuildRailLong>::Do(kExec, t, t, HsbRailType(), across, true, false);
		if (build()) return true;
	}
	if (GetTileType(t) == TileType::Water) {
		if (next != INVALID_TILE) {
			if (HsbSpanWaterRail(t, next) && build()) return true;
		} else {
			/* Direction unknown: try both ways along the track axis. */
			for (const int side : {1, -1}) {
				if (HsbSpanWaterRail(t, TileXY(TileX(t) + (axis == Axis::X ? side : 0),
				                              TileY(t) + (axis == Axis::X ? 0 : side))) && build()) return true;
			}
		}
		return false;
	}
	/* Level the ground only where there is no rail on it yet: levelling under
	 * existing track is refused ("must remove railway track first"), and the track
	 * is what we are here to lay. No Auto on the levelling either - it would take a
	 * building down with the hill. */
	if (GetTileSlope(t) != SLOPE_FLAT && !IsPlainRailTile(t)) {
		CommandCost lev = std::get<0>(Command<Commands::TerraformLand>::Do(kExec, t, ComplementSlope(GetTileSlope(t)), true));
		if (lev.Succeeded() && build()) return true;
		CommandCost tun = Command<Commands::BuildTunnel>::Do(kStruct, t, TransportType::Rail, HsbRailType(), INVALID_ROADTYPE);
		if (tun.Succeeded() && build()) return true;
		if (tun.Failed()) HsbNoteRailFailure(tun, t, "tunnel");
	}
	return false;
}

/** One straight rail leg, or a corner when `across` says the end is a junction. */
bool HsbBuildRailLeg(TileIndex from, TileIndex to, Axis axis)
{
	if (from == to) return true;
	if (TileX(from) != TileX(to) && TileY(from) != TileY(to)) return false;
	const Track along = axis == Axis::X ? Track::X : Track::Y;
	const Track across = axis == Axis::X ? Track::Y : Track::X;
	TileIndex t = from;
	for (uint guard = 0; guard < Map::Size(); ++guard) {
		if (!IsValidTile(t)) return false;
		/* The two end tiles take the crossing tracks, so a line can turn. */
		const bool endpoint = (t == from || t == to);
		if (!HsbBuildRailTile(t, along, endpoint ? across : Track::Begin)) return false;
		if (t == to) break;
		t += TileOffsByDiagDir(AxisToDiagDir(axis));
	}
	return true;
}

/** Can this tile carry track, allowing for a bridge or a levelled slope? */
bool HsbCarriesRail(TileIndex t)
{
	if (!IsValidTile(t)) return false;
	switch (GetTileType(t)) {
		case TileType::Clear:
		case TileType::Trees:
		case TileType::Railway:
			return true;
		case TileType::Road:
			/* A level crossing is what a railway does at a road, and the engine builds
			 * one when the settings allow it - and they are pinned on above, so the
			 * model may say yes.
			 *
			 * Measured on a 256-tile map with a hand-placed industry layout: making
			 * this a wall instead changed nothing (0 rail lines routed either way), so
			 * the crossings were never what stopped the rail, and the honest reading is
			 * that the blocker is the tile either side of them - houses and town tiles
			 * the straight-run router walks into, which the planner then has to ban and
			 * re-plan around. The rail_component count in the audit is what says what
			 * the network cost; the refusal tally names the reason. */
			return true;
		case TileType::Water:
			/* Water is a wall for the search, and that is the same lesson the road
			 * planner taught - measured twice on the rail, the second time with a
			 * *correct* bridge test: letting the search cross water where a bridge
			 * was genuinely possible took the network from 137 lines to 66. A search
			 * that may cross a river commits to long paths over it, and the build
			 * walks the whole path before failing. The straight pieces cross, with
			 * one bridge command per river. */
			return false;
		case TileType::TunnelBridge:
			/* Only a rail bridge carries the rail. Where the road network already
			 * bridged the river, the tile is a road bridge and the rail command
			 * answers "must demolish bridge first" - so treat it as the wall it is
			 * and route round. */
			return GetTunnelBridgeTransportType(t) == TransportType::Rail;
		default:
			/* A house, an industry or a station: no track, ever. */
			return false;
	}
}

/** Greedy, orthogonal-only fallback, the rail twin of HsbWalkRoad. */
bool HsbWalkRail(TileIndex from, TileIndex to)
{
	TileIndex cur = from;
	auto try_step = [&](TileIndex next) -> bool {
		if (next == cur || !IsValidTile(next)) return false;
		const bool horiz = TileY(next) == TileY(cur);
		const Track along = horiz ? Track::X : Track::Y;
		const Track across = horiz ? Track::Y : Track::X;
		return HsbBuildRailTile(cur, along, across, next) && HsbBuildRailTile(next, along, across, next);
	};
	for (uint guard = 0; guard < 4096; ++guard) {
		if (cur == to) return true;
		const int dx = (TileX(to) > TileX(cur)) - (TileX(to) < TileX(cur));
		const int dy = (TileY(to) > TileY(cur)) - (TileY(to) < TileY(cur));
		if (dx != 0) {
			TileIndex next = TileXY(static_cast<int>(TileX(cur)) + dx, TileY(cur));
			if (try_step(next)) { cur = next; continue; }
		}
		if (dy != 0) {
			TileIndex next = TileXY(TileX(cur), static_cast<int>(TileY(cur)) + dy);
			if (try_step(next)) { cur = next; continue; }
		}
		bool moved = false;
		for (const int d : {1, -1}) {
			for (const int along : {0, 1}) {
				TileIndex cand = along == 0 ? HsbTileClamped(static_cast<int>(TileX(cur)) + d, TileY(cur))
				                            : HsbTileClamped(TileX(cur), static_cast<int>(TileY(cur)) + d);
				if (!HsbDetourAllowed(HsbManhattanTiles(cur, to), HsbManhattanTiles(cand, to), 64)) continue;
				const TileType tt = GetTileType(cand);
				if (tt != TileType::Clear && tt != TileType::Trees && !IsWaterTile(cand)) continue;
				if (try_step(cand)) { cur = cand; moved = true; break; }
			}
			if (moved) break;
		}
		if (!moved) return false;
	}
	return false;
}

/**
 * Plan a path of track from A to B, build it, and re-plan where the build
 * disagrees with the plan - the same contract as the road planner, for the same
 * reason: a guessed passability model puts water and slopes on a path that then
 * dies halfway along, and a railway that stops in a river bed is worse than one
 * that goes round.
 */
bool HsbPlanAndBuildRail(TileIndex from, TileIndex to)
{
	if (from == to) return true;
	constexpr int kSteps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

	std::vector<uint8_t> banned(Map::Size(), 0);
	for (uint attempt = 0; attempt < 16; ++attempt) {
		std::vector<uint32_t> prev(Map::Size(), UINT32_MAX);
		std::vector<TileIndex> queue;
		queue.reserve(65536);
		prev[from.base()] = from.base();
		queue.push_back(from);

		bool found = false;
		for (size_t head = 0; head < queue.size() && head <= HsbSearchNodeBudget; ++head) {
			const TileIndex cur = queue[head];
			if (cur == to) { found = true; break; }
			for (const auto &d : kSteps) {
				TileIndex n = cur + TileDiffXY(d[0], d[1]);
				if (!IsValidTile(n) || prev[n.base()] != UINT32_MAX || banned[n.base()]) continue;
				if (!HsbCarriesRail(n)) continue;
				prev[n.base()] = cur.base();
				queue.push_back(n);
			}
		}
		if (!found) return HsbWalkRail(from, to);

		std::vector<TileIndex> path;
		for (TileIndex t = to; t != from; t = TileIndex(static_cast<TileIndex::BaseType>(prev[t.base()]))) {
			path.push_back(t);
		}
		const size_t mark = g_hsb_rail_new_tiles.size();
		bool ok = true;
		TileIndex bad = INVALID_TILE;
		for (size_t i = path.size(); i > 0 && ok; --i) {
			const TileIndex t = path[i - 1];
			const TileIndex next = i < path.size() ? path[i] : from;
			const bool horiz = TileY(next) == TileY(t);
			if (!HsbBuildRailTile(t, horiz ? Track::X : Track::Y, horiz ? Track::Y : Track::X, next)) {
				bad = t;
				ok = false;
			}
		}
		if (ok) return true;
		HsbUndoRailFrom(mark);
		for (TileIndex t : path) {
			if (t == from || t == to) continue;
			banned[t.base()] = 1;
		}
		if (!IsValidTile(bad) || bad == from) return HsbWalkRail(from, to);
	}
	return HsbWalkRail(from, to);
}

/**
 * Route a rail line between two points: straight runs, one corner per piece,
 * pieces at most HsbRoadRun() tiles per axis. Same shape and same reasons as the
 * road router: one L for a whole line would double its length and with it the
 * amount of terrain it has to cross.
 */
bool HsbRouteRail(TileIndex a, TileIndex b)
{
	if (a == b) return true;
	if (!IsValidTile(a) || !IsValidTile(b)) return false;

	const size_t mark = g_hsb_rail_new_tiles.size();
	auto give_up = [&]() {
		HsbUndoRailFrom(mark);
		return false;
	};

	/* Already on one axis: a single straight run. */
	if (TileX(a) == TileX(b) || TileY(a) == TileY(b)) {
		return HsbBuildRailLeg(a, b, TileY(a) == TileY(b) ? Axis::X : Axis::Y) ? true : give_up();
	}

	int x = static_cast<int>(TileX(a));
	int y = static_cast<int>(TileY(a));
	const int tx = static_cast<int>(TileX(b));
	const int ty = static_cast<int>(TileY(b));

	for (uint guard = 0; guard < 4096; ++guard) {
		if (x == tx && y == ty) break;
		const int run = HsbRoadRun();
		const int nx = HsbRunEnd(x, tx, run);
		const int ny = HsbRunEnd(y, ty, run);
		const TileIndex here = HsbTileClamped(x, y);
		const TileIndex end = HsbTileClamped(nx, ny);

		/* Both corner orders, horizontal first: one refused piece should not lose a
		 * line, and the cost of the two is the same to work out. */
		bool done = false;
		for (const bool x_first : {true, false}) {
			const TileIndex via = x_first ? HsbTileClamped(nx, y) : HsbTileClamped(x, ny);
			const size_t piece = g_hsb_rail_new_tiles.size();
			if (HsbBuildRailLeg(here, via, x_first ? Axis::X : Axis::Y) &&
			    HsbBuildRailLeg(via, end, x_first ? Axis::Y : Axis::X)) {
				done = true;
				break;
			}
			/* Undo this attempt before trying the other order. */
			constexpr DoCommandFlags kExec{DoCommandFlag::Execute, DoCommandFlag::Auto, DoCommandFlag::NoWater};
			for (size_t i = g_hsb_rail_new_tiles.size(); i > piece; --i) {
				const TileIndex t = g_hsb_rail_new_tiles[i - 1];
				if (IsValidTile(t) && IsPlainRailTile(t)) {
					Command<Commands::RemoveRail>::Do(kExec, t, Track::X);
					Command<Commands::RemoveRail>::Do(kExec, t, Track::Y);
				}
			}
			if (g_hsb_rail_new_tiles.size() > piece) g_hsb_rail_new_tiles.resize(piece);
		}
		if (!done) {
			/* The straight way is refused. Go round rather than lose the line: a main
			 * line that stops at a river is not a main line. */
			if (!HsbPlanAndBuildRail(here, end)) return give_up();
		}

		x = nx;
		y = ny;
	}
	return x == tx && y == ty;
}

/**
 * Is a depot actually joined to something?
 *
 * The build command puts the exit wherever it was told to and connects whatever is
 * on that side: a depot whose exit faces open ground is a shed in a field, and the
 * vehicle inside it never moves. The engine reports success and the map looks fine -
 * 17 train depots and 21 bus depots sat like that until a log line asked the only
 * question that matters ("no track within two tiles of the depot"). So the exit is
 * checked against the network, and a depot that fails is taken down again and tried
 * facing another way.
 */
bool HsbDepotIsConnected(TileIndex site, bool rail)
{
	if (rail) {
		if (!IsRailDepotTile(site)) return false;
		const TileIndex exit = TileIndex(site + TileOffsByDiagDir(GetRailDepotDirection(site)));
		return IsValidTile(exit) && IsPlainRailTile(exit);
	}
	if (!IsRoadDepotTile(site)) return false;
	const TileIndex exit = TileIndex(site + TileOffsByDiagDir(GetRoadDepotDirection(site)));
	return IsValidTile(exit) && IsNormalRoadTile(exit);
}

/**
 * Take a stranded depot off the map so another site can be tried.
 *
 * Only the depot's own tiles and the track it brought with it. Clearing a two-by-two
 * block blindly also cleared whatever stood next to it - 21 station platforms lost
 * their train facility this way, and a station without a facility is a platform no
 * vehicle can ever be sent to.
 */
void HsbDemolishDepot(TileIndex site)
{
	for (TileIndex t : TileArea(site, 1, 1)) {
		if (!IsValidTile(t)) continue;
		if (GetTileType(t) == TileType::Station) continue;   // never a station
		const bool ours = IsRailDepotTile(t) || IsRoadDepotTile(t) || IsShipDepotTile(t) ||
		                  IsPlainRailTile(t) || IsNormalRoadTile(t);
		if (!ours) continue;
		Command<Commands::LandscapeClear>::Do(DoCommandFlags{DoCommandFlag::Execute, DoCommandFlag::NoWater, DoCommandFlag::AllTiles}, t);
	}
}

/**
 * A "service if needed" order, so a fleet nobody manages stays healthy instead of
 * breaking down and standing in a shed for ever.
 *
 * The depot has to be named: an order with an invalid depot is refused unless it also
 * carries the "nearest depot" action, and 21 buses once sat there with a perfectly
 * serviceable depot three metres away and no way of being told.
 */
Order HsbServiceOrder(TileIndex site)
{
	/* A depot is two by two and only the entrance tile carries the depot index, so
	 * the site the command was issued on is not necessarily the tile the order has
	 * to name. Ask the four neighbours before giving up. */
	TileIndex depot = INVALID_TILE;
	for (TileIndex t : TileArea(site, 1, 1)) {
		/* GetDepotIndex asserts on a tile that is not a depot, so ask the tile what it
		 * is first - reading it blind gives a DepotID made of the m2 bits of an
		 * ordinary field. */
		if (!IsValidTile(t)) continue;
		if (!IsRoadDepotTile(t) && !IsRailDepotTile(t) && !IsShipDepotTile(t)) continue;
		if (Depot::GetIfValid(GetDepotIndex(t)) != nullptr) { depot = t; break; }
	}
	if (depot == INVALID_TILE) {
		Debug(Facility::Misc, Severity::Critical, "hsb: no depot tile found around ({},{}) for a service order", TileX(site), TileY(site));
	}
	Order order;
	/* PartOfOrders *and* Service: this depot is a stop on the route, and the vehicle
	 * is serviced there when it is due. Service on its own is refused by
	 * CmdInsertOrder with a bare error, because a service order that is not part of
	 * the route has nowhere to be. */
	OrderDepotTypeFlags type{};
	type.Set(OrderDepotTypeFlag::PartOfOrders);
	type.Set(OrderDepotTypeFlag::Service);
	order.MakeGoToDepot(GetDepotIndex(depot), type, OrderNonStopFlags{}, OrderDepotActionFlags{});
	return order;
}

/** A station order: fill up here, empty there, and wait for it to be worth it. */
Order HsbStopOrder(StationID station)
{
	Order order;
	order.MakeGoToStation(station);
	order.SetLoadType(OrderLoadType::FullLoad);
	order.SetUnloadType(OrderUnloadType::Unload);
	order.SetStopLocation(OrderStopLocation::FarEnd);
	return order;
}

/**
 * The cheapest buildable engines of a type - a starting fleet, not a showcase.
 *
 * There is no VEH_BUS: a bus is a road engine whose default cargo is passengers,
 * and a truck is the same engine type without them. Asking for the cargo rather
 * than for a "bus" flag is also what makes this work with a NewGRF that replaces
 * the base game's vehicles.
 */
std::vector<EngineID> HsbEngineCandidates(VehicleType type, bool passengers_only = false)
{
	std::vector<std::pair<Money, EngineID>> found;
	for (Engine *e : Engine::Iterate()) {
		if (e->type != type) continue;
		if (!IsEngineBuildable(e->index, type, _current_company)) continue;
		if (passengers_only && e->GetDefaultCargoType() != GetCargoTypeByLabel(CT_PASSENGERS)) continue;
		if (type == VehicleType::Train) {
			const RailVehicleInfo &rvi = e->VehInfo<RailVehicleInfo>();
			/* A wagon cannot move on its own. A multihead engine can - it is two coupled
			 * units, and the front one is the consist's primary, which is where orders
			 * have to be set. */
			if (rvi.railveh_type == RailVehicleType::Wagon) continue;
			if (!IsCompatibleRail(rvi.railtypes, HsbRailType())) continue;
			/* Same test the build command makes, against the network's rail type: an
			 * engine that cannot draw power on plain rail is refused with a bare
			 * CMD_ERROR and no message at all, and every base-game freight train below
			 * the cheapest electric one is such an engine. */
			if (!HasPowerOnRail(rvi.railtypes, HsbRailType())) continue;
		}
		found.emplace_back(e->GetCost(), e->index);
	}
	std::sort(found.begin(), found.end());
	std::vector<EngineID> out;
	out.reserve(found.size());
	for (const auto &entry : found) out.push_back(entry.second);
	return out;
}

/**
 * Buy one vehicle into a depot, cheapest suitable engine that actually goes in.
 *
 * The engine list is a *list*, not a single choice, because the engine's own build
 * command is the only honest test of whether it fits here: it is the one that knows
 * about power on the tile, about depot type, and about the rail type under the shed.
 * Anything cheaper that is refused is simply the next entry.
 */
VehicleID HsbBuyVehicle(TileIndex depot, const std::vector<EngineID> &candidates, CargoType cargo)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute};
	for (EngineID eid : candidates) {
		const auto bought = Command<Commands::BuildVehicle>::Do(kExec, depot, eid, true, cargo, ClientID::Invalid);
		const CommandCost &cost = std::get<0>(bought);
		const VehicleID vid = std::get<1>(bought);
		if (!cost.Succeeded() || vid == VehicleID::Invalid()) continue;
		/* Orders are refused against anything that is not the primary vehicle of its
		 * consist, with no message at all. A multihead engine is bought as two coupled
		 * units and the command hands back the rear one, so walk to the front: that is
		 * the vehicle the route belongs to. */
		Vehicle *v = Vehicle::Get(vid);
		while (v != nullptr && !v->IsPrimaryVehicle()) v = v->Previous();
		if (v == nullptr) {
			Debug(Facility::Misc, Severity::Critical, "hsb: vehicle {} is not part of a consist with a front", vid.base());
			continue;
		}
		return v->index;
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: no engine of {} candidate(s) fits the depot at ({},{}) (rail type {}, depot rail type {})",
		candidates.size(), TileX(depot), TileY(depot), static_cast<int>(HsbRailType()), static_cast<int>(GetRailType(depot)));
	return VehicleID::Invalid();
}

/**
 * Take a vehicle out of its depot.
 *
 * Commands::BuildVehicle leaves the new consist parked with its brakes on, and giving it
 * orders does not release it: a player has to press the start button, and the builder
 * never presses any buttons. The result is a scenario whose vehicles all have a
 * route, all pass a check for having orders, and none of them ever moves - which is
 * exactly what the fleet gate caught on the first scenario built this way
 * ("in_depot=35").
 *
 * @return whether the start command was accepted.
 */
bool HsbStartVehicle(VehicleID vid)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute};
	const CommandCost &cost = Command<Commands::StartStopVehicle>::Do(kExec, vid, false);
	if (!cost.Succeeded()) {
		const StringID why = cost.GetErrorMessage();
		Debug(Facility::Misc, Severity::Critical, "hsb: vehicle {} refused to leave the depot: {}",
		      vid.base(), why == INVALID_STRING_ID ? "no reason given" : GetString(why));
		return false;
	}
	return true;
}

/** Append an order to a vehicle's (new) order list. */
bool HsbAddOrder(VehicleID vid, const Order &order)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute};
	Vehicle *v = Vehicle::Get(vid);
	const VehicleOrderID at = v->GetNumManualOrders();
	const CommandCost res = Command<Commands::InsertOrder>::Do(kExec, vid, at, order);
	if (res.Failed()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: order refused for vehicle {} (order type {}, primary {}, engine type {}, in depot {}): {}",
			vid.base(), static_cast<int>(order.GetType()), v->IsPrimaryVehicle(),
			static_cast<int>(v->type), v->IsInDepot(),
			HsbWhy(res));
		return false;
	}
	return true;
}

struct HsbServiceCounts {
	uint bus_depots = 0, bus_depots_stranded = 0, bus_stops = 0, buses = 0, trains = 0;
};

/**
 * Bus service: a depot per real town, two stops on the town's own roads, one bus
 * shuttling between them. This is the smallest complete transport system the game
 * has and the one a new player recognises fastest - and it is also the proof that
 * the road network is actually connected, because a bus that cannot reach its own
 * stops stands in the depot for ever.
 */
void HsbBuildBusService(HsbServiceCounts &counts, uint max_towns)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute};
	uint not_started = 0;
	const std::vector<EngineID> buses = HsbEngineCandidates(VehicleType::Road, true);
	if (buses.empty()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: no bus engine available to the network owner");
		return;
	}
	/* Largest towns first: a bus in a hamlet nobody lives in is a bus standing
	 * still, and the fleet belongs where the passengers are. */
	std::vector<Town *> towns;
	for (Town *town : Town::Iterate()) {
		if (town->xy == INVALID_TILE) continue;
		if (town->cache.population < 400) continue;
		towns.push_back(town);
	}
	std::sort(towns.begin(), towns.end(), [](const Town *a, const Town *b) { return a->cache.population > b->cache.population; });
	if (towns.size() > max_towns) towns.resize(max_towns);

	StringID depot_why = INVALID_STRING_ID, stop_why = INVALID_STRING_ID;
	for (Town *town : towns) {
		/* A depot stands beside a road, two by two, exit towards the road; the engine
		 * adds the road section that connects it. Same lesson as the train depot: put
		 * the command on the road tile and it is refused. */
		TileIndex depot{};
		bool have_depot = false;
		for (TileIndex road : SpiralTileSequence(town->xy, 8, 0, 0)) {
			if (have_depot) break;
			if (!IsValidTile(road) || !IsNormalRoadTile(road)) continue;
			for (const DiagDirection dir : {DiagDirection::SW, DiagDirection::SE, DiagDirection::NW, DiagDirection::NE}) {
				TileIndex site = TileIndex(road + TileOffsByDiagDir(dir));
				if (!IsValidTile(site)) continue;
				/* Clear, flat ground only: a depot built on an industry tile converts it
				 * and the industry closes, silently and with a success return. A bus
				 * depot is four tiles, so the whole footprint has to be clear, not just
				 * the tile named. */
				if (!IsTileType(site, TileType::Clear) || GetTileSlope(site) != SLOPE_FLAT) continue;
				for (TileIndex c : TileArea(site, 1, 1)) {
					if (!IsValidTile(c) || !IsTileType(c, TileType::Clear)) { site = INVALID_TILE; break; }
				}
				if (site == INVALID_TILE) continue;
				/* Exit back towards the road; the step direction has to be reversed. */
				const CommandCost res = Command<Commands::BuildRoadDepot>::Do(kExec, site, HsbRoadType(), ReverseDiagDir(dir));
				if (!res.Succeeded()) {
					if (depot_why == INVALID_STRING_ID) depot_why = res.GetErrorMessage();
					continue;
				}
				if (!HsbDepotIsConnected(site, false)) {
					++counts.bus_depots_stranded;
					HsbDemolishDepot(site);
					continue;
				}
				depot = site;
				have_depot = true;
				++counts.bus_depots;
				break;
			}
		}
		if (!have_depot) continue;

		/* Two stops, far enough apart that the bus actually drives between them. */
		std::vector<TileIndex> stops;
		for (TileIndex road : SpiralTileSequence(town->xy, 12, 0, 0)) {
			if (stops.size() >= 2) break;
			if (!IsValidTile(road) || !IsNormalRoadTile(road)) continue;
			/* A drive-through stop needs a road that goes one way. On a junction the
			 * engine answers "drive through stops can't have junctions". */
			if (GetRoadTileType(road) != RoadTileType::Normal) continue;
			bool too_close = false;
			for (TileIndex have : stops) {
				if (HsbManhattanTiles(have, road) < 10) too_close = true;
			}
			if (too_close) continue;
			/* A drive-through stop faces along the road, so it needs the diagonal that
			 * road runs in. */
			const RoadBits rbits = GetRoadBits(road, RoadTramType::Road);
			DiagDirection dir = DiagDirection::SE;
			if (rbits == ROAD_X) {
				dir = DiagDirection::NE;
			} else if (rbits == ROAD_Y) {
				dir = DiagDirection::SE;
			} else {
				/* Fallback: pick a diagonal matching the road axis if possible. */
				if ((rbits & ROAD_X).Any()) dir = DiagDirection::NE;
				else dir = DiagDirection::SE;
			}
			const CommandCost res = Command<Commands::BuildRoadStop>::Do(kExec, road, 1, 1, RoadStopType::Bus, true,
				dir, HsbRoadType(), ROADSTOP_CLASS_DFLT, 0, StationID::Invalid(), true);
			if (res.Succeeded()) { stops.push_back(road); ++counts.bus_stops; }
			else if (stop_why == INVALID_STRING_ID) stop_why = res.GetErrorMessage();
		}
		if (stops.size() < 2) continue;

		const VehicleID vid = HsbBuyVehicle(depot, buses, GetCargoTypeByLabel(CT_PASSENGERS));
		if (vid == VehicleID::Invalid()) continue;
		++counts.buses;

		/* Pick up at one stop, empty at the other, and get serviced when due. The
		 * order list loops on its own. */
		const bool routed = HsbAddOrder(vid, HsbStopOrder(GetStationIndex(stops[0])))
			&& HsbAddOrder(vid, HsbStopOrder(GetStationIndex(stops[1])))
			&& HsbAddOrder(vid, HsbServiceOrder(depot));
		if (!routed) {
			Debug(Facility::Misc, Severity::Critical, "hsb: bus in {} has no working route - it would stand in the depot", depot);
			--counts.buses;
		} else if (!HsbStartVehicle(vid)) {
			++not_started;
		}
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: bus service: {} depots ({} taken down again), {} stops, {} buses ({} refused to start; "
		"first depot refusal: {}, first stop refusal: {})",
		counts.bus_depots, counts.bus_depots_stranded, counts.bus_stops, counts.buses, not_started,
		depot_why == INVALID_STRING_ID ? std::string("none") : GetString(depot_why),
		stop_why == INVALID_STRING_ID ? std::string("none") : GetString(stop_why));
}

/**
 * Trains that actually run.
 *
 * A train needs three things the rest of the map does not put next to each other: a
 * depot to be bought in, and two places on *one* connected network to shuttle
 * between. The freight depots stand at industries and the stations stand at towns,
 * and the rail network is not one network - most lines are routed, so a flood fill
 * from a freight depot very often reaches nothing at all. Chasing that to zero would
 * mean rebuilding the terrain.
 *
 * So the starting trains get their own infrastructure: a short spur off a station's
 * own track, a depot at the end of it, and a two-stop round that goes station,
 * depot, station. That is a complete, working service - it loads, it runs, it comes
 * back to be serviced - and it is visible from the station a new player will look at
 * first. Extending the network so freight depots reach towns is a job for the network
 * phases, not for a vehicle.
 */
void HsbBuildStationTrains(HsbServiceCounts &counts, uint wanted)
{
	const std::vector<EngineID> cargo_trains = [] {
		std::vector<EngineID> out;
		for (EngineID eid : HsbEngineCandidates(VehicleType::Train)) {
			const Engine *e = Engine::Get(eid);
			if (e->CanCarryCargo() && e->GetDisplayDefaultCapacity() > 0) out.push_back(eid);
		}
		return out;
	}();
	if (cargo_trains.empty()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: no cargo-capable train engine available to the network owner");
		return;
	}

	/* Stations, biggest town first: that is where a player looks. */
	std::vector<Station *> stations;
	for (Station *st : Station::Iterate()) {
		if (!IsRailStationTile(st->xy)) continue;
		stations.push_back(st);
	}
	std::sort(stations.begin(), stations.end(), [](const Station *a, const Station *b) {
		return (a->town != nullptr ? a->town->cache.population : 0) > (b->town != nullptr ? b->town->cache.population : 0);
	});

	uint built = 0, no_spur = 0, no_track = 0, not_started = 0;
	for (Station *st : stations) {
		if (built >= wanted) break;
		/* The track the station stands on. A station is *all* platform tiles, so the
		 * anchor is the approach line just outside it - which is why looking inside the
		 * station's own footprint found plain rail in none of them. */
		TileIndex anchor{};
		bool have_anchor = false;
		for (uint step = 1; step <= 4 && !have_anchor; ++step) {
			for (TileIndex t : SpiralTileSequence(st->xy, step, 0, 0)) {
				if (!IsValidTile(t) || !IsPlainRailTile(t)) continue;
				anchor = t;
				have_anchor = true;
				break;
			}
		}
		if (!have_anchor) { ++no_track; continue; }

		/* A depot within sight of it, exit facing the track, or nothing is built. */
		TileIndex depot{};
		bool have_depot = false;
		for (uint step = 2; step <= 14 && !have_depot; step += 2) {
			for (TileIndex t : SpiralTileSequence(anchor, step, 0, 0)) {
				if (have_depot) break;
				if (!IsValidTile(t) || !IsPlainRailTile(t)) continue;
				if (GetTileSlope(t) != SLOPE_FLAT) continue;
				for (const DiagDirection dir : {DiagDirection::SW, DiagDirection::SE, DiagDirection::NW, DiagDirection::NE}) {
					const TileIndex site = TileIndex(t + TileOffsByDiagDir(dir));
					if (!IsValidTile(site) || GetTileSlope(site) != SLOPE_FLAT) continue;
					bool on_station = false;
					for (TileIndex c : TileArea(site, 1, 1)) {
						if (IsValidTile(c) && GetTileType(c) == TileType::Station) on_station = true;
					}
					if (on_station) continue;
					/* Same two rules as the freight phase: never on a tile that is not
					 * clear ground, and the exit has to point back at the track, which is
					 * the reverse of the step that found the site. */
					if (!IsTileType(site, TileType::Clear)) continue;
					if (!Command<Commands::BuildRailDepot>::Do(DoCommandFlag::Execute, site, HsbRailType(), ReverseDiagDir(dir)).Succeeded()) continue;
					if (!HsbDepotIsConnected(site, true)) { HsbDemolishDepot(site); continue; }
					depot = site;
					have_depot = true;
					break;
				}
			}
		}
		if (!have_depot) { ++no_spur; continue; }

		const VehicleID train = HsbBuyVehicle(depot, cargo_trains, static_cast<CargoType>(0));
		if (train == VehicleID::Invalid()) { HsbDemolishDepot(depot); continue; }
		++built;

		/* Station, then the depot: the order list loops by itself, and the depot order
		 * services the train on the way round. Naming the station twice - which is what
		 * "station, depot, station" looks like - makes the game greet the player with a
		 * notification for every single train. */
		const bool routed = HsbAddOrder(train, HsbStopOrder(st->index))
			&& HsbAddOrder(train, HsbServiceOrder(depot));
		if (!routed) {
			Debug(Facility::Misc, Severity::Critical, "hsb: train for the station at ({},{}) has no working route", TileX(st->xy), TileY(st->xy));
			--built;
		} else if (!HsbStartVehicle(train)) {
			++not_started;
		}
	}
	uint with_facility = 0, station_total = 0;
	for (const Station *st : Station::Iterate()) {
		++station_total;
		if (st->facilities.Test(StationFacility::Train)) ++with_facility;
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: station trains: {} trains with spur depots ({} stations had no track to build on, "
		"{} had no room for a spur, {} refused to start); {} of {} stations carry the train facility",
		built, no_track, no_spur, not_started, with_facility, station_total);
	counts.trains = built;
}

/**
 * The built network as a character grid, for looking at rather than counting.
 *
 * Everything else this build reports is a number: 145 rail lines routed, 710 road
 * components, 1 of 124 towns reachable. A number cannot show that a corridor runs
 * through the middle of nowhere 200 tiles south of a town, and that is exactly the
 * complaint that started this - roads and rails placed in places that make no sense,
 * spotted on screen and unfalsifiable from the log.
 *
 * The distinction between R (a corridor the builder laid) and ^ (a town street) is
 * the whole point. Counting town streets alongside corridors is what made "largest
 * road component" meaningless for years.
 *
 * Off unless HSB_DUMP_NETWORK names a file: it is a diagnostic, not a gate, and
 * nothing reads it.
 */
void HsbDumpNetwork()
{
	const char *path = std::getenv("HSB_DUMP_NETWORK");
	if (path == nullptr || path[0] == '\0') return;

	/* Most specific first: a station on a bridge is still a station. */
	/* Order matters and the first version got it wrong: "is this inside a town?" was
	 * tested before the tile type, and a town claim covers roughly 20x20 tiles here,
	 * so most of the map's characters came out as town and every street and corridor
	 * inside one was invisible. Town membership is a FALLBACK for ground with nothing
	 * on it; what is on the tile wins (see HsbDumpRank). */
	const auto classify = [](int x, int y) -> char {
		const TileIndex t = TileXY(x, y);
		if (!IsValidTile(t)) return ' ';
		/* By tile type, most specific first. GetStationIndex() asserts TileType::Station and
		 * simply reads m2() otherwise, so asking it about an arbitrary tile answers
		 * with garbage - it called every tile in the map a station - and GetTownIndex()
		 * covers a town's whole claim. */
		if (IsTileType(t, TileType::Station)) return 'S';
		if (IsTileType(t, TileType::Industry)) return 'I';
		if (IsBridgeAbove(t)) return '#';
		if (IsTileType(t, TileType::Railway)) {
			if (IsRailDepotTile(t)) return 'D';
			return HasTrack(t, Track::X) && HasTrack(t, Track::Y) ? 't' : 'T';
		}
		if (IsTileType(t, TileType::Road)) {
			if (IsRoadDepotTile(t)) return 'D';
			if (IsLevelCrossing(t)) return 'r';
			if (GetRoadOwner(t, RoadTramType::Road) == OWNER_TOWN) return '^';
			return 'R';
		}
		if (IsTileType(t, TileType::Water)) return '~';
		if (GetTownIndex(t) != TownID::Invalid()) return ',';
		return '.';
	};

	constexpr uint32_t kStep = 4;
	const std::vector<std::string> rows = HsbDumpGrid(Map::SizeX(), Map::SizeY(), kStep, classify);
	std::ofstream out(path);
	if (!out) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot write the network dump to {}", path);
		return;
	}
	out << fmt::format("# hsb network dump: {}x{} tiles, {} tile(s) per character, x east, y south\n",
		Map::SizeX(), Map::SizeY(), kStep);
	for (uint32_t row = 0; row < rows.size(); ++row) {
		out << (row + 1) * kStep << '|' << rows[row] << '\n';
	}
	out.close();
	Debug(Facility::Misc, Severity::Critical, "hsb: network dump written to {}", path);
}

/**
 * Choose the tile the scenario should open on, and record it for the save.
 *
 * A headless build has no window, so there is no viewport to read a position from
 * and the save records a deliberately invalid one, which every engine - patched or
 * stock - then resolves to the map corner. The map corner of a cropped region is
 * deliberately empty.
 *
 * Opening on a town is not a nicety: the town is where the stations, the depots and
 * the buses are, so it is the only place a player can see that the scenario works at
 * all. Among the towns, the one that best shows the network wins: a town with the
 * most stations, then the biggest population. That needs no window and it is written
 * into the .scn as data, so it works on a stock engine.
 */
void HsbChooseStartView()
{
	/* Score every town: a station is worth more than population, because population
	 * is not what a player is looking at when they open a map. */
	struct Candidate {
		TileIndex tile = INVALID_TILE;
		uint stations = 0;
		uint population = 0;
	};
	std::map<TownID, Candidate> candidates;
	for (const Town *t : Town::Iterate()) {
		if (t->xy == INVALID_TILE) continue;
		candidates[static_cast<TownID>(t->index)].tile = t->xy;
		candidates[static_cast<TownID>(t->index)].population = t->cache.population;
	}
	for (const Station *s : Station::Iterate()) {
		if (s->town == nullptr) continue;
		auto it = candidates.find(static_cast<TownID>(s->town->index));
		if (it != candidates.end()) ++it->second.stations;
	}

	const Candidate *best = nullptr;
	for (const auto &kv : candidates) {
		if (best == nullptr || kv.second.stations > best->stations ||
				(kv.second.stations == best->stations && kv.second.population > best->population)) {
			best = &kv.second;
		}
	}
	if (best == nullptr || best->tile == INVALID_TILE) {
		Debug(Facility::Misc, Severity::Critical, "hsb: start view: no town to open on");
		_hsb_start_view_tile = INVALID_TILE;
		return;
	}
	_hsb_start_view_tile = best->tile;
	Debug(Facility::Misc, Severity::Critical, "hsb: start view: tile ({},{}) with {} station(s), population {}",
	      TileX(best->tile), TileY(best->tile), best->stations, best->population);
}

/**
 * Read a corridor file: an array of {name?, waypoints: [[lat,lon], ...]}.
 *
 * Shared by roads.json, autobahn.json and rail.json, which differ only in what the
 * result is fed to. Validating here rather than at each call site means one bad
 * waypoint is a build error with a named file, not a silent segment that goes
 * nowhere.
 */
bool HsbReadCorridors(const std::string &path, const char *label, std::vector<std::vector<std::pair<double, double>>> &out)
{
	std::ifstream file(path);
	if (!file) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot open {} {}", label, path);
		return false;
	}
	nlohmann::json json;
	try {
		file >> json;
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: {} parse error: {}", label, e.what());
		return false;
	}
	if (!json.is_array()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: {} root must be an array", label);
		return false;
	}
	for (const auto &corr : json) {
		std::string name = "<unnamed>";
		if (corr.contains("name")) {
			if (!corr.at("name").is_string()) {
				Debug(Facility::Misc, Severity::Critical, "hsb: {} corridor name must be a string", label);
				return false;
			}
			name = corr.at("name").get<std::string>();
		}
		if (!corr.contains("waypoints") || !corr.at("waypoints").is_array() || corr.at("waypoints").size() < 2) {
			Debug(Facility::Misc, Severity::Critical, "hsb: {} corridor '{}' needs a waypoints array of at least two [lat,lon] pairs", label, name);
			return false;
		}
		std::vector<std::pair<double, double>> wps;
		for (const auto &wp : corr.at("waypoints")) {
			if (!wp.is_array() || wp.size() != 2 || !wp.at(0).is_number() || !wp.at(1).is_number()) {
				Debug(Facility::Misc, Severity::Critical, "hsb: {} corridor '{}' waypoints must be numeric [lat, lon] pairs", label, name);
				return false;
			}
			const double lat = wp.at(0).get<double>();
			const double lon = wp.at(1).get<double>();
			if (!HsbCoordinateInBounds(g_hsb_bounds, lat, lon)) {
				Debug(Facility::Misc, Severity::Critical, "hsb: {} corridor '{}' waypoint ({},{}) is outside the manifest bounds", label, name, lat, lon);
				return false;
			}
			wps.emplace_back(lat, lon);
		}
		out.push_back(std::move(wps));
	}
	return true;
}

/**
 * Real station positions from stations.json, keyed by the town they serve.
 *
 * The file is output from scripts/build-osm-network.py: one entry per town,
 * with the station's real lat/lon. It is optional; without it a station is
 * placed on the first rail tile the town's spiral finds, exactly as before.
 */
bool HsbReadStationAnchors(const std::string &path)
{
	std::ifstream file(path);
	if (!file) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot open stations {}", path);
		return false;
	}
	nlohmann::json json;
	try {
		file >> json;
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: stations parse error: {}", e.what());
		return false;
	}
	if (!json.is_array()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: stations root must be an array");
		return false;
	}
	g_hsb_station_anchors.clear();
	for (const auto &entry : json) {
		if (!entry.contains("town") || !entry.at("town").is_string()
				|| !entry.contains("lat") || !entry.at("lat").is_number()
				|| !entry.contains("lon") || !entry.at("lon").is_number()) {
			Debug(Facility::Misc, Severity::Critical, "hsb: stations entry needs town, lat and lon");
			return false;
		}
		const std::string town = entry.at("town").get<std::string>();
		const double lat = entry.at("lat").get<double>();
		const double lon = entry.at("lon").get<double>();
		if (!HsbCoordinateInBounds(g_hsb_bounds, lat, lon)) {
			Debug(Facility::Misc, Severity::Critical, "hsb: station for '{}' at ({},{}) is outside the manifest bounds", town, lat, lon);
			return false;
		}
		const std::string name = entry.contains("name") && entry.at("name").is_string()
			? entry.at("name").get<std::string>() : town;
		g_hsb_station_anchors[town] = HsbStationAnchor{lat, lon, name};
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: station anchors loaded: {}", g_hsb_station_anchors.size());
	return true;
}

/** A point-of-interest file: an array of {name, xy: [lat, lon]}. */
bool HsbReadPlaces(const std::string &path, const char *label,
	std::vector<std::pair<std::string, std::pair<double, double>>> &out)
{
	std::ifstream file(path);
	if (!file) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot open {} {}", label, path);
		return false;
	}
	nlohmann::json json;
	try {
		file >> json;
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: {} parse error: {}", label, e.what());
		return false;
	}
	if (!json.is_array()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: {} root must be an array", label);
		return false;
	}
	for (const auto &entry : json) {
		std::string name = "<unnamed>";
		if (entry.contains("name")) {
			if (!entry.at("name").is_string()) {
				Debug(Facility::Misc, Severity::Critical, "hsb: {} entry name must be a string", label);
				return false;
			}
			name = entry.at("name").get<std::string>();
		}
		if (!entry.contains("xy") || !entry.at("xy").is_array() || entry.at("xy").size() != 2
				|| !entry.at("xy").at(0).is_number() || !entry.at("xy").at(1).is_number()) {
			Debug(Facility::Misc, Severity::Critical, "hsb: {} entry '{}' needs xy: [lat, lon] numbers", label, name);
			return false;
		}
		const double lat = entry.at("xy").at(0).get<double>();
		const double lon = entry.at("xy").at(1).get<double>();
		if (!HsbCoordinateInBounds(g_hsb_bounds, lat, lon)) {
			Debug(Facility::Misc, Severity::Critical, "hsb: {} entry '{}' at ({},{}) is outside the manifest bounds", label, name, lat, lon);
			return false;
		}
		out.emplace_back(name, std::make_pair(lat, lon));
	}
	return true;
}

/**
 * Route a whole corridor file, snapping endpoints out of town centres and off the
 * water, and collecting the links that failed for a retry once the rest of the
 * network exists.
 */
uint HsbRouteCorridors(const std::vector<std::vector<std::pair<double, double>>> &corridors,
	uint &routed, uint &snapped, uint &unsnappable, std::vector<std::pair<TileIndex, TileIndex>> &retry)
{
	const uint before = routed;
	for (const auto &wps : corridors) {
		TileIndex prev{};
		bool have_prev = false;
		for (const auto &wp : wps) {
			const TileIndex want = HsbTileFor(wp.first, wp.second);
			bool moved = false;
			const TileIndex cur = HsbSnapBuildable(want, static_cast<uint>(kHsbEndpointSnap.fallback), &moved);
			if (moved) {
				++snapped;
			} else if (!IsValidTile(cur) || GetTileType(cur) == TileType::Void || GetTileType(cur) == TileType::Water) {
				/* Nowhere to stand: every link to this point will fail, and it would look
				 * like a routing problem rather than an endpoint one. */
				++unsnappable;
				Debug(Facility::Misc, Severity::Critical, "hsb: no buildable ground within {} tiles of ({},{}) for a corridor endpoint, type {}",
					kHsbEndpointSnap.fallback, TileX(want), TileY(want), GetTileType(cur));
			}
			if (!IsValidTile(cur)) { have_prev = false; continue; }
			if (have_prev && cur != prev) {
				const Town *nt = ClosestTownFromTile(cur, UINT_MAX);
				const TownID tid = (nt == nullptr) ? TownID::Invalid() : nt->index;
				if (HsbRouteRoad(prev, cur, tid)) {
					++routed;
				} else {
					retry.emplace_back(prev, cur);
				}
			}
			prev = cur;
			have_prev = true;
		}
	}
	return routed - before;
}

/**
 * Retry links that failed earlier.
 *
 * A corridor fails early because the ground it wants is not there yet - a bank to
 * bridge, a gap to walk round, a neighbouring road to join - and most of them find
 * it after the other hundred-odd links have been built.
 *
 * @return the number still unrouted.
 */
uint HsbRetryRoadLinks(std::vector<std::pair<TileIndex, TileIndex>> &links)
{
	for (uint pass = 1; pass <= 6 && !links.empty(); ++pass) {
		std::vector<std::pair<TileIndex, TileIndex>> still;
		uint got = 0;
		for (const auto &link : links) {
			const Town *nt = ClosestTownFromTile(link.second, UINT_MAX);
			const TownID tid = (nt == nullptr) ? TownID::Invalid() : nt->index;
			if (HsbRouteRoad(link.first, link.second, tid)) {
				++got;
			} else {
				still.push_back(link);
			}
		}
		Debug(Facility::Misc, Severity::Critical, "hsb: retry pass {}: {} links routed, {} still unrouted", pass, got, still.size());
		if (got == 0) break;
		links.swap(still);
	}
	return static_cast<uint>(links.size());
}

/**
 * A spur from the nearest existing road to the edge of every land industry.
 *
 * Target a buildable tile next to the industry, not the industry tile itself:
 * Commands::BuildRoad refuses anything but bare land.
 */
void HsbBuildIndustrySpurs(uint &spur_ok, uint &spur_missed)
{
	for (const Industry *ind : Industry::Iterate()) {
		if (ind->location.tile == INVALID_TILE) continue;
		const IndustrySpec *spec = GetIndustrySpec(ind->type);
		const std::string name = (spec == nullptr) ? "<unknown>" : GetString(spec->name);
		if (IsWaterTile(ind->location.tile)) continue;

		TileIndex target{};
		bool found = false;
		for (TileIndex t : SpiralTileSequence(ind->location.tile, 6, 0, 0)) {
			if (!IsValidTile(t) || IsTileType(t, TileType::TunnelBridge)) continue;
			if (GetTileType(t) != TileType::Clear || GetTileSlope(t) != SLOPE_FLAT) continue;
			target = t;
			found = true;
			break;
		}
		if (!found) { ++spur_missed; continue; }

		/* Start from the nearest road already on the map. */
		TileIndex from{};
		found = false;
		for (TileIndex t : SpiralTileSequence(target, 48, 0, 0)) {
			if (!IsValidTile(t) || !IsNormalRoadTile(t)) continue;
			from = t;
			found = true;
			break;
		}
		if (!found) { ++spur_missed; continue; }

		const Town *nt = ClosestTownFromTile(target, UINT_MAX);
		const TownID tid = (nt == nullptr) ? TownID::Invalid() : nt->index;
		if (HsbRouteRoad(from, target, tid)) {
			++spur_ok;
		} else {
			Debug(Facility::Misc, Severity::Critical, "hsb: industry spur failed near '{}' at tile ({},{})",
				name, TileX(target), TileY(target));
			++spur_missed;
		}
	}
}

/**
 * A siding to the industries worth rail, with a depot at the end so an arriving
 * train has somewhere to be serviced.
 */
uint HsbBuildFreightSidings(uint &depots, uint &stranded)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute};
	uint sidings = 0;
	StringID depot_why = INVALID_STRING_ID;
	for (const Industry *ind : Industry::Iterate()) {
		if (ind->location.tile == INVALID_TILE || IsWaterTile(ind->location.tile)) continue;
		const IndustrySpec *spec = GetIndustrySpec(ind->type);
		const std::string iname = (spec == nullptr) ? std::string("<unknown>") : GetString(spec->name);
		if (!HsbIndustryWantsRail(iname)) continue;

		TileIndex target{};
		bool have_target = false;
		for (TileIndex t : SpiralTileSequence(ind->location.tile, 6, 0, 0)) {
			if (!IsValidTile(t)) continue;
			if (GetTileType(t) != TileType::Clear || GetTileSlope(t) != SLOPE_FLAT) continue;
			target = t;
			have_target = true;
			break;
		}
		if (!have_target) continue;

		TileIndex from{};
		have_target = false;
		for (TileIndex t : SpiralTileSequence(target, 80, 0, 0)) {
			if (!IsValidTile(t) || !IsPlainRailTile(t)) continue;
			from = t;
			have_target = true;
			break;
		}
		if (!have_target) continue;
		if (!HsbRouteRail(from, target)) continue;
		++sidings;

		bool placed = false;
		/* The exit has to face the track, so every combination of site and direction is
		 * tried and the result is checked rather than assumed. */
		for (TileIndex t : SpiralTileSequence(target, 8, 0, 0)) {
			if (placed) break;
			if (!IsValidTile(t) || !IsPlainRailTile(t)) continue;
			for (const DiagDirection dir : {DiagDirection::SW, DiagDirection::SE, DiagDirection::NW, DiagDirection::NE}) {
				const TileIndex site = TileIndex(t + TileOffsByDiagDir(dir));
				if (!IsValidTile(site)) continue;
				/* Clear, flat ground only, and this is load-bearing rather than fussy. A
				 * train depot is a single tile and the engine builds it by calling
				 * SetTileType() on it directly; Commands::LandscapeClear, which runs first,
				 * does not refuse an industry. A depot sited on an industry tile
				 * therefore converts that tile and orphans the industry, which then
				 * closes - and the depot command reports success throughout. */
				if (!IsTileType(site, TileType::Clear) || GetTileSlope(site) != SLOPE_FLAT) continue;
				/* The exit has to point back at the track, so the command is given the
				 * opposite of the step that got us here. Passing `dir` instead aims the
				 * exit one tile past the track, which is how 368 depots were built, found
				 * unconnected and taken down again while the log blamed the site rather
				 * than the direction. */
				const CommandCost res = Command<Commands::BuildRailDepot>::Do(kExec, site, HsbRailType(), ReverseDiagDir(dir));
				if (!res.Succeeded()) {
					if (depot_why == INVALID_STRING_ID) depot_why = res.GetErrorMessage();
					continue;
				}
				if (!HsbDepotIsConnected(site, true)) {
					++stranded;
					HsbDemolishDepot(site);
					continue;
				}
				++depots;
				placed = true;
				break;
			}
		}
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: rail freight: {} sidings, {} depots, {} taken down again for want of a "
		"connected exit, first refusal: {}", sidings, depots, stranded,
		depot_why == INVALID_STRING_ID ? std::string("none") : GetString(depot_why));
	return sidings;
}

/**
 * A rail station where a line meets a town.
 *
 * Two platforms and a passing loop is the smallest thing that takes a train, and
 * what a branch line has.
 *
 * `adjacent` is false: with it, a station built next to an existing one is merged
 * into it and the command still reports success, so the build log said 61 stations
 * while 21 of them were empty shells no vehicle could ever be sent to. One station
 * per place is what a scenario wants anyway.
 */
uint HsbBuildStations(uint &towns_without_rail)
{
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute};
	uint rail_stations = 0;
	StringID station_why = INVALID_STRING_ID;
	for (const Town *town : Town::Iterate()) {
		if (town->xy == INVALID_TILE) continue;
		/* Reach further than a town's own radius: the railway runs beside the town,
		 * not through it, and a station belongs on the line. With OSM stations,
		 * the spiral starts at the station's real position instead of the town
		 * centre; the search still finds the nearest track to it. */
		TileIndex anchor = town->xy;
		const auto station_it = g_hsb_station_anchors.find(town->name);
		if (station_it != g_hsb_station_anchors.end()) {
			anchor = HsbTileFor(station_it->second.lat, station_it->second.lon);
		}
		TileIndex spot{};
		bool have_spot = false;
		for (TileIndex t : SpiralTileSequence(anchor, 14, 0, 0)) {
			if (!IsValidTile(t) || !IsPlainRailTile(t)) continue;
			spot = t;
			have_spot = true;
			break;
		}
		if (!have_spot) {
			++towns_without_rail;
			continue;
		}
		const Axis axis = HasTrack(spot, Track::X) && !HasTrack(spot, Track::Y) ? Axis::X : Axis::Y;
		/* No DoCommandFlag::Auto: a tested station command dry-runs its clear-tile
		 * check, and on a plain track tile that dry run answers "must remove railway
		 * track first" - which is the one thing a station is *for*. Building for real
		 * replaces the track, exactly as it does under the player's cursor. */
		const CommandCost res = Command<Commands::BuildRailStation>::Do(kExec, spot, HsbRailType(), axis, 2, 4,
			STAT_CLASS_DFLT, 0, StationID::Invalid(), false);
		if (res.Succeeded()) {
			/* And the count is of stations that exist and can be used. */
			const Station *made = Station::GetIfValid(GetStationIndex(spot));
			if (made != nullptr && made->facilities.Test(StationFacility::Train)) {
				++rail_stations;
			} else {
				Debug(Facility::Misc, Severity::Critical, "hsb: station at ({},{}) was built but cannot take a train", TileX(spot), TileY(spot));
			}
		} else if (station_why == INVALID_STRING_ID) {
			station_why = res.GetErrorMessage();
		}
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: rail: {} stations built, {} towns with no rail within 14 tiles, first refusal: {}",
		rail_stations, towns_without_rail,
		station_why == INVALID_STRING_ID ? std::string("none") : GetString(station_why));
	return rail_stations;
}

/**
 * An airport where the data says one is.
 *
 * The list is data, and it is a real airport map rather than a rule about which
 * cities deserve one - an earlier version picked "the most isolated big towns" and
 * put runways where no airport has ever been. The position is the airport's real
 * position, because an airport is a big flat field and belongs outside its town.
 *
 * A runway needs a lot of flat, clear, unbuilt ground, so the search is wide and
 * only accepts land that takes an airport: the command decides, which is the only
 * honest test.
 */
uint HsbBuildAirports(const std::string &path)
{
	std::vector<std::pair<std::string, std::pair<double, double>>> places;
	if (!HsbReadPlaces(path, "airports", places)) return 0;

	/* The probe tests with DoCommandFlag::Auto on purpose. The build that follows
	 * must not: Auto means "test", and in test mode the command dry-runs its
	 * clear-tile check, which then refuses a site the test just approved. That is
	 * why four real airports were found a spot for and then not built on it. */
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute};
	uint built = 0;
	for (const auto &entry : places) {
		const std::string &name = entry.first;
		const TileIndex want = HsbTileFor(entry.second.first, entry.second.second);

		TileIndex spot{};
		bool found = false;
		for (uint radius = 2; radius <= static_cast<uint>(kHsbSiteSearchRadius.hi) && !found; radius += 2) {
			for (TileIndex t : SpiralTileSequence(want, radius, 0, 0)) {
				if (!IsValidTile(t)) continue;
				if (!Command<Commands::BuildAirport>::Do(DoCommandFlag::Auto, t, HsbAirportType(), HsbAirportLayout(),
				                                     StationID::Invalid(), true).Succeeded()) continue;
				spot = t;
				found = true;
				break;
			}
		}
		if (!found) {
			Debug(Facility::Misc, Severity::Critical, "hsb: no room for an airport near '{}' at ({},{})", name, TileX(want), TileY(want));
			continue;
		}
		if (Command<Commands::BuildAirport>::Do(kExec, spot, HsbAirportType(), HsbAirportLayout(),
		                                   StationID::Invalid(), true).Succeeded()) {
			++built;
		} else {
			Debug(Facility::Misc, Severity::Critical, "hsb: airport '{}' refused at ({},{})", name, TileX(spot), TileY(spot));
		}
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: airports: {} built", built);
	return built;
}

/**
 * A dock for every coastal town and every port the data names.
 *
 * A dock is only useful where a ship can reach it and a road can reach the town,
 * so the search looks for water along the coast and lets the command decide, exactly
 * as the airports do.
 *
 * A dock is not built *on* the water: CmdBuildDock wants a land tile with a slope
 * that points at a flat water tile, so the search starts from the water and tries
 * the four tiles it touches. Searching land tiles on its own finds nothing at all.
 */
uint HsbBuildHarbours(const std::string &path)
{
	std::vector<std::pair<std::string, std::pair<double, double>>> places;
	if (!HsbReadPlaces(path, "harbours", places)) return 0;

	/* No NoWater here: Commands::BuildDock's own flags are {Auto} only, and adding the
	 * construction flag the road command uses forbids the dock outright ("can't build
	 * on water") on every tile of the coast. */
	constexpr DoCommandFlags kExec{DoCommandFlag::Execute, DoCommandFlag::Auto};
	uint built = 0;
	for (const auto &entry : places) {
		const std::string &name = entry.first;
		const TileIndex want = HsbTileFor(entry.second.first, entry.second.second);
		TileIndex spot{};
		bool found = false;
		for (uint radius = 1; radius <= static_cast<uint>(kHsbSiteSearchRadius.hi) && !found; ++radius) {
			for (TileIndex t : SpiralTileSequence(want, radius, 0, 0)) {
				if (!IsValidTile(t) || !IsWaterTile(t)) continue;
				for (int d = 0; d < 4 && !found; ++d) {
					static const int kOff[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
					TileIndex land = t + TileDiffXY(kOff[d][0], kOff[d][1]);
					if (!IsValidTile(land) || IsWaterTile(land)) continue;
					if (!Command<Commands::BuildDock>::Do(DoCommandFlag::Auto, land, StationID::Invalid(), true).Succeeded()) continue;
					spot = land;
					found = true;
				}
			}
		}
		if (!found) {
			Debug(Facility::Misc, Severity::Critical, "hsb: no harbour for '{}' near ({},{})", name, TileX(want), TileY(want));
			continue;
		}
		CommandCost res = Command<Commands::BuildDock>::Do(kExec, spot, StationID::Invalid(), true);
		if (res.Succeeded()) {
			++built;
		} else {
			Debug(Facility::Misc, Severity::Critical, "hsb: harbour '{}' found a spot at ({},{}) but the build refused: {}",
				name, TileX(spot), TileY(spot), HsbWhy(res));
		}
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: harbours: {} docks built", built);
	return built;
}

/**
 * Borrow a company to own the network.
 *
 * The infrastructure phases ask the current company for its rail types, for its
 * money, and for the owner of a new station. While a scenario is being built there
 * is no company to ask - the player's company is created when the scenario is
 * *loaded* - so the first rail tile dereferences nothing and the builder dies.
 *
 * is_ai = true is the whole reason: on game start OpenTTD calls
 * GetFirstPlayableCompanyID(), which returns "the first existing *human* company" -
 * and a human company that exists in the save is handed to the player. A player
 * landing as the network's owner with somebody else's fleet, no company dialog and no
 * way to start fresh, is the "no company in the save" bug wearing a different hat.
 * Flagged as AI, the company keeps its network and the player is offered the free
 * slot.
 */
CompanyID HsbBorrowCompany()
{
	Company *company = DoStartupNewCompany(true);
	if (company == nullptr) {
		Debug(Facility::Misc, Severity::Critical, "hsb: no company slot to borrow - the network cannot be built");
		return CompanyID::Invalid();
	}
	g_hsb_net.network_company = company->index;
	_current_company = _local_company = company->index;
	/* The line network is several hundred thousand track tiles, which is far more
	 * money than a starting company has. The money is given away again before the
	 * save - the company is handed back with its loan at zero - so the balance never
	 * reaches a scenario; without this the build stops at "not enough cash"
	 * somewhere in the middle of the network. Enough to run the fleet for years,
	 * small enough that the number on the money display means something. */
	company->money = static_cast<Money>(g_hsb_net.money);
	company->current_loan = 0;
	/* Which rail and road types this company may use. StartupEngines() normally hands
	 * every company these bitmasks when the game starts - which is *after* world
	 * generation, so a company created by the builder has none, and every road vehicle
	 * is "not available" while the road itself is perfectly buildable. That is the
	 * whole reason the bus service came up with no bus engine and no explanation. */
	company->avail_railtypes = GetCompanyRailTypes(company->index);
	company->avail_roadtypes = GetCompanyRoadTypes(company->index);
	Debug(Facility::Misc, Severity::Critical, "hsb: borrowed company {} with {} cash", company->index.base(), g_hsb_net.money);
	return company->index;
}

/**
 * Bring the engine list up to the scenario date and give the borrowed company the
 * engines a game start would have handed it.
 *
 * Engines are aged and made available in StartupEngines(), which the game runs
 * *after* world generation, so during the build every engine is "not available" and
 * IsEngineBuildable() refuses all of them - the symptom being a scenario with depots
 * and no vehicles, and no reason printed by the engine at all.
 *
 * The scenario editor does the per-company part with Commands::EngineControl, and the
 * command it calls is one line: set this company's bit in the engine's availability
 * mask. World generation does not do it - the masks are per company and this company
 * did not exist when they were filled in.
 */
void HsbHandOutEngines()
{
	StartupEngines();
	if (!Company::IsValidID(g_hsb_net.network_company)) return;
	for (Engine *e : Engine::Iterate()) {
		if (e->flags.Test(EngineFlag::Available)) e->company_avail.Set(g_hsb_net.network_company);
	}
}

} // namespace

bool HsbBuildRequested()
{
	static bool done = false;
	if (done) return false;
	if (HsbManifestPath() == nullptr) return false;
	if (_game_mode != GameMode::Editor || _switch_mode != SwitchMode::None) return false;
	done = true;
	return true;
}

bool HsbBuildScenario()
{
	/* First line of every build, before anything is measured: the configuration
	 * this log is about. It has to come first because it is the one line a
	 * parameter sweep cannot do without - a sweep runs a dozen builds side by
	 * side, and without this line two of them are the same log. */
	HsbLogTuning();

	const char *manifest_path = HsbManifestPath();
	if (manifest_path == nullptr) return false;

	std::ifstream manifest_file(manifest_path);
	if (!manifest_file) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot open manifest {}", manifest_path);
		return false;
	}
	nlohmann::json manifest;
	try {
		manifest_file >> manifest;
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest parse error: {}", e.what());
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
			Debug(Facility::Misc, Severity::Critical, "hsb: manifest fields have invalid types");
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
		Debug(Facility::Misc, Severity::Critical, "hsb: invalid manifest fields: {}", e.what());
		return false;
	}
	if (heightmap.empty() || towns.empty() || industries_path.empty() || !HsbSafeFilename(save)
			|| !std::filesystem::path(heightmap).is_absolute()
			|| !std::filesystem::path(towns).is_absolute()
			|| !std::filesystem::path(industries_path).is_absolute()
			|| !HsbSafeFilename(BasenameOf(heightmap))
			|| !HsbSafeFilename(BasenameOf(towns))) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest paths or save filename are invalid");
		return false;
	}
	if (start_year < 0 || start_year > 5'000'000 || heightmap_height < 1 || heightmap_height > static_cast<int>(MAX_TILE_HEIGHT)
			|| snow_line_height < 0 || snow_line_height > static_cast<int>(MAX_TILE_HEIGHT)
			|| industry_search_radius < 1 || industry_search_radius > 512 || generation_seed == UINT32_MAX
			|| max_loan < 0 || max_loan > 4'000'000'000 || competitors < 0 || competitors > 15) {
		Debug(Facility::Misc, Severity::Critical, "hsb: manifest year, height, snow line, industry radius, loan, or competitors is out of range");
		return false;
	}
	g_hsb_industry_radius = static_cast<uint>(industry_search_radius);
	if (rotation == "clockwise") {
		g_hsb_counter_clockwise = false;
	} else if (rotation == "counter-clockwise" || rotation == "counter_clockwise" || rotation == "counterclockwise") {
		g_hsb_counter_clockwise = true;
	} else {
		Debug(Facility::Misc, Severity::Critical, "hsb: unknown heightmap_rotation '{}', want clockwise or counter-clockwise", rotation);
		return false;
	}

	size_t expected_towns = 0;
	if (!HsbValidateTownData(towns, expected_towns)) return false;
	if (!HsbParseNetworkSettings(manifest, competitors)) return false;

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
		for (uint8_t i = 0; i < to_underlying(Currency::End); i++) {
			if (currency == _currency_specs[static_cast<Currency>(i)].code) {
				_settings_newgame.locale.currency = static_cast<Currency>(i);
				found = true;
				break;
			}
		}
		if (!found) {
			Debug(Facility::Misc, Severity::Critical, "hsb: unknown currency code '{}'", currency);
			return false;
		}
	}
	/* Construction settings, pinned and set BEFORE GenerateWorld.
	 *
	 * GenerateWorld copies _settings_newgame into _settings_game, so anything set
	 * afterwards never reaches the live settings - and the road command asks
	 * _settings_game.construction.build_on_slopes before it will cut a foundation.
	 * Setting it after world generation is silently a no-op, and the symptom is
	 * 270 "Site unsuitable for tunnel entrance" refusals: with the flag off, the
	 * road command refuses any sloped tile, the explicit level fails for the same
	 * reason, and the tunnel needs an entrance the terrain has not got.
	 *
	 * These are difficulty settings baked into the scenario's newgame block, so
	 * they change what the player can do too - which is why they are pinnable
	 * rather than inherited from the builder's own config.
	 *
	 *  - build_on_slopes: on a heightmap with a dozen height units almost every
	 *    corridor crosses a slope, so a road network is only possible if the
	 *    terrain is levelled under it.
	 *  - autoslope: rail_cmd.cpp's CheckRailSlope answers "must remove railway
	 *    track first" when it is off, which reads like a track conflict and is not
	 *    one at all - it checks AutoslopeEnabled() first and gives up without ever
	 *    looking at the track. The same gate stops roads, stations, depots,
	 *    bridges, tunnels, industries and towns from building on slopes.
	 *  - crossing_with_competitor: the scenario's road network is town-owned (that
	 *    is what makes a town grow along it) and the railway crosses those roads
	 *    constantly. A crossing is what a railway does at a road.
	 *  - road_stop_on_town_road: what makes a bus stop on a town's own street
	 *    possible at all.
	 *
	 * Pinned only when the network is requested. A manifest that builds no roads
	 * gets whatever the -c config says, because turning a difficulty setting on
	 * for a scenario that has no network is a change nobody asked for - but a
	 * manifest that builds roads cannot inherit it, because that value is machine
	 * state and it decides whether the network is possible at all.
	 */
	if (HsbNetworkIsRequested()) {
		_settings_newgame.construction.build_on_slopes = g_hsb_net.build_on_slopes;
		_settings_newgame.construction.autoslope = g_hsb_net.autoslope;
		_settings_newgame.construction.crossing_with_competitor = true;
		_settings_newgame.construction.road_stop_on_town_road = true;
	}
	/* Selective servicing OFF: with it on, industries never deliver to a station
	 * no vehicle has loaded at yet - a permanent deadlock for every new
	 * full-load route. */
	_settings_newgame.order.selectgoods = false;

	/* 1. Terrain: same as the GUI heightmap flow (dimensions from the PNG). */
	uint dim_x = 0, dim_y = 0;
	if (!GetHeightmapDimensions(DetailedFileType::HeightmapPng, BasenameOf(heightmap), &dim_x, &dim_y)) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot read heightmap {}", heightmap);
		return false;
	}
	if (!HsbIsPowerOfTwo(dim_x, MIN_MAP_SIZE, MAX_MAP_SIZE)
			|| !HsbIsPowerOfTwo(dim_y, MIN_MAP_SIZE, MAX_MAP_SIZE) || dim_x != dim_y) {
		Debug(Facility::Misc, Severity::Critical, "hsb: heightmap must be square powers of 2 in 64..4096, got {}x{}", dim_x, dim_y);
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
	_file_to_saveload.ftype = FiosType{AbstractFileType::Heightmap, DetailedFileType::HeightmapPng};
	Debug(Facility::Misc, Severity::Critical, "hsb: generating {}x{} terrain, height {} rotation {}", dim_x, dim_y, heightmap_height, rotation);
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
	Debug(Facility::Misc, Severity::Critical, "hsb: towns placed: {}/{}", towns_placed, expected_towns);
	for (const Sign *s : Sign::Iterate()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: town fallback sign: {} at {},{}", s->name, s->x, s->y);
	}
	if (towns_placed != expected_towns) {
		Debug(Facility::Misc, Severity::Critical, "hsb: town import incomplete, aborting");
		return false;
	}

	/* 3. Industries: resolve types by display name, place at anchors. */
	std::ifstream ind_file(industries_path);
	if (!ind_file) {
		Debug(Facility::Misc, Severity::Critical, "hsb: cannot open {}", industries_path);
		return false;
	}
	nlohmann::json ind_json;
	try {
		ind_file >> ind_json;
	} catch (nlohmann::json::exception &e) {
		Debug(Facility::Misc, Severity::Critical, "hsb: industries parse error: {}", e.what());
		return false;
	}
	if (!ind_json.is_array()) {
		Debug(Facility::Misc, Severity::Critical, "hsb: industries root must be an array");
		return false;
	}

	std::map<std::string, IndustryType> by_name;
	for (IndustryType it : _sorted_industry_types) {
		const IndustrySpec *spec = GetIndustrySpec(it);
		if (!spec->enabled) continue;
		by_name.emplace(GetString(spec->name), it);
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: {} industry types available", by_name.size());
	for (const auto &named : by_name) {
		Debug(Facility::Misc, Severity::Critical, "hsb: available industry '{}'", named.first);
	}

	uint placed = 0, failed = 0, idx = 0;
	/* Mirror the editor GUI (industry_gui.cpp OnPlaceObject): build as
	 * OWNER_NONE with industry restrictions bypassed.
	 *
	 * Scoped in a block, deliberately. These three are function-wide in the rest
	 * of the engine's callers, and here that leaked into the network phase: the
	 * backups restore at the end of the enclosing scope, which was the end of
	 * HsbBuildScenario, so every road, station, depot and vehicle below was built
	 * with _current_company still OWNER_NONE and _generating_world still true.
	 * The engine answered by creating a second company to own the fleet, and the
	 * save then held a network nobody owned plus a company the player was handed. */
	{
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
					Debug(Facility::Misc, Severity::Critical, "hsb: invalid industry entry schema");
					return false;
				}
				const std::string type = entry.at("type").get<std::string>();
				const double lat = entry.at("lat").get<double>();
				const double lon = entry.at("lon").get<double>();
				const std::string near = entry.contains("near") ? entry.at("near").get<std::string>() : type;
				if (type.empty() || !HsbCoordinateInBounds(g_hsb_bounds, lat, lon)) {
					Debug(Facility::Misc, Severity::Critical, "hsb: invalid industry type or coordinates");
					return false;
				}
				auto it = by_name.find(type);
				if (it == by_name.end()) {
					Debug(Facility::Misc, Severity::Critical, "hsb: unknown industry type '{}' near {}", type, near);
					++failed;
					continue;
				}
				TileIndex target = HsbTileFor(lat, lon);
				Town *nearest = ClosestTownFromTile(target, UINT_MAX);
				std::string nearest_name = (nearest == nullptr) ? "<none>" : nearest->name;
				bool ok = false;
				std::map<std::string, uint> err_counts;
				CommandCost anchor_result = Command<Commands::BuildIndustry>::Do(DoCommandFlag::Execute, target, it->second, 0, false, idx);
				if (anchor_result.Succeeded()) {
					ok = true;
				} else {
					err_counts[HsbWhy(anchor_result)]++;
				}
				if (!ok) {
					for (TileIndex t : SpiralTileSequence(target, g_hsb_industry_radius, 0, 0)) {
						CommandCost res = Command<Commands::BuildIndustry>::Do(DoCommandFlag::Execute, t, it->second, 0, false, idx);
						if (res.Succeeded()) {
							ok = true;
							break;
						}
						err_counts[HsbWhy(res)]++;
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
					Debug(Facility::Misc, Severity::Critical, "hsb: could not place '{}' near {} (tile {},{} nearest town {}): {}", type, near,
						TileX(target), TileY(target), nearest_name, top);
					++failed;
				}
			}
		} catch (nlohmann::json::exception &e) {
			Debug(Facility::Misc, Severity::Critical, "hsb: invalid industry entry: {}", e.what());
			return false;
		}
	}   /* _current_company, _generating_world and _ignore_industry_restrictions restored */

	Debug(Facility::Misc, Severity::Critical, "hsb: industries placed {}/{}, failed {}", placed, ind_json.size(), failed);
	HsbLogIndustries("after placing industries");
	if (failed > 0) return false;
	/* 3b. Network: roads, rail, airports, docks and a starting fleet.
	 *
	 * Optional, and off unless the manifest names an input file. Everything below
	 * needs a company, because the engine asks _current_company for rail types, for
	 * money and for the owner of a new station - a scenario has none until it is
	 * loaded - so the build borrows one and keeps it in the save.
	 *
	 * The construction settings are pinned above, before GenerateWorld, rather than
	 * here: _settings_game is only refreshed by world generation, so setting them in
	 * this block would be a silent no-op.
	 */
	if (HsbNetworkIsRequested()) {
		g_hsb_net.network_company = HsbBorrowCompany();
		if (g_hsb_net.network_company == CompanyID::Invalid()) return false;
		/* Log the *live* settings, not the newgame ones: these were set before
		 * GenerateWorld, and the gap between the two blocks is exactly the bug this
		 * comment exists to prevent. */
		Debug(Facility::Misc, Severity::Critical, "hsb: live settings: build_on_slopes {} autoslope {} crossings_with_competitor {} "
			"road_stop_on_town_road {}; rail type {}, road type {}",
			_settings_game.construction.build_on_slopes ? "on" : "OFF",
			_settings_game.construction.autoslope ? "on" : "OFF",
			_settings_game.construction.crossing_with_competitor ? "on" : "OFF",
			_settings_game.construction.road_stop_on_town_road ? "on" : "OFF",
			static_cast<uint>(HsbRailType()), static_cast<uint>(HsbRoadType()));

		/* Forests: dense tree blocks around Forest industry anchors. Placed before the
		 * network, so a corridor routed afterwards has to treat a forest as a hill it
		 * can cut through rather than a treeless shortcut. */
		uint forest_blocks = 0;
		for (const auto &entry : ind_json) {
			if (entry.at("type").get<std::string>() != "Forest") continue;
			const TileIndex center = HsbTileFor(entry.at("lat").get<double>(), entry.at("lon").get<double>());
			const int half = 10;
			const TileIndex tl = TileXY(std::max<int>(TileX(center) - half, 1), std::max<int>(TileY(center) - half, 1));
			const TileIndex br = TileXY(std::min<int>(TileX(center) + half, Map::SizeX() - 2),
			                             std::min<int>(TileY(center) + half, Map::SizeY() - 2));
			if (Command<Commands::PlantTree>::Do(DoCommandFlag::Execute, tl, br, TREE_INVALID, false).Succeeded()) ++forest_blocks;
		}
		Debug(Facility::Misc, Severity::Critical, "hsb: forest blocks planted: {}", forest_blocks);
		HsbLogIndustries("after trees");

		/* Roads. Four layers, all routed by HsbRouteRoad - long straight legs joined
		 * by right-angle corners, the geometry the player's autoroad tool produces:
		 * the trunk corridors, a network over every town, a spur to every land
		 * industry, and then whatever failed on the first pass retried now that the
		 * rest of the network exists.
		 *
		 * Then everything that dead-ends is pruned, and the fragments are stitched:
		 * a road that leads nowhere is worse than no road at all, because it looks
		 * like a road a player would follow. HsbAuditNetwork reports what survived. */

		uint road_routed = 0, road_snapped = 0, road_unsnappable = 0;
		std::vector<std::pair<TileIndex, TileIndex>> retry_links;
		if (!g_hsb_net.autobahnen.empty()) {
			std::vector<std::vector<std::pair<double, double>>> corridors;
			if (!HsbReadCorridors(g_hsb_net.autobahnen, "autobahnen", corridors)) return false;
			const uint segments = HsbRouteCorridors(corridors, road_routed, road_snapped, road_unsnappable, retry_links);
			Debug(Facility::Misc, Severity::Critical, "hsb: trunk corridors: {} segments ok", segments);
			HsbLogIndustries("after trunk corridors");
		}
		if (!g_hsb_net.roads.empty()) {
			std::vector<std::vector<std::pair<double, double>>> corridors;
			if (!HsbReadCorridors(g_hsb_net.roads, "roads", corridors)) return false;
			const uint segments = HsbRouteCorridors(corridors, road_routed, road_snapped, road_unsnappable, retry_links);
			Debug(Facility::Misc, Severity::Critical, "hsb: town network: {} segments ok", segments);
			HsbLogIndustries("after town network");
		}
		uint spur_ok = 0, spur_missed = 0;
		HsbBuildIndustrySpurs(spur_ok, spur_missed);
		Debug(Facility::Misc, Severity::Critical, "hsb: industry spurs: {} built, {} missed", spur_ok, spur_missed);
		HsbLogIndustries("after industry spurs");

		const uint pruned = HsbPruneDeadEnds();
		Debug(Facility::Misc, Severity::Critical, "hsb: dead-end road tiles pruned: {}", pruned);
		const uint still_unrouted = HsbRetryRoadLinks(retry_links);
		Debug(Facility::Misc, Severity::Critical, "hsb: road links unrouted after retries: {}", still_unrouted);
		HsbJoinTownsToTrunk();
		HsbJoinRoadFragments();
		Debug(Facility::Misc, Severity::Critical, "hsb: road endpoints snapped out of towns: {}, unsnappable: {}", road_snapped, road_unsnappable);
		HsbLogIndustries("after road network");

		/* Rail: main lines from the data, then a station where a line meets a town,
		 * then a siding with a depot at every bulk industry worth rail. */
		if (g_hsb_net.with_rail && !g_hsb_net.rail.empty()) {
			std::vector<std::vector<std::pair<double, double>>> corridors;
			if (!HsbReadCorridors(g_hsb_net.rail, "rail", corridors)) return false;
			if (!g_hsb_net.stations.empty() && !HsbReadStationAnchors(g_hsb_net.stations)) return false;
			uint rail_routed = 0, rail_failed = 0;
			for (const auto &wps : corridors) {
				TileIndex prev{};
				bool have_prev = false;
				for (const auto &wp : wps) {
					const TileIndex cur = HsbSnapBuildable(HsbTileFor(wp.first, wp.second),
						static_cast<uint>(kHsbEndpointSnap.fallback));
					if (!IsValidTile(cur)) { have_prev = false; continue; }
					if (have_prev && cur != prev) {
						if (HsbRouteRail(prev, cur)) ++rail_routed; else ++rail_failed;
					}
					prev = cur;
					have_prev = true;
				}
			}
			Debug(Facility::Misc, Severity::Critical, "hsb: rail: {} lines routed, {} failed", rail_routed, rail_failed);
			HsbLogIndustries("after rail track");

			uint towns_without_rail = 0;
			const uint rail_stations = HsbBuildStations(towns_without_rail);
			uint depots = 0, stranded = 0;
			const uint sidings = HsbBuildFreightSidings(depots, stranded);
			Debug(Facility::Misc, Severity::Critical, "hsb: rail totals: {} lines, {} stations, {} sidings, {} depots ({} taken down again)",
				rail_routed, rail_stations, sidings, depots, stranded);
			HsbLogIndustries("after rail freight");

			/* Rolling stock, last: it needs depots from this phase and the stations
			 * from the rail phase. */
			if (g_hsb_net.with_fleet) {
				HsbHandOutEngines();
				HsbServiceCounts service{};
				HsbBuildBusService(service, g_hsb_net.bus_towns);
				HsbBuildStationTrains(service, g_hsb_net.trains);
			} else {
				Debug(Facility::Misc, Severity::Critical, "hsb: fleet not requested; the network is built but nothing runs on it");
			}
		}

		/* Airports and docks: the real ones, at their real positions. The list is data
		 * rather than a rule about which cities deserve one - an earlier version picked
		 * "the most isolated big towns" and put runways where no airport has ever
		 * been. The position matters too: an airport is a big flat field and belongs
		 * outside its town. */
		if (!g_hsb_net.airports.empty()) HsbBuildAirports(g_hsb_net.airports);
		if (!g_hsb_net.harbours.empty()) HsbBuildHarbours(g_hsb_net.harbours);

		/* The company stays. It owns the stations, the depots, the bus stops and the
		 * fleet, and orders are refused for anything it does not own - so deleting it
		 * would leave the running services belonging to a company that no longer
		 * exists. With zero companies the player is a spectator with nothing to
		 * build; with every company there is no slot left to start in. The rule is
		 * exactly one, and the manifest is required to leave a second slot free. */
		Company *owner = Company::GetIfValid(g_hsb_net.network_company);
		if (owner != nullptr) {
			/* The borrowed cash is given away with the company: it is a build
			 * scaffold, not a scenario starting balance, and a player who finds a
			 * trillion euro on the network company's money display has found a bug. */
			owner->money = 0;
			owner->current_loan = 0;
			_current_company = _local_company = CompanyID::Invalid();
			Debug(Facility::Misc, Severity::Critical, "hsb: keeping the infrastructure company (id {}) in the save", owner->index.base());
		}
	}

	/* 4. Audit, then save. The audit runs before the save so a gate can assert on
	 * the numbers even when the save itself fails. */
	HsbAuditBuild(static_cast<uint>(towns_placed), placed);
	/* Refusal tallies, before the gates: a gate that fails on "one industry did
	 * not survive" is the question, and the tally is the answer. Printing them
	 * after the gates made every failure look identical. */
	HsbDumpNetwork();
	HsbReportRoadFailures();

	/* The pre-save gates. Each of these is a scenario that looks finished and is
	 * not, and none of them is visible from the exit code: a company that leaves
	 * the player with no slot, a depot full of vehicles that never leave, an
	 * industry that a road bulldozed, a start view on the empty map corner. */
	if (HsbNetworkIsRequested()) {
		const uint companies = Company::GetNumItems();
		Debug(Facility::Misc, Severity::Critical, "hsb: companies in the save: {} (must be 1: the network owner, with a slot left free for the player)", companies);
		if (companies != 1) {
			/* Name them. "found 2" is a number; which one is the intruder, and whether
			 * it is the network's owner or something the engine created behind our
			 * back, is the whole answer. */
			for (const Company *c : Company::Iterate()) {
				uint vehicles = 0;
				for (const Vehicle *v : Vehicle::Iterate()) {
					if (v->owner == c->index) ++vehicles;
				}
				Debug(Facility::Misc, Severity::Critical, "hsb:   company {} '{}' is_ai {} money {} vehicles {}",
					c->index.base(), c->name, c->is_ai ? "yes" : "NO",
					static_cast<int64_t>(c->money), vehicles);
			}
			Debug(Facility::Misc, Severity::Critical, "hsb: ERROR - expected exactly one company (the network owner), found {}", companies);
			return false;
		}
		if (_settings_game.difficulty.max_no_competitors < 2) {
			Debug(Facility::Misc, Severity::Critical, "hsb: ERROR - only {} company slot(s): the player would have nowhere to start",
				_settings_game.difficulty.max_no_competitors);
			return false;
		}
		if (g_hsb_net.with_fleet) {
			/* Count what a player would call vehicles: the primary of each consist.
			 * The rear half of a two-unit diesel has no orders of its own - it shares
			 * the consist's - and counting it as stranded would say a dozen vehicles
			 * were broken when none were. */
			uint primaries = 0, routed = 0, parked = 0;
			for (const Vehicle *v : Vehicle::Iterate()) {
				if (!v->IsPrimaryVehicle()) continue;
				++primaries;
				if (v->orders != nullptr && v->GetNumOrders() > 0) ++routed;
				else continue;
				if (v->IsStoppedInDepot()) ++parked;
			}
			Debug(Facility::Misc, Severity::Critical, "hsb: fleet: {} consist(s), {} with a route, {} of those still parked in a depot", primaries, routed, parked);
			if (routed == 0) {
				Debug(Facility::Misc, Severity::Critical, "hsb: ERROR - the scenario ships no running service");
				return false;
			}
			/* Having orders is not the same as running. A consist that is still in its
			 * depot at save time has a perfect route and will never take it, because
			 * nothing pressed the start button. That is a silent failure: every earlier
			 * check passed, and the map is a museum. */
			if (parked > 0) {
				Debug(Facility::Misc, Severity::Critical, "hsb: ERROR - {} routed consist(s) never left the depot; the scenario would load as a museum", parked);
				return false;
			}
		}
		/* The placement log says N/N and the save used to hold fewer, with nothing in
		 * between complaining: a depot built on an industry tile converted it, and a
		 * rail line levelled through one. Both now refuse rather than demolish, so the
		 * design total has to survive to the save or the build says so. */
		const uint alive = HsbCountIndustries();
		Debug(Facility::Misc, Severity::Critical, "hsb: pre-save industry count: {} of {} designed", alive, ind_json.size());
		if (alive < ind_json.size()) {
			Debug(Facility::Misc, Severity::Critical, "hsb: ERROR - {} of {} designed industries did not survive the build",
				ind_json.size() - alive, ind_json.size());
			return false;
		}
		/* A start view that is missing or off the map is the bug a first playthrough
		 * shows: the game opens and the first thing on screen is empty grass. It is
		 * cheap to catch here and impossible to notice from a build log, so the build
		 * fails instead of shipping it. */
		HsbChooseStartView();
		if (_hsb_start_view_tile == INVALID_TILE) {
			Debug(Facility::Misc, Severity::Critical, "hsb: ERROR - no start view tile was chosen, the player would open on the empty map corner");
			return false;
		}
		Debug(Facility::Misc, Severity::Critical, "hsb: start view tile is valid: ({},{})", TileX(_hsb_start_view_tile), TileY(_hsb_start_view_tile));
	}

	if (SaveOrLoad(save, SaveLoadOperation::Save, DetailedFileType::GameFile, Subdirectory::Scenario, false) != SaveLoadResult::Ok) {
		Debug(Facility::Misc, Severity::Critical, "hsb: save failed");
		return false;
	}
	Debug(Facility::Misc, Severity::Critical, "hsb: saved {}", save);
	return true;
}
