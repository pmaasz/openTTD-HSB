#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../hsb_builder.h"
#include "../hsb_builder_math.h"
#include "../hsb_builder_net.h"
#include "../3rdparty/nlohmann/json.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

void SetManifestEnvironment(const char *value);

class TestDirectory {
public:
	TestDirectory()
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path() /
			("hsb-builder-test-" + std::to_string(stamp));
		std::filesystem::create_directories(path);
	}

	~TestDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path path;
};

class EnvironmentGuard {
public:
	~EnvironmentGuard()
	{
		SetManifestEnvironment(nullptr);
	}
};

void SetManifestEnvironment(const char *value)
{
#ifdef _WIN32
	_putenv_s("HSB_BUILD", value == nullptr ? "" : value);
#else
	if (value == nullptr) {
		unsetenv("HSB_BUILD");
	} else {
		setenv("HSB_BUILD", value, 1);
	}
#endif
}

nlohmann::json ValidManifest(const std::filesystem::path &directory)
{
	return {
		{"heightmap", (directory / "map.png").string()},
		{"towns", (directory / "towns.json").string()},
		{"industries", (directory / "industries.json").string()},
		{"bounds", {
			{"lat_min", 10.0},
			{"lat_max", 20.0},
			{"lon_min", 30.0},
			{"lon_max", 40.0},
		}},
		{"start_year", 2025},
		{"save", "output.scn"},
	};
}

void WriteManifest(const std::filesystem::path &directory, const nlohmann::json &manifest)
{
	std::ofstream output(directory / "manifest.json");
	output << manifest.dump();
}

void WriteFile(const std::filesystem::path &path, const std::string &contents)
{
	std::ofstream output(path);
	output << contents;
}

bool RunManifest(const std::filesystem::path &directory, const nlohmann::json &manifest)
{
	WriteManifest(directory, manifest);
	const auto manifest_path = (directory / "manifest.json").string();
	SetManifestEnvironment(manifest_path.c_str());
	return HsbBuildScenario();
}

}

TEST_CASE("HSB geometry uses the last-pixel convention")
{
	const HsbBounds bounds{10.0, 20.0, 30.0, 40.0};

	const HsbTile center = HsbTileForBounds(bounds, 15.0, 35.0, 100, 100, false);
	CHECK(center.x == 50);
	CHECK(center.y == 50);

	const HsbTile clockwise = HsbTileForBounds(bounds, 17.0, 36.0, 100, 100, false);
	CHECK(clockwise.x == 30);
	CHECK(clockwise.y == 60);

	const HsbTile counter_clockwise = HsbTileForBounds(bounds, 17.0, 36.0, 100, 100, true);
	CHECK(counter_clockwise.x == 40);
	CHECK(counter_clockwise.y == 30);

	const HsbTile north_west = HsbTileForBounds(bounds, 20.0, 30.0, 100, 100, false);
	CHECK(north_west.x == 0);
	CHECK(north_west.y == 0);
}

TEST_CASE("HSB geometry and filename guards reject invalid values")
{
	const HsbBounds bounds{10.0, 20.0, 30.0, 40.0};
	CHECK(HsbCoordinateInBounds(bounds, 15.0, 35.0));
	CHECK_FALSE(HsbCoordinateInBounds(bounds, 21.0, 35.0));
	CHECK_FALSE(HsbCoordinateInBounds(bounds, 15.0, std::numeric_limits<double>::quiet_NaN()));
	CHECK(HsbIsPowerOfTwo(64, 64, 4096));
	CHECK(HsbIsPowerOfTwo(4096, 64, 4096));
	CHECK_FALSE(HsbIsPowerOfTwo(8192, 64, 4096));
	CHECK_FALSE(HsbIsPowerOfTwo(65, 64, 4096));
	CHECK(HsbSafeFilename("output.scn"));
	CHECK_FALSE(HsbSafeFilename("../output.scn"));
	CHECK_FALSE(HsbSafeFilename("sub/output.scn"));
	CHECK_FALSE(HsbSafeFilename("."));
	CHECK_FALSE(HsbSafeFilename(std::string("output\0.scn", 11)));
}

