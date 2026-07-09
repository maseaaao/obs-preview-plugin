#pragma once

#include <cstdint>
#include <vector>

class JpegEncoder {
public:
	static std::vector<uint8_t> encodeBgr(const uint8_t *bgr, int width, int height, int quality);
};
