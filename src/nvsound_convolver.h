/* nvsound_convolver.h - uniformly partitioned FFT convolution, for mixer.set_impulse_response.
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
#include <cstdint>
#include <memory>
#include <mutex>

namespace nvsound {

// Convolves interleaved audio with a recorded impulse response. The algorithm is the one in dep/ma_convolution_node.c, moved out of its MiniAudio node.
class convolver {
	struct state;
	std::unique_ptr<state> st;
	std::mutex m; // The audio thread only ever waits on this for a pointer swap.
	unsigned int channels, sample_rate, partition_size;
public:
	convolver(unsigned int channels, unsigned int sample_rate, unsigned int partition_size = 512);
	~convolver();
	// Builds everything a response needs, then swaps it in. tail_seconds above zero cuts the response to that length; rate_divisor (a power of two) runs the wet path at a fraction of the sample rate to save work. A mono response is used for every channel. Returns false for unusable arguments.
	bool set_ir(const float* frames, uint64_t frame_count, unsigned int ir_channels, float tail_seconds = 0, unsigned int rate_divisor = 1);
	void clear_ir();
	bool has_ir();
	// out = dry_in * dry + (conv_in convolved) * wet. conv_in and dry_in can be different, so the wet path can be fed a narrowed stereo image. out may be the same as either input.
	void process(const float* conv_in, const float* dry_in, float* out, unsigned int frames, float dry, float wet);
};

} // namespace nvsound
