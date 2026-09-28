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

/**
 * Visit every tile of the shortest 4-connected staircase from (x0, y0) to (x1, y1).
 *
 * Ships follow water edges, so a river rasterised as a straight line between two
 * waypoints yields tiles that only touch at their corners wherever it runs
 * diagonally: a chain of ponds that looks connected on the map and is not.
 * The staircase moves one tile at a time along the four cardinal directions and
 * visits both ends of every step, so consecutive visits share an edge.
 *
 * Horizontal moves come first, then vertical. The callback is invoked with
 * (x, y, horizontal) and is expected to be called for every tile on the path,
 * the start included and the end included. The path is empty when the start
 * already is the end.
 */
template <typename Tstep>
inline void HsbStaircaseWalk(int x0, int y0, int x1, int y1, Tstep step)
{
	const int dx = (x1 > x0) - (x1 < x0);
	const int dy = (y1 > y0) - (y1 < y0);
	int x = x0;
	int y = y0;
	step(x, y, true);
	while (x != x1) {
		x += dx;
		step(x, y, true);
	}
	while (y != y1) {
		y += dy;
		step(x, y, false);
	}
}

inline bool HsbSafeFilename(const std::string &filename)
{
	if (filename.empty() || filename == "." || filename == "..") return false;
	return filename.find('\0') == std::string::npos
		&& filename.find_first_of("/\\:") == std::string::npos
		&& std::filesystem::path(filename).filename().string() == filename;
}

#endif
