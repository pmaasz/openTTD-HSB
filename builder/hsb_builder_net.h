#ifndef HSB_BUILDER_NET_H
#define HSB_BUILDER_NET_H

/**
 * Pure geometry, tunables and lookup tables for the HSB network phase.
 *
 * Everything in here is deliberately free of engine types (no TileIndex, no
 * CommandCost), so it can be unit tested without a generated map. The engine
 * code in hsb_builder.cpp is the thin layer that asks the engine what a tile
 * is and then uses these helpers to decide what to do about it.
 *
 * The rules these helpers encode came out of measuring the sibling
 * germanyTransportTycoonDeluxe builder against a 4096 tile map; the reasoning
 * lives with the call sites there.
 *
 * This file is part of openTTD-HSB.
 * Licensed under the GNU General Public License, version 2.
 * See LICENSE.md for details.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
/** How many tiles the fallback planner may look at before giving up. */
inline constexpr uint32_t HsbSearchNodeBudget = 250000;

/** Chebyshev distance: the ring a flood or a corner search walks outwards. */
inline int HsbChebyshev(int dx, int dy)
{
	return std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
}

/** Manhattan distance, used for "is this detour still on the way". */
inline int HsbManhattan(int x0, int y0, int x1, int y1)
{
	return std::abs(x1 - x0) + std::abs(y1 - y0);
}

/**
 * Parse one integer out of an environment variable.
 *
 * `raw` is the getenv() result or nullptr. An unset, empty or unparseable value
 * yields `fallback`, and the caller logs it: a tuning knob that silently
 * became the default is how a sweep ends up comparing two identical builds.
 * Out-of-range values are clamped, because a run of 0 turns every chunk into a
 * corner and a snap of 0 snaps nowhere, and neither failure names the number
 * that caused it.
 *
 * @return the value to use; `*clamped` is set when the range had to be applied.
 */
inline int HsbTuneInt(const char *raw, int fallback, int lo, int hi, bool *clamped = nullptr)
{
	if (clamped != nullptr) *clamped = false;
	if (raw == nullptr || *raw == '\0') return fallback;
	char *end = nullptr;
	const long value = std::strtol(raw, &end, 10);
	if (end == raw) return fallback;
	if (value < lo || value > hi) {
		if (clamped != nullptr) *clamped = true;
		return static_cast<int>(std::clamp<long>(value, lo, hi));
	}
	return static_cast<int>(value);
}

/**
 * The corner-fan offsets a router should try, in the order it should try them.
 *
 * The default ladder for a maximum of `def_max` is 0, -2, +2, -4, +4, ... : the
 * even ladder, in that order, because order is part of the result. Candidates
 * are costed and stable-sorted, so a tie goes to the one that was pushed first
 * and the order written here is what decides those ties.
 *
 * A `raw` value containing a comma is an explicit list ("0,-3,3"), for when the
 * even ladder is the wrong shape. A field that is not a number at all is
 * dropped rather than fatal - this is a tuning knob for a parameter sweep, not
 * a manifest a build depends on - and an empty result falls back to {0}, since
 * a fan of nothing cannot route anything.
 *
 * Offsets are clamped to +/-64. Every offset is four candidate legs and each
 * candidate is a build attempt the whole build pays on, per chunk, per link: a
 * fan of a thousand is not a wider search, it is a build that never finishes.
 */
inline std::vector<int> HsbCornerFanOffsets(const char *raw, int def_max)
{
	std::vector<int> offsets;
	if (raw != nullptr && std::string(raw).find(',') != std::string::npos) {
		const std::string list(raw);
		size_t pos = 0;
		while (pos <= list.size() && offsets.size() < 33) {
			const size_t comma = list.find(',', pos);
			const std::string field = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
			char *end = nullptr;
			const long value = std::strtol(field.c_str(), &end, 10);
			if (!field.empty() && end != field.c_str()) {
				offsets.push_back(static_cast<int>(std::clamp<long>(value, -64, 64)));
			}
			if (comma == std::string::npos) break;
			pos = comma + 1;
		}
	} else {
		bool clamped = false;
		const int max = HsbTuneInt(raw, def_max, 0, 64, &clamped);
		offsets.push_back(0);
		for (int step = 1; 2 * step <= max; ++step) {
			offsets.push_back(-2 * step);
			offsets.push_back(2 * step);
		}
		/* An odd maximum has an odd rung on it. Put it last so the ladder above
		 * keeps its order. */
		if (max % 2 != 0) {
			offsets.push_back(-max);
			offsets.push_back(max);
		}
	}
	if (offsets.empty()) offsets.push_back(0);
	return offsets;
}

