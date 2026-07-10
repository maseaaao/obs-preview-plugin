#include "capture_pacing.hpp"
#include "settings.hpp"
#include "tls_connection.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>

int main()
{
	assert(maxTlsBufferedBytes == 64 * 1024);
	assert(SettingsStore::normalizeFps(1) == 1);
	assert(SettingsStore::normalizeFps(9) == 5);
	assert(SettingsStore::normalizeFps(61) == 60);
	assert(SettingsStore::normalizeResolutionScale(34) == 33);
	assert(SettingsStore::normalizeResolutionScale(99) == 75);

	const auto half = SettingsStore::scaledResolution(1920, 1080, 50);
	assert(half.width == 960 && half.height == 540);
	const auto third = SettingsStore::scaledResolution(1920, 1080, 33);
	assert(third.width == 634 && third.height == 356);
	assert(SettingsStore::migrateResolutionScale(640, 360, 1920, 1080) == 33);

	uint64_t last = 0;
	bool hasLast = false;
	constexpr uint64_t interval = 1000000000ULL / 25;
	assert(acceptFrameTimestamp(last, hasLast, interval, 0));
	assert(!acceptFrameTimestamp(last, hasLast, interval, interval - 1));
	assert(acceptFrameTimestamp(last, hasLast, interval, interval));

	for (const auto requested : {1, 2, 5, 25, 35, 60}) {
		const int sourceFps = 60;
		const int divisor = std::max(1, sourceFps / requested);
		const uint64_t targetInterval = 1000000000ULL / requested;
		uint64_t scheduled = 0;
		bool hasScheduled = false;
		int accepted = 0;
		for (int frame = 0; frame < sourceFps * 10; ++frame) {
			if (frame % divisor != 0)
				continue;
			const auto timestamp = static_cast<uint64_t>(frame) * 1000000000ULL / sourceFps;
			if (acceptFrameTimestamp(scheduled, hasScheduled, targetInterval, timestamp))
				++accepted;
		}
		assert(std::abs(accepted - requested * 10) <= 1);
	}
	return 0;
}
