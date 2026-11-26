/* nvsound_timing.h - how long the audio device's callbacks take, against the time each one is given. A callback over its budget is a dropout you can hear. Both sound cores keep these, without locks or allocation, and print them at shutdown when NVSOUND_REPORT is set.
 *
 * NVGT - NonVisual Gaming Toolkit
 * Copyright (c) 2022-2025 Sam Tupy
 * https://nvgt.dev
 * This software is provided "as-is", without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
*/

#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace nvsound {

struct callback_timing {
	// Callback durations in steps of 0.1 ms up to 50 ms, the last step holding everything longer.
	static constexpr int steps = 500;
	std::atomic<uint64_t> histogram[steps + 1] = {};
	std::atomic<uint64_t> callbacks{0}, over_budget{0}, worst_us{0}, frames{0};
	void record(std::chrono::steady_clock::duration took, uint64_t frames_asked, int sample_rate) {
		uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(took).count());
		uint64_t budget_us = frames_asked * 1000000 / uint64_t(sample_rate > 0 ? sample_rate : 48000);
		callbacks.fetch_add(1, std::memory_order_relaxed);
		frames.fetch_add(frames_asked, std::memory_order_relaxed);
		if (us > budget_us) over_budget.fetch_add(1, std::memory_order_relaxed);
		uint64_t w = worst_us.load(std::memory_order_relaxed);
		while (us > w && !worst_us.compare_exchange_weak(w, us, std::memory_order_relaxed)) {}
		histogram[us / 100 < steps ? us / 100 : steps].fetch_add(1, std::memory_order_relaxed);
	}
	// The duration below which this share of callbacks fell, in ms.
	double percentile(double share) const {
		uint64_t total = callbacks, seen = 0;
		for (int i = 0; i <= steps; i++) {
			seen += histogram[i];
			if (total && double(seen) >= share * double(total)) return (i + 1) / 10.0;
		}
		return steps / 10.0;
	}
	void report(const char* core) const {
		std::fprintf(stderr, "nvsound %s: %llu callbacks, %llu over budget, worst %.2f ms, 99%% under %.1f ms, 99.9%% under %.1f ms\n", core, (unsigned long long)callbacks.load(), (unsigned long long)over_budget.load(), worst_us.load() / 1000.0, percentile(0.99), percentile(0.999));
	}
};

} // namespace nvsound
