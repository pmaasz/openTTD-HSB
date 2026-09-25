#ifndef HSB_BUILDER_MATH_H
#define HSB_BUILDER_MATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

struct HsbBounds {
	double lat_min = 0.0;
	double lat_max = 0.0;
	double lon_min = 0.0;
	double lon_max = 0.0;
};

struct HsbTile {
	uint32_t x = 0;
	uint32_t y = 0;
};

inline bool HsbIsPowerOfTwo(uint32_t value, uint32_t minimum, uint32_t maximum)
{
	return value >= minimum && value <= maximum && (value & (value - 1)) == 0;
}

inline bool HsbCoordinateInBounds(const HsbBounds &bounds, double lat, double lon)
{
	return std::isfinite(lat) && std::isfinite(lon)
		&& bounds.lat_min <= lat && lat <= bounds.lat_max
		&& bounds.lon_min <= lon && lon <= bounds.lon_max;
}

inline HsbTile HsbTileForBounds(const HsbBounds &bounds, double lat, double lon,
	uint32_t max_x, uint32_t max_y, bool counter_clockwise)
{
	const double x_prop = (bounds.lat_max - lat) / (bounds.lat_max - bounds.lat_min);
	const double y_prop = (lon - bounds.lon_min) / (bounds.lon_max - bounds.lon_min);
	const double tile_x = counter_clockwise ? 1.0 - y_prop : x_prop;
	const double tile_y = counter_clockwise ? x_prop : y_prop;
	const long clamped_x = std::clamp(std::lround(tile_x * max_x), 0L, static_cast<long>(max_x));
	const long clamped_y = std::clamp(std::lround(tile_y * max_y), 0L, static_cast<long>(max_y));
	return {static_cast<uint32_t>(clamped_x), static_cast<uint32_t>(clamped_y)};
}

inline bool HsbSafeFilename(const std::string &filename)
{
	if (filename.empty() || filename == "." || filename == "..") return false;
	return filename.find('\0') == std::string::npos
		&& filename.find_first_of("/\\:") == std::string::npos
		&& std::filesystem::path(filename).filename().string() == filename;
}

#endif