TEST_CASE("HSB staircase walk keeps every carved tile edge-connected")
{
	/* A river rasterised as a straight line between two waypoints produces tiles
	 * that only touch at their corners wherever it runs diagonally: a chain of
	 * ponds a ship cannot enter. The walk must visit every tile of the shortest
	 * path and consecutive visits must share an edge. */
	auto collect = [](int x0, int y0, int x1, int y1) {
		std::vector<std::pair<int, int>> path;
		HsbStaircaseWalk(x0, y0, x1, y1, [&](int x, int y, bool) { path.emplace_back(x, y); });
		return path;
	};

	SECTION("horizontal runs") {
		CHECK(collect(0, 0, 3, 0) == std::vector<std::pair<int, int>>{{0, 0}, {1, 0}, {2, 0}, {3, 0}});
		CHECK(collect(3, 0, 0, 0) == std::vector<std::pair<int, int>>{{3, 0}, {2, 0}, {1, 0}, {0, 0}});
	}
	SECTION("vertical runs") {
		CHECK(collect(0, 0, 0, 2) == std::vector<std::pair<int, int>>{{0, 0}, {0, 1}, {0, 2}});
	}
	SECTION("a diagonal is a staircase, not a cross") {
		const std::vector<std::pair<int, int>> path = collect(0, 0, 3, 3);
		REQUIRE(path.size() == 7);
		for (size_t i = 1; i < path.size(); ++i) {
			const int dx = std::abs(path[i].first - path[i - 1].first);
			const int dy = std::abs(path[i].second - path[i - 1].second);
			CHECK(dx + dy == 1);
		}
		CHECK(path.back() == std::make_pair(3, 3));
	}
	SECTION("a zero-length move still visits its single tile") {
		CHECK(collect(5, 5, 5, 5) == std::vector<std::pair<int, int>>{{5, 5}});
	}
	SECTION("the path is monotone and covers the bounding box") {
		const std::vector<std::pair<int, int>> path = collect(0, 0, 2, 5);
		REQUIRE(path.size() == 8);
		for (size_t i = 1; i < path.size(); ++i) {
			CHECK(path[i].first >= path[i - 1].first);
			CHECK(path[i].second >= path[i - 1].second);
		}
	}
}

TEST_CASE("HSB rejects malformed manifests before map generation")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json manifest = ValidManifest(directory.path);

	CHECK_FALSE(RunManifest(directory.path, {{}}));
	CHECK_FALSE(RunManifest(directory.path, {{"bounds", manifest.at("bounds")}}));
	manifest.at("bounds")["lat_max"] = 5.0;
	CHECK_FALSE(RunManifest(directory.path, manifest));
	manifest = ValidManifest(directory.path);
	manifest["save"] = "../escape.scn";
	CHECK_FALSE(RunManifest(directory.path, manifest));
	manifest = ValidManifest(directory.path);
	manifest["seed"] = 0xFFFFFFFF;
	CHECK_FALSE(RunManifest(directory.path, manifest));
	manifest = ValidManifest(directory.path);
	manifest["heightmap"] = "map.png";
	CHECK_FALSE(RunManifest(directory.path, manifest));
}

TEST_CASE("HSB rejects out-of-range scenario settings before map generation")
{
	TestDirectory directory;
	EnvironmentGuard environment;

	/* Company slots, loan, snow line, and currency are pinned so they are not
	 * inherited from the builder's own openttd.cfg - but an out-of-range or
	 * unknown value must be a build error, not a silent fallback. */
	nlohmann::json manifest = ValidManifest(directory.path);
	manifest["currency"] = "XYZ";
	CHECK_FALSE(RunManifest(directory.path, manifest));
	manifest = ValidManifest(directory.path);
	manifest["competitors"] = 99;
	CHECK_FALSE(RunManifest(directory.path, manifest));
	manifest = ValidManifest(directory.path);
	manifest["max_loan"] = -1;
	CHECK_FALSE(RunManifest(directory.path, manifest));
	manifest = ValidManifest(directory.path);
	manifest["snow_line_height"] = 9999;
	CHECK_FALSE(RunManifest(directory.path, manifest));
	manifest = ValidManifest(directory.path);
	manifest["industry_search_radius"] = 0;
	CHECK_FALSE(RunManifest(directory.path, manifest));
}