/**
 * How far along one axis a corridor chunk runs.
 *
 * Corner spacing is the whole point of the router: one L for a whole corridor
 * doubles the length of a diagonal run and with it the amount of river, cliff
 * and town it has to cross, and on a map this size most of those fail. So the
 * corridor is cut into pieces of at most `run` tiles per axis and each piece
 * gets its own corner - drag, drag again - which is what a person does with the
 * same autoroad button.
 *
 * Progress is monotone and never overshoots the target, so the walk terminates
 * for any target.
 */
inline int HsbRunEnd(int from, int to, int run)
{
	return from + std::clamp(to - from, -run, run);
}

/**
 * The cost of running a leg over one tile's worth of ground.
 *
 * Water and slopes are structure, not mistakes: they want a bridge and a tunnel
 * respectively, and both are command calls with their own failure modes, so
 * they are weighted above ordinary ground. An industry is weighted below water
 * because it is never built on at all (a road there converts the factory and the
 * industry closes, silently, with a success return) - a leg that has to go
 * round one is worth more than a leg that has to bridge a stream.
 */
inline uint32_t HsbObstacleCost(bool water, bool sloped, bool industry)
{
	if (water) return 16;
	if (sloped) return 4;
	if (industry) return 8;
	return 0;
}

/** Which axis a straight leg runs along, for a piece that has already been costed. */
enum class HsbLegAxis {
	x,
	y,
};

inline HsbLegAxis HsbOtherAxis(HsbLegAxis axis)
{
	return axis == HsbLegAxis::x ? HsbLegAxis::y : HsbLegAxis::x;
}

/**
 * A leg endpoint nudged onto buildable ground, searched along one axis only.
 *
 * A straight leg needs both its ends on one row or one column, so a snap that
 * moves the target sideways leaves the piece unroutable - and then every link
 * whose target was nudged fails, which is most of them. The axis the second leg
 * runs along is kept and only the other coordinate is searched.
 *
 * `free_axis` is the axis the *second* leg runs along, and therefore the one the
 * endpoint may not move along. Returns the offset applied (0 when the endpoint
 * was already usable or nothing within `radius` was, in which case the caller's
 * own coordinates are written back), so the caller can report an unsnappable
 * endpoint instead of routing into a house.
 */
template <typename Tpredicate>
inline int HsbSnapAlong(int x, int y, HsbLegAxis free_axis, int radius, Tpredicate buildable, int *out_x, int *out_y)
{
	const bool free_is_y = free_axis == HsbLegAxis::y;
	for (int r = 0; r <= radius; ++r) {
		const int n = (r == 0) ? 1 : 2;
		for (int i = 0; i < n; ++i) {
			const int d = (r == 0) ? 0 : (i == 0 ? r : -r);
			const int nx = free_is_y ? x : x + d;
			const int ny = free_is_y ? y + d : y;
			if (buildable(nx, ny)) {
				if (out_x != nullptr) *out_x = nx;
				if (out_y != nullptr) *out_y = ny;
				return d;
			}
		}
	}
	if (out_x != nullptr) *out_x = x;
	if (out_y != nullptr) *out_y = y;
	return 0;
}

/** One corner candidate: where to turn, where to end up, and in what order. */
struct HsbCorner {
	/** The corner tile, which stays exactly where the geometry puts it. */
	int via_x = 0;
	int via_y = 0;
	/** The chunk endpoint, nudged onto buildable ground along `second`. */
	int end_x = 0;
	int end_y = 0;
	HsbLegAxis first = HsbLegAxis::x;
	HsbLegAxis second = HsbLegAxis::y;
	/** True when the candidate is the geometry's own corner rather than a nudged one. */
	bool geometric = true;
};

