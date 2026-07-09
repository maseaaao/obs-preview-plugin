#pragma once

#include <cstdint>
#include <vector>

class JpegEncoder {
public:
	static std::vector<uint8_t> encodeRgba(const uint8_t *rgba, int width, int height, int quality);
};