TEST_CASE("HSB rejects invalid town data before map generation")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json manifest = ValidManifest(directory.path);
	std::ofstream towns(directory.path / "towns.json");
	towns << R"([])";
	towns.close();

	CHECK_FALSE(RunManifest(directory.path, manifest));
}

/* ---------------------------------------------------------------------------
 * The network phase.
 *
 * These cover the two halves of it separately, because they fail in different
 * ways. The pure half (this header's geometry, the tunables, the lookup tables)
 * is where a wrong answer produces a plausible-looking map nobody can debug from
 * a log, so it is pinned directly. The manifest half is where a typo produces an
 * hour of routing and then a refusal to save, so every documented rule has a
 * case that must be rejected before a byte of map is generated.
 * ------------------------------------------------------------------------- */

TEST_CASE("HSB tunables are read, clamped and reported rather than guessed")
{
	bool clamped = false;

	SECTION("an unset or empty variable is the default, and is not clamped") {
		CHECK(HsbTuneInt(nullptr, 64, 1, 512, &clamped) == 64);
		CHECK_FALSE(clamped);
		CHECK(HsbTuneInt("", 64, 1, 512, &clamped) == 64);
		CHECK_FALSE(clamped);
	}

	SECTION("a number in range is taken verbatim") {
		CHECK(HsbTuneInt("17", 64, 1, 512, &clamped) == 17);
		CHECK_FALSE(clamped);
		CHECK(HsbTuneInt("-3", 64, 1, 512, &clamped) == 1);
		CHECK(clamped);
	}

	SECTION("a number out of range is clamped, and says so") {
		CHECK(HsbTuneInt("0", 64, 1, 512, &clamped) == 1);
		CHECK(clamped);
		CHECK(HsbTuneInt("99999", 64, 1, 512, &clamped) == 512);
		CHECK(clamped);
	}

	SECTION("something that is not a number falls back instead of becoming 0") {
		/* A run of 0 turns every chunk into a corner and a snap of 0 snaps nowhere,
		 * so a parse failure that silently yielded 0 would wedge the router with no
		 * indication which number did it. */
		CHECK(HsbTuneInt("abc", 64, 1, 512, &clamped) == 64);
		CHECK_FALSE(clamped);
		CHECK(HsbTuneInt("12abc", 64, 1, 512, &clamped) == 12);
	}
}

TEST_CASE("HSB corner fan is the even ladder, in the order the ties depend on")
{
	SECTION("the default ladder is 0, -2, +2, -4, +4 ...") {
		CHECK(HsbCornerFanOffsets(nullptr, 8) == std::vector<int>{0, -2, 2, -4, 4, -6, 6, -8, 8});
	}
	SECTION("a maximum of 0 still offers the origin corner") {
		CHECK(HsbCornerFanOffsets(nullptr, 0) == std::vector<int>{0});
	}
	SECTION("an odd maximum puts its odd rung last, so the ladder above keeps its order") {
		CHECK(HsbCornerFanOffsets(nullptr, 5) == std::vector<int>{0, -2, 2, -4, 4, -5, 5});
	}
	SECTION("the maximum itself is range-checked like any other tunable") {
		CHECK(HsbCornerFanOffsets("99999", 8).size() == HsbCornerFanOffsets("64", 8).size());
	}
	SECTION("an explicit list is taken verbatim, including out-of-order offsets") {
		/* Order is part of the result: candidates are costed and stable-sorted, so a
		 * tie goes to the one pushed first. */
		CHECK(HsbCornerFanOffsets("0,-3,3", 8) == std::vector<int>{0, -3, 3});
		CHECK(HsbCornerFanOffsets("7,0,-7", 8) == std::vector<int>{7, 0, -7});
	}
	SECTION("a field that is not a number is dropped, not fatal - this is a sweep knob") {
		CHECK(HsbCornerFanOffsets("0,zz,4", 8) == std::vector<int>{0, 4});
	}
	SECTION("an empty or all-garbage list still offers one corner, or nothing routes") {
		CHECK(HsbCornerFanOffsets(",,", 8) == std::vector<int>{0});
		CHECK(HsbCornerFanOffsets("abc", 8) == std::vector<int>{0, -2, 2, -4, 4, -6, 6, -8, 8});
	}
	SECTION("explicit offsets are clamped, because each one is four build attempts") {
		CHECK(HsbCornerFanOffsets("0,1000", 8) == std::vector<int>{0, 64});
	}
}