/**
 * Every corner a corridor chunk between `here` and `there` could use.
 *
 * For each offset in the fan: the two corners the geometry puts there (the
 * horizontal-first and the vertical-first dogleg), then the same two orders with
 * the corner moved perpendicular to the first leg. Two candidates is not enough
 * on a map of factories, rivers and towns - a link that just misses a steel mill
 * has nowhere else to go, gives up, and leaves the trunk in pieces - but the set
 * has to be a fixed fan rather than a search so the cost per chunk stays
 * predictable.
 *
 * Endpoints are snapped along the axis the *second* leg runs along, so a piece
 * whose ends are on different rows or columns stays routable; a snap that moves
 * an endpoint sideways would leave the whole chunk unroutable.
 *
 * The returned order is the tie-break order for equal costs, which is why the
 * two geometric candidates come first per offset.
 *
 * @param buildable the caller's tile predicate, used only for the endpoint snap
 */
template <typename Tpredicate>
inline std::vector<HsbCorner> HsbCornerCandidates(int here_x, int here_y, int there_x, int there_y,
	const std::vector<int> &fan, uint32_t snap, Tpredicate buildable)
{
	std::vector<HsbCorner> out;
	const auto snap_end = [&](int cx, int cy, HsbLegAxis second, HsbLegAxis first, bool geometric) {
		HsbCorner corner;
		corner.via_x = cx;
		corner.via_y = cy;
		corner.first = first;
		corner.second = second;
		corner.geometric = geometric;
		HsbSnapAlong(cx, cy, second, static_cast<int>(snap), buildable, &corner.end_x, &corner.end_y);
		return corner;
	};

	/* The two corners the geometry itself puts there, once. The fan's zero offset
	 * is deliberately not used to produce them: it would place each corner exactly
	 * where it already is and charge a second dry-run build attempt for the
	 * privilege. */
	out.push_back(snap_end(there_x, here_y, HsbLegAxis::y, HsbLegAxis::x, true));
	out.push_back(snap_end(here_x, there_y, HsbLegAxis::x, HsbLegAxis::y, true));
	/* Then the same two orders with the corner moved off its row and column. A zero
	 * offset is skipped, because a "nudged" corner that did not move is not nudged. */
	for (const int off : fan) {
		if (off == 0) continue;
		out.push_back(snap_end(there_x, here_y + off, HsbLegAxis::y, HsbLegAxis::x, false));
		out.push_back(snap_end(here_x + off, there_y, HsbLegAxis::x, HsbLegAxis::y, false));
	}
	return out;
}

/**
 * Bulk industries a freight siding is worth building to.
 *
 * A coal mine or a steel mill wants rail; a department store does not, and a
 * siding to the wrong industry is track the player has to dig up again. Names
 * are matched against the runtime industry display names, so the set is spelled
 * the way the game spells them (with the duplicates that NewGRF sets use).
 */
inline bool HsbIndustryWantsRail(const std::string &name)
{
	static const char *kWantsRail[] = {
		"Coal Mine", "Iron Ore Mine", "Integrated Steel Mill", "Oil Refinery",
		"Power Station", "Sawmill", "Forest", "Farm", "Farming", "Grain Farm",
		"Coal Liquefaction Plant", "Steamcracker", "Petrol Station", "Salt Mine",
	};
	for (const char *want : kWantsRail) {
		if (name == want) return true;
	}
	return false;
}

/** Industries that need a wider search because they only fit against water. */
inline bool HsbIndustryNeedsWater(const std::string &name)
{
	return name == "Port" || name == "Oil Rig" || name == "Fishing Grounds";
}

/**
 * How interesting a character of the network dump is, so one dump cell shows
 * the most notable thing in its 4x4 block.
 *
 * Order matters and it was got wrong once: "is this inside a town?" outranks
 * everything else, and a town claim covers roughly 20x20 tiles, so most of a
 * map came out as town and every street and corridor inside one was invisible.
 * A viewer that hides the thing you are looking for is worse than one that shows
 * it too eagerly, so a town street outranks the town it sits in.
 */
inline int HsbDumpRank(char c)
{
	switch (c) {
		case ' ': return 0;
		case '.': return 1;
		case '~': return 2;
		case 'I': return 3;
		case ',': return 4;
		case '^': return 5;
		case 'D': return 6;
		case 'r': return 7;
		case 'R': return 8;
		case '#': return 9;
		case 'T': return 10;
		case 't': return 11;
		case 'S': return 12;
		default: return 1;
	}
}

