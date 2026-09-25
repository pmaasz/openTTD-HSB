#include "../stdafx.h"

#include "../3rdparty/catch2/catch.hpp"

#include "../hsb_builder.h"
#include "../hsb_builder_math.h"
#include "../3rdparty/nlohmann/json.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

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