TEST_CASE("HSB chunks a corridor into straight runs that always make progress")
{
	SECTION("a long diagonal advances by the run length on each axis, not the whole way") {
		/* One L for a whole corridor doubles its length and with it the amount of
		 * river, cliff and town it has to cross. */
		CHECK(HsbRunEnd(0, 400, 64) == 64);
		CHECK(HsbRunEnd(0, 400, 64) != 400);
		CHECK(HsbRunEnd(400, 0, 64) == 336);
		CHECK(HsbRunEnd(10, 400, 1) == 11);
	}
	SECTION("a target within one run is reached in one chunk") {
		CHECK(HsbRunEnd(0, 40, 64) == 40);
		CHECK(HsbRunEnd(400, 380, 64) == 380);
	}
	SECTION("a run of 0 would never advance, so it is clamped by the caller") {
		/* Documented rather than asserted here: HsbTuneInt floors HSB_TUNE_ROAD_RUN at
		 * 1, so a zero run cannot reach this helper from the environment. */
		CHECK(HsbTuneInt("0", 64, 1, 512) == 1);
	}
	SECTION("a walk from anywhere to anywhere terminates in a bounded number of chunks") {
		for (int from : {0, 37, 400}) {
			for (int to : {0, 199, 400}) {
				int x = from;
				uint chunks = 0;
				while (x != to && chunks < 1000) {
					const int next = HsbRunEnd(x, to, 64);
					CHECK(next != x);
					x = next;
					++chunks;
				}
				CHECK(x == to);
				CHECK(chunks <= 7);
			}
		}
	}
}

TEST_CASE("HSB obstacle costs order structure above ground, and a factory above a hill")
{
	/* Water and slopes are structures - a bridge, a tunnel - each a command call
	 * with its own failure modes. An industry is never built on at all, but a leg
	 * that has to go round one is worth more than a leg that bridges a stream, so
	 * the router prefers the bridge. */
	CHECK(HsbObstacleCost(false, false, false) == 0);
	CHECK(HsbObstacleCost(false, true, false) == 4);
	CHECK(HsbObstacleCost(false, false, true) == 8);
	CHECK(HsbObstacleCost(true, false, false) == 16);
	CHECK(HsbObstacleCost(true, false, true) == 16);
	CHECK(HsbObstacleCost(true, true, true) == 16);
}

