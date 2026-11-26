/* nvsound_common.h - what NVGT's sound engine cores share: decoders, sources, sliders and the engine's sample rate and clock.
 * The effects, convolution, HRTF and decoding code use only this, so the same code runs under either core.
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
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <istream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class asIScriptEngine;
class asIScriptFunction;
class CScriptArray;
class WDL_Resampler;
class pack_interface;

namespace nvsound {

// Everything renders in blocks of this many frames, because Steam Audio's effects must be fed fixed sized buffers.
constexpr int block_frames = 256;
// The largest request any node can receive in one call, which happens when a pitched mixer asks its children for extra frames.
constexpr int max_render_frames = 8192;
constexpr int max_source_channels = 8;

// A spinlock for the handful of parameters (volume, pan, pitch, position) that both the script and audio threads touch. Critical sections are a few instructions long, so spinning is cheaper than a kernel mutex.
class spinlock {
	std::atomic_flag flag = ATOMIC_FLAG_INIT;
public:
	void lock() { while (flag.test_and_set(std::memory_order_acquire)) {} }
	void unlock() { flag.clear(std::memory_order_release); }
};

// A value that can be set instantly or moved to a target over time, measured on the engine's frame clock.
struct slider {
	float value, from = 0, to = 0;
	uint64_t start = 0, end = 0;
	bool sliding = false;
	explicit slider(float v = 0) : value(v) {}
	void set(float v) { value = v; sliding = false; }
	void slide(float target, uint64_t now, uint64_t duration);
	float at(uint64_t now);
};

// Decoders. Each reads interleaved float frames from a std::istream that it owns.
class decoder {
public:
	std::unique_ptr<std::istream> stream;
	unsigned int channels = 0, sample_rate = 0;
	virtual ~decoder() {}
	virtual bool open() = 0;
	virtual uint64_t read(float* out, uint64_t frames) = 0;
	virtual bool seek(uint64_t frame) = 0;
	virtual uint64_t length() = 0; // 0 when unknown.
};
std::unique_ptr<decoder> open_decoder(std::unique_ptr<std::istream> stream);
std::unique_ptr<std::istream> open_memory_stream(std::shared_ptr<const std::string> data);
// Wraps a stream that can't seek so open_decoder can rewind over its first window bytes instead of reading all of it into memory. The source isn't owned.
std::unique_ptr<std::istream> open_window_stream(std::istream* source, size_t window = 128 * 1024);
// Opens a file the way sound.load does: from pack if it's active, otherwise from the filesystem, with the default decryption key applied.
std::unique_ptr<std::istream> open_sound_file(const std::string& filename, const pack_interface* pack);

// Fully decoded audio, already converted to the engine's sample rate.
struct pcm_data {
	std::vector<float> samples;
	unsigned int channels = 0, sample_rate = 0;
	uint64_t frames = 0;
};

// What a sound plays. read always writes exactly the number of frames asked for, padding with silence, and sets ended once no further data will ever arrive.
class source {
public:
	virtual ~source() {}
	virtual unsigned int channels() const = 0;
	virtual unsigned int sample_rate() const = 0;
	virtual uint64_t read(float* out, uint64_t frames, bool& ended) = 0;
	virtual bool seek(uint64_t frame) = 0;
	virtual uint64_t position() = 0;
	virtual uint64_t length() = 0; // 0 when unknown.
	virtual void set_loop(bool loop) = 0;
};
std::shared_ptr<source> make_pcm_source(std::shared_ptr<const pcm_data> data);
// Streams normally share one feeder thread. A stream whose reads can block, on a network or on a script that hasn't pushed more data yet, gets a thread of its own instead, and abort must make any blocked read return at once. Such streams can't seek.
std::shared_ptr<source> make_stream_source(std::unique_ptr<decoder> dec, bool dedicated_thread = false, std::function<void()> abort = nullptr, uint64_t prefill_frames = ~uint64_t(0));

// Raw float PCM pushed by a script (push_memory with a sample rate). Unbounded like BASS's push streams: pushing never blocks and never drops.
class pcm_push_source;
std::shared_ptr<pcm_push_source> make_pcm_push_source(unsigned int sample_rate, unsigned int channels);
void pcm_push(pcm_push_source* s, const float* samples, size_t count);
void pcm_push_end(pcm_push_source* s);
void pcm_push_clear(pcm_push_source* s, uint64_t fade_frames); // Drops everything waiting to play, fading out over the next fade_frames instead of cutting off.
uint64_t pcm_push_queued(pcm_push_source* s); // Frames waiting to be played.
std::shared_ptr<source> as_source(std::shared_ptr<pcm_push_source> s);

// An encoded file pushed in pieces (push_memory without a sample rate). Reads wait for more data once the decoder is running.
struct push_bytes;
std::shared_ptr<push_bytes> make_push_bytes();
void push_bytes_append(push_bytes* p, const char* data, size_t size, bool end);
size_t push_bytes_size(push_bytes* p);
std::unique_ptr<std::istream> open_push_stream(std::shared_ptr<push_bytes> p); // Reads return what has arrived so far until set_push_blocking.
void set_push_blocking(push_bytes* p);
void abort_push_bytes(push_bytes* p);
std::vector<float> resample_interleaved(const float* in, uint64_t frames, unsigned int channels, unsigned int rate_in, unsigned int rate_out, uint64_t& frames_out);
void stop_stream_feeder();
void feed_streams_now(); // Tops every feeder stream right up, on the calling thread. For offline rendering.

// Provided by whichever core is built.
bool init();
int get_sample_rate();
uint64_t get_frame_clock();

} // namespace nvsound
