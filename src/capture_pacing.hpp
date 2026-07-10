#pragma once

#include <cstdint>

// Keep the ideal output timeline, rather than moving it to the latest source
// frame. That avoids aliasing 60 -> 30 -> 15 FPS when the target is 25 FPS.
inline bool acceptFrameTimestamp(uint64_t &lastScheduledTimestamp, bool &hasScheduledTimestamp, uint64_t frameIntervalNs,
					 uint64_t timestamp)
{
	if (frameIntervalNs == 0 || !hasScheduledTimestamp || timestamp < lastScheduledTimestamp) {
		lastScheduledTimestamp = timestamp;
		hasScheduledTimestamp = true;
		return true;
	}
	const auto elapsed = timestamp - lastScheduledTimestamp;
	if (elapsed < frameIntervalNs)
		return false;

	lastScheduledTimestamp += (elapsed / frameIntervalNs) * frameIntervalNs;
	return true;
}