TEST_CASE("HSB corner candidates cover both orders, both nudges, and stay routable")
{
	/* Two candidates is not enough on a map of factories, rivers and towns: a link
	 * that just misses a steel mill has nowhere else to go, gives up, and leaves
	 * the trunk in pieces. */
	const std::vector<int> fan = HsbCornerFanOffsets(nullptr, 2);   // 0, -2, 2
	const auto no_obstruction = [](int, int) { return true; };
	const auto corners = HsbCornerCandidates(10, 20, 74, 92, fan, 8, no_obstruction);
	/* Two geometric corners, then one nudged pair per non-zero offset. The zero
	 * offset contributes only the two geometric ones: nudging a corner by zero
	 * places it exactly where it already is, and would be charged a second dry-run
	 * build attempt for the privilege. */
	REQUIRE(corners.size() == 2 + (fan.size() - 1) * 2);

	SECTION("the first two candidates are the geometry's own corners, horizontal-first") {
		CHECK(corners[0].via_x == 74);
		CHECK(corners[0].via_y == 20);
		CHECK(corners[0].first == HsbLegAxis::x);
		CHECK(corners[0].second == HsbLegAxis::y);
		CHECK(corners[0].geometric);
		CHECK(corners[1].via_x == 10);
		CHECK(corners[1].via_y == 92);
		CHECK(corners[1].first == HsbLegAxis::y);
		CHECK(corners[1].second == HsbLegAxis::x);
		CHECK(corners[1].geometric);
	}
	SECTION("every candidate's two legs are on one row or one column") {
		/* A leg needs both ends on one row or one column; a candidate that is not
		 * is unroutable no matter what the tile predicate says. */
		for (const auto &c : corners) {
			if (c.first == HsbLegAxis::x) CHECK(c.via_y == c.end_y);
			else CHECK(c.via_x == c.end_x);
			if (c.second == HsbLegAxis::x) CHECK(c.via_y == c.end_y);
			else CHECK(c.via_x == c.end_x);
		}
	}
	SECTION("a nudged corner is off the geometry's row or column, and never marked geometric") {
		size_t nudged = 0;
		for (const auto &c : corners) {
			if (c.geometric) continue;
			++nudged;
			const bool off_column = c.via_x != 74 && c.via_x != 10;
			const bool off_row = c.via_y != 20 && c.via_y != 92;
			CHECK((off_column || off_row));
		}
		CHECK(nudged == (fan.size() - 1) * 2);
	}
	SECTION("an endpoint on the far side snaps back towards the target, not away") {
		/* buildable() refuses one specific tile, so the snap must step over it. */
		int blocked_x = 74;
		int probed = 0;
		const auto one_hole = [&](int x, int y) {
			++probed;
			return !(x == blocked_x && y == 92);
		};
		const auto snapped = HsbCornerCandidates(10, 20, 74, 92, std::vector<int>{0}, 8, one_hole);
		REQUIRE(snapped.size() == 2);
		CHECK(blocked_x == 74);
		CHECK(snapped[0].end_y != 92);
		/* The free axis for a horizontal-first dogleg is Y, so X is untouched. */
		CHECK(snapped[0].end_x == 74);
	}
	SECTION("a target nothing can reach keeps the caller's coordinates rather than drifting") {
		const auto nowhere = [](int, int) { return false; };
		int out_x = 0, out_y = 0;
		CHECK(HsbSnapAlong(5, 7, HsbLegAxis::y, 4, nowhere, &out_x, &out_y) == 0);
		CHECK(out_x == 5);
		CHECK(out_y == 7);
	}
	SECTION("the snap searches the free axis only") {
		CHECK(HsbSnapAlong(5, 7, HsbLegAxis::y, 0, [](int, int) { return true; }, nullptr, nullptr) == 0);
		const auto x_only = [](int x, int) { return x == 9; };
		CHECK(HsbSnapAlong(5, 7, HsbLegAxis::x, 4, x_only, nullptr, nullptr) == 4);
		CHECK(HsbSnapAlong(5, 7, HsbLegAxis::y, 4, x_only, nullptr, nullptr) == 0);
	}
}

TEST_CASE("HSB connectivity radius covers a corner and never searches past the geometry")
{
	/* One step either side of the straight line is where a single corner puts the
	 * route, so manhattan + 8 is enough; searching further is the expensive way to
	 * find out it did not get there. */
	CHECK(HsbConnectRadius(0, 0) == 12);
	CHECK(HsbConnectRadius(4, 0) == 12);
	CHECK(HsbConnectRadius(20, 0) == 28);
	SECTION("a cap wins, so a per-route check stays bounded") {
		CHECK(HsbConnectRadius(200, 40) == 40);
		CHECK(HsbConnectRadius(200, 0) == 208);
		CHECK(HsbConnectRadius(2, 40) == 12);
	}
}

TEST_CASE("HSB detour budget lets a corridor go round mountains but not wander off")
{
	CHECK(HsbDetourAllowed(100, 100, 64));
	CHECK(HsbDetourAllowed(100, 164, 64));
	CHECK_FALSE(HsbDetourAllowed(100, 165, 64));
	/* Going the wrong way is always a bigger detour than going the right one. */
	CHECK(HsbDetourAllowed(100, 20, 64));
	CHECK(HsbDetourAllowed(0, 64, 64));
}

