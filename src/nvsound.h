/* nvsound.h - NVGT's sound engine: SDL3 device output, our own mixer tree, Steam Audio for spatialization.
 * This replaces the MiniAudio based engine in sound.cpp/sound_nodes.cpp and exposes the API that the legacy_sound plugin used to (sound, mixer, sound_environment).
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

class effect;
class mixer;
// Shared by sounds and mixers: refcounting, the output mixer, volume/pan/pitch with slides, and basic positioning.
class sound_base {
protected:
	std::atomic<int> ref_count{1};
	spinlock params_lock;
	slider volume{0}, pan{0}, pitch{100};
	float last_gain_left = -1, last_gain_right = -1; // -1 until the first block, so a sound doesn't ramp in from silence.
	float listener_x = 0, listener_y = 0, listener_z = 0, x = 0, y = 0, z = 0, rotation = 0, pan_step = 1, volume_step = 1;
	bool positioned = false;
	std::unique_ptr<WDL_Resampler> resampler;
	// The set_fx chain. The audio thread holds fx_mutex while processing this node's effects, the script while changing them.
	std::mutex fx_mutex;
	std::vector<std::unique_ptr<effect>> effects;
	std::atomic<int> effect_count{0};
	void process_fx(float* frames, int count);
	void apply_gain(const float* in, float* out, int frames, float vol_db, float pan_value, float position_pan);
	void positioning(float& vol_db, float& pan_value);
public:
	mixer* parent = nullptr;
	bool set_parent(mixer* m, bool internal = false); // Moves this node within the mixer tree without the checks set_mixer does.
	bool use_hrtf = false;
	virtual ~sound_base();
	void add_ref();
	void release();
	virtual void render(float* out, int frames, int depth) = 0;
	float get_volume();
	bool set_volume(float v);
	bool slide_volume(float v, unsigned int ms);
	float get_pan();
	bool set_pan(float v);
	bool slide_pan(float v, unsigned int ms);
	float get_pitch();
	bool set_pitch(float v);
	bool slide_pitch(float v, unsigned int ms);
	bool is_sliding();
	bool is_volume_sliding();
	bool is_pan_sliding();
	bool is_pitch_sliding();
	bool set_position(float listener_x, float listener_y, float listener_z, float sound_x, float sound_y, float sound_z, float rotation, float pan_step, float volume_step);
	void set_hrtf(bool enable) { use_hrtf = enable; }
	int set_fx(const std::string& fx, int index = -1);
	virtual bool set_mixer(mixer* m) = 0;
};

class mixer : public sound_base {
	std::vector<sound_base*> children;
	friend class sound_base;
public:
	mixer(bool is_master = false);
	~mixer();
	void render(float* out, int frames, int depth) override;
	bool set_mixer(mixer* m) override;
	bool is_ancestor_of(mixer* m);
};

class sound : public sound_base {
	std::shared_ptr<source> src;
	std::atomic<bool> playing{false}, ended{false}, looping{false};
	float forced_length = 0; // set_length, in milliseconds.
	std::shared_ptr<pcm_push_source> pcm_pushing;
	std::shared_ptr<push_bytes> bytes_pushing;
	void attach_source(std::shared_ptr<source> s, const std::string& filename);
	bool load_bytes(std::shared_ptr<const std::string> bytes, const std::string& cache_key, const std::string& name, bool cache);
	bool attach_cached(const std::string& cache_key, const std::string& name);
	bool start_pushed_decoder();
public:
	std::string loaded_filename;
	~sound();
	void render(float* out, int frames, int depth) override;
	bool set_mixer(mixer* m) override;
	bool load(const std::string& filename, const pack_interface* pack, bool allow_preloads = true);
	bool stream(const std::string& filename, const pack_interface* pack);
	bool load_url(const std::string& url);
	bool load_memory(std::string& data, unsigned int size, const std::string& preload_filename = "", bool legacy_encrypt = false);
	bool load_callbacks(asIScriptFunction* close_cb, asIScriptFunction* length_cb, asIScriptFunction* read_cb, asIScriptFunction* seek_cb, const std::string& data, const std::string& preload_filename = "");
	bool push_memory(const std::string& data, bool end_stream = false, int pcm_rate = 0, int pcm_channels = 0);
	bool push_floats(CScriptArray* data, bool end_stream = false, int pcm_rate = 0, int pcm_channels = 0);
	bool close();
	bool play(bool reset_loop_state = true);
	bool play_wait();
	bool play_looped();
	bool pause();
	bool stop();
	bool seek(float ms);
	bool is_active();
	bool is_playing();
	bool is_paused();
	float get_length();
	float get_position();
	void set_length(float ms) { if (ms >= 0) forced_length = ms; }
	const std::string& get_loaded_filename() const { return loaded_filename; }
	double get_pitch_lower_limit() const { return 0; }
};

// Engine level functions.
bool init();
void shutdown();
bool is_available();
int get_sample_rate();
uint64_t get_frame_clock();
std::recursive_mutex& get_graph_mutex(); // Held by the audio thread while rendering and by anything that changes the mixer tree.
mixer* get_master();
mixer* get_default_mixer();
float get_master_volume();
void set_master_volume(float v);
unsigned int get_output_device();
bool set_output_device(unsigned int index);
std::vector<std::string> get_output_device_names();
void set_default_pack(const pack_interface* pack);
const pack_interface* get_default_pack();
void set_decryption_key(const std::string& key);
unsigned int get_input_device();
void set_input_device(unsigned int index);
std::vector<std::string> get_input_device_names();

void close_all_microphones(); // Called by shutdown, so no microphone touches SDL after SDL_Quit.

void RegisterNvsound(asIScriptEngine* engine);
void RegisterNvsoundMicrophone(asIScriptEngine* engine);

} // namespace nvsound