/**
 * The character grid the network dump should print.
 *
 * `classify` is the caller's tile predicate and is only asked about the first
 * tile in the block that outranks what has been found so far, so a block with a
 * station in it does not pay for the other fifteen.
 *
 * One character per `step` x `step` tiles over the whole map: everything else a
 * build reports is a number, and a number cannot show that a corridor runs
 * through the middle of nowhere 200 tiles south of a town.
 */
template <typename Tclassify>
inline std::vector<std::string> HsbDumpGrid(uint32_t size_x, uint32_t size_y, uint32_t step, Tclassify classify)
{
	std::vector<std::string> rows;
	const uint32_t y_blocks = (size_y + step - 1) / step;
	rows.reserve(y_blocks);
	for (uint32_t by = 0; by < size_y; by += step) {
		std::string row;
		row.reserve((size_x + step - 1) / step);
		for (uint32_t bx = 0; bx < size_x; bx += step) {
			char best = ' ';
			int best_rank = -1;
			for (uint32_t dy = 0; dy < step && best_rank < 12; ++dy) {
				for (uint32_t dx = 0; dx < step && best_rank < 12; ++dx) {
					const char c = classify(static_cast<int>(bx + dx), static_cast<int>(by + dy));
					const int rank = HsbDumpRank(c);
					if (rank > best_rank) {
						best_rank = rank;
						best = c;
					}
				}
			}
			row.push_back(best);
		}
		rows.push_back(row);
	}
	return rows;
}

/**
 * The search radius a connectivity check may cover between two road tiles.
 *
 * One step either side of the straight line is where a single corner puts the
 * route, so the straight-line distance plus a margin is enough - and searching
 * further than the geometry could possibly have reached is the expensive way to
 * find out that it did not.
 */
inline uint32_t HsbConnectRadius(int manhattan, uint32_t cap)
{
	uint32_t radius = manhattan > 0 ? static_cast<uint32_t>(manhattan) + 8u : 12u;
	if (radius < 12u) radius = 12u;
	if (cap != 0 && radius > cap) radius = cap;
	return radius;
}

/**
 * A detour budget for the greedy tile-by-tile walker.
 *
 * The only place a corridor genuinely wants to go round something is a mountain
 * range, so the budget is generous; a tight one means giving up in front of the
 * Alps.
 */
inline bool HsbDetourAllowed(int from_manhattan, int to_manhattan, int budget)
{
	return to_manhattan <= from_manhattan + budget;
}

/* ---------------------------------------------------------------------------
 * Manifest ranges for the network phase.
 *
 * One table, used by the builder to validate and by the unit tests to assert the
 * same numbers. A limit that exists in the documentation but not here is a limit
 * nobody checks, and a limit that exists here but not in the documentation is a
 * build error a user cannot understand.
 * ------------------------------------------------------------------------- */

struct HsbRange {
	int64_t lo = 0;
	int64_t hi = 0;
	int64_t fallback = 0;
};

/** Cash handed to the borrowed infrastructure company, in currency units. */
inline constexpr HsbRange kHsbNetworkMoney{0, 4'000'000'000, 50'000'000};
/** Towns that get a bus depot, two stops and one bus. */
inline constexpr HsbRange kHsbBusTowns{0, 1024, 24};
/** Stations that get a spur depot and a train. */
inline constexpr HsbRange kHsbTrains{0, 1024, 12};
/** Radius, in tiles, of the spiral search for an airport or a dock site. */
inline constexpr HsbRange kHsbSiteSearchRadius{1, 256, 40};
/** Radius, in tiles, of the spiral search for a corridor endpoint. */
inline constexpr HsbRange kHsbEndpointSnap{0, 256, 60};
/** How far the network audit looks for a road a town can actually be driven to. */
inline constexpr HsbRange kHsbTownRoadRadius{1, 64, 8};
/** How many towns the network audit reports as off the network, individually. */
inline constexpr HsbRange kHsbAuditReportLimit{0, 4096, 16};

/** Whether a value is inside a range, with a half-open-error message for logs. */
inline bool HsbInRange(const HsbRange &range, int64_t value)
{
	return value >= range.lo && value <= range.hi;
}

#endif