TEST_CASE("HSB rail sidings are built only to industries bulk cargo wants")
{
	/* A coal mine or a steel mill wants rail; a department store does not, and a
	 * siding to the wrong industry is track the player has to dig up again. */
	CHECK(HsbIndustryWantsRail("Coal Mine"));
	CHECK(HsbIndustryWantsRail("Iron Ore Mine"));
	CHECK(HsbIndustryWantsRail("Integrated Steel Mill"));
	CHECK(HsbIndustryWantsRail("Oil Refinery"));
	CHECK(HsbIndustryWantsRail("Power Station"));
	CHECK(HsbIndustryWantsRail("Sawmill"));
	CHECK(HsbIndustryWantsRail("Forest"));
	CHECK(HsbIndustryWantsRail("Salt Mine"));
	CHECK_FALSE(HsbIndustryWantsRail(""));
	CHECK_FALSE(HsbIndustryWantsRail("Bank"));
	CHECK_FALSE(HsbIndustryWantsRail("Farm Shop"));
	CHECK_FALSE(HsbIndustryWantsRail("coal mine"));
	CHECK_FALSE(HsbIndustryWantsRail("Coal"));
}

TEST_CASE("HSB water industries get a wider placement search")
{
	CHECK(HsbIndustryNeedsWater("Port"));
	CHECK(HsbIndustryNeedsWater("Oil Rig"));
	CHECK(HsbIndustryNeedsWater("Fishing Grounds"));
	CHECK_FALSE(HsbIndustryNeedsWater("Coal Mine"));
	CHECK_FALSE(HsbIndustryNeedsWater("port"));
}

TEST_CASE("HSB network dump ranks what is on the tile above what surrounds it")
{
	/* The distinction between R (a corridor the builder laid) and ^ (a town street)
	 * is the whole point. A town claim covers roughly 20x20 tiles, so ranking town
	 * membership first made most of the map come out as town and every street and
	 * corridor inside one invisible - a viewer that hides what you are looking for
	 * is worse than one that shows it too eagerly. */
	CHECK(HsbDumpRank('^') > HsbDumpRank(','));
	CHECK(HsbDumpRank('R') > HsbDumpRank('^'));
	CHECK(HsbDumpRank('S') > HsbDumpRank('t'));
	/* A junction outranks plain track: two lines meeting is the interesting thing
	 * in a block, a single rail tile is not. */
	CHECK(HsbDumpRank('t') > HsbDumpRank('T'));
	CHECK(HsbDumpRank('#') > HsbDumpRank('R'));
	CHECK(HsbDumpRank('~') > HsbDumpRank('.'));
	CHECK(HsbDumpRank(' ') == 0);

	SECTION("the rank order is strictly increasing along the priority chain") {
		const char *chain = " .~I,^DrR#TtS";
		REQUIRE(std::strlen(chain) == 13);
		for (size_t i = 1; i < std::strlen(chain); ++i) {
			CHECK(HsbDumpRank(chain[i]) > HsbDumpRank(chain[i - 1]));
		}
	}
	SECTION("an unknown character is treated as bare ground, never as a wall") {
		CHECK(HsbDumpRank('?') == HsbDumpRank('.'));
	}
}

TEST_CASE("HSB network dump shows the most notable thing in each block")
{
	/* One character per block over the whole map: everything else a build reports is
	 * a number, and a number cannot show that a corridor runs through the middle of
	 * nowhere. */
	SECTION("a block takes the highest-ranked character anywhere inside it") {
		const auto classify = [](int x, int y) -> char {
			if (y == 4 && x == 4) return 'S';
			return '.';
		};
		const auto rows = HsbDumpGrid(8, 8, 4, classify);
		REQUIRE(rows.size() == 2);
		CHECK(rows[0] == "..");
		CHECK(rows[1] == ".S");
	}
	SECTION("a town street inside a town block is still visible") {
		/* The whole point of the ranking: a town claim covers roughly 20x20 tiles,
		 * so ranking town membership first made every street inside one invisible. */
		const auto classify = [](int x, int y) -> char {
			if ((x == 1 && y == 1) || (x == 5 && y == 1)) return '^';
			if (y < 4 && x < 4) return ',';
			return '.';
		};
		const auto rows = HsbDumpGrid(8, 8, 4, classify);
		REQUIRE(rows.size() == 2);
		/* Block (0,0) is town with a street in it, and shows the street rather than
		 * the town it sits in. Block (1,0) has a street in open ground and shows it
		 * too. The bottom two blocks are bare. */
		CHECK(rows[0] == "^^");
		CHECK(rows[1] == "..");
	}
	SECTION("the grid covers the whole map, and is one character per step") {
		const auto flat = [](int, int) { return '.'; };
		const auto rows = HsbDumpGrid(9, 12, 4, flat);
		REQUIRE(rows.size() == 3);
		for (const auto &row : rows) CHECK(row.size() == 3);
	}
}

TEST_CASE("HSB manifest network ranges are documented where the builder checks them")
{
	/* One table, used by the builder to validate and by these tests to assert the
	 * same numbers. A limit that exists in the documentation but not here is a
	 * limit nobody checks. */
	CHECK(HsbInRange(kHsbNetworkMoney, 0));
	CHECK(HsbInRange(kHsbNetworkMoney, 4'000'000'000));
	CHECK_FALSE(HsbInRange(kHsbNetworkMoney, 4'000'000'001));
	CHECK_FALSE(HsbInRange(kHsbNetworkMoney, -1));
	CHECK_FALSE(HsbInRange(kHsbBusTowns, -1));
	CHECK(HsbInRange(kHsbBusTowns, kHsbBusTowns.fallback));
	CHECK_FALSE(HsbInRange(kHsbTrains, 1025));
	CHECK_FALSE(HsbInRange(kHsbSiteSearchRadius, 0));
	CHECK(HsbInRange(kHsbSiteSearchRadius, 40));
	CHECK(HsbInRange(kHsbTownRoadRadius, kHsbTownRoadRadius.fallback));
	/* A search radius of 0 would make every site search a single tile and fail
	 * every airport and dock, so the lower bound is 1, not 0. */
	CHECK(kHsbSiteSearchRadius.lo == 1);
	/* The bus and train defaults are non-zero: a fleet of nothing is not a fleet. */
	CHECK(kHsbBusTowns.fallback > 0);
	CHECK(kHsbTrains.fallback > 0);
}

/* ---------------------------------------------------------------------------
 * The manifest half of the network phase: every rule has to be a build error,
 * not an hour of routing.
 * ------------------------------------------------------------------------- */

TEST_CASE("HSB accepts a network manifest and rejects nothing in it")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json manifest = ValidManifest(directory.path);
	manifest["competitors"] = 2;
	manifest["roads"] = (directory.path / "roads.json").string();
	manifest["autobahnen"] = (directory.path / "autobahnen.json").string();
	manifest["rail"] = (directory.path / "rail.json").string();
	manifest["airports"] = (directory.path / "airports.json").string();
	manifest["harbours"] = (directory.path / "harbours.json").string();
	manifest["with_rail"] = true;
	manifest["with_fleet"] = true;
	manifest["network_money"] = 1000;
	manifest["bus_towns"] = 4;
	manifest["trains"] = 2;

	/* Every path is read directly rather than resolved by basename, and the ones
	 * written here do not exist: reaching the heightmap is what fails, not the
	 * network keys. */
	WriteFile(directory.path / "roads.json", "[]");
	WriteFile(directory.path / "autobahnen.json", "[]");
	WriteFile(directory.path / "rail.json", "[]");
	WriteFile(directory.path / "airports.json", "[]");
	WriteFile(directory.path / "harbours.json", "[]");
	CHECK_FALSE(RunManifest(directory.path, manifest));
}

TEST_CASE("HSB rejects a network input path that is relative or traverses")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json base = ValidManifest(directory.path);
	base["competitors"] = 2;

	for (const char *key : {"roads", "autobahnen", "rail", "airports", "harbours"}) {
		INFO("key " << key);
		nlohmann::json manifest = base;
		manifest[key] = "roads.json";
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
	for (const char *key : {"roads", "rail"}) {
		INFO("key " << key);
		nlohmann::json manifest = base;
		manifest[key] = "../roads.json";
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
}

TEST_CASE("HSB rejects a network input path of the wrong JSON type")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json base = ValidManifest(directory.path);
	base["competitors"] = 2;

	SECTION("a number where a path belongs") {
		nlohmann::json manifest = base;
		manifest["roads"] = 42;
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
	SECTION("an array where a path belongs") {
		nlohmann::json manifest = base;
		manifest["airports"] = nlohmann::json::array();
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
	SECTION("an empty string is treated as absent, so the phase is skipped") {
		/* A script that builds a path and gets it empty should not silently route
		 * a network, but it also should not be a hard error - that is the same as
		 * leaving the key out. */
		nlohmann::json manifest = base;
		manifest["roads"] = "";
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
	SECTION("null is the documented way to turn a phase off") {
		nlohmann::json manifest = base;
		manifest["roads"] = nullptr;
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
}

TEST_CASE("HSB rejects network counts and money that are out of range or mistyped")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json base = ValidManifest(directory.path);
	base["competitors"] = 2;
	base["roads"] = (directory.path / "roads.json").string();

	SECTION("out of range") {
		nlohmann::json manifest = base;
		manifest["network_money"] = -1;
		CHECK_FALSE(RunManifest(directory.path, manifest));
		manifest = base;
		manifest["network_money"] = 4'000'000'001;
		CHECK_FALSE(RunManifest(directory.path, manifest));
		manifest = base;
		manifest["bus_towns"] = 1025;
		CHECK_FALSE(RunManifest(directory.path, manifest));
		manifest = base;
		manifest["trains"] = 99999;
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
	SECTION("wrong type: a float where an integer count belongs") {
		/* nlohmann::json will happily hold 4.5 for a count; asking for an integer
		 * has to be an error, not a silent truncation. */
		nlohmann::json manifest = base;
		manifest["bus_towns"] = 4.5;
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
	SECTION("wrong type: a number where a boolean switch belongs") {
		nlohmann::json manifest = base;
		manifest["with_rail"] = 1;
		CHECK_FALSE(RunManifest(directory.path, manifest));
		manifest = base;
		manifest["with_fleet"] = "yes";
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
	SECTION("the boundary values are accepted") {
		nlohmann::json manifest = base;
		manifest["network_money"] = 0;
		manifest["bus_towns"] = 0;
		manifest["trains"] = 0;
		CHECK_FALSE(RunManifest(directory.path, manifest));
	}
}

TEST_CASE("HSB refuses a network without a free company slot for the player")
{
	TestDirectory directory;
	EnvironmentGuard environment;

	/* The infrastructure company that owns the network takes one slot. With zero
	 * or one slot the player opens the save as a spectator with nothing to build,
	 * which is invisible from the exit code and only shows up in a playthrough. */
	nlohmann::json manifest = ValidManifest(directory.path);
	manifest["competitors"] = 0;
	manifest["roads"] = (directory.path / "roads.json").string();
	WriteFile(directory.path / "roads.json", "[]");
	CHECK_FALSE(RunManifest(directory.path, manifest));

	manifest["competitors"] = 1;
	CHECK_FALSE(RunManifest(directory.path, manifest));

	manifest["competitors"] = 2;
	CHECK_FALSE(RunManifest(directory.path, manifest));

	SECTION("no network needs no second slot, so zero is still fine") {
		nlohmann::json plain = ValidManifest(directory.path);
		plain["competitors"] = 0;
		CHECK_FALSE(RunManifest(directory.path, plain));
	}
}

TEST_CASE("HSB refuses a rail file with the rail phase switched off")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json manifest = ValidManifest(directory.path);
	manifest["competitors"] = 2;
	manifest["rail"] = (directory.path / "rail.json").string();
	manifest["with_rail"] = false;
	WriteFile(directory.path / "rail.json", "[]");

	/* Track without stations and depots looks broken, so naming the file and
	 * refusing to build it is better than silently ignoring one of the two. */
	CHECK_FALSE(RunManifest(directory.path, manifest));
}

TEST_CASE("HSB refuses a fleet with no network to run on")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json manifest = ValidManifest(directory.path);
	manifest["competitors"] = 2;
	manifest["with_fleet"] = true;

	/* No roads and no rail: there is nothing to put a bus on. */
	CHECK_FALSE(RunManifest(directory.path, manifest));
}

TEST_CASE("HSB accepts a rail-only network with no roads")
{
	TestDirectory directory;
	EnvironmentGuard environment;
	nlohmann::json manifest = ValidManifest(directory.path);
	manifest["competitors"] = 2;
	manifest["rail"] = (directory.path / "rail.json").string();
	WriteFile(directory.path / "rail.json", "[]");

	/* Rail on its own is a legitimate scenario: a country with main lines and
	 * depots and no roads is a thing, and the builder must not insist on roads. */
	CHECK_FALSE(RunManifest(directory.path, manifest));
}
