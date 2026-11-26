/* core.cpp - the new nvsound core: the audio thread's render, the commands that change it, and the script side's sounds and mixers. See DESIGN.md.
 * The render math, loading, caching and device handling are today's nvsound.cpp, moved. What's new is how changes reach the audio thread: as commands it applies at the start of a block, so it never waits on a lock.
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

#include "core.h"
#include "graph.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <SDL3/SDL.h>
#include <resample.h>

namespace nvsound {

// Engine state.
static int g_sample_rate = 48000;
static std::atomic<uint64_t> g_frame_clock{0};
static mixer* g_master = nullptr;
static mixer* g_default_mixer = nullptr;
static std::atomic<float> g_master_volume{0};
static unsigned int g_output_device = 1; // 0 is no sound, 1 follows the system default, and real devices start at 2, as they did under BASS.
static unsigned int g_input_device = 0xffffffff;
static bool g_initialized = false;
static bool g_shut_down = false;
static bool g_global_hrtf = true;
static SDL_AudioStream* g_output_stream = nullptr;
static const double g_preload_max_seconds = 30.0;
static command_queue<command> g_commands;
static garbage g_garbage;
static std::unique_ptr<host> g_host;
static void release_played(bool all);

int get_sample_rate() { return g_sample_rate; }
uint64_t get_frame_clock() { return g_frame_clock; }
mixer* get_master() { return g_master; }
mixer* get_default_mixer() { return g_default_mixer ? g_default_mixer : g_master; }
void set_default_mixer(mixer* m) { g_default_mixer = m; }
mixer** default_mixer_slot() { return &g_default_mixer; }
void send(const command& c) { g_commands.push(c); }
void retire(retired r) { g_garbage.retire(r); }
size_t collect_garbage() { return g_garbage.collect(); }

static uint64_t ms_to_frames(unsigned int ms) { return uint64_t(ms) * g_sample_rate / 1000; }
static float db_to_gain(float db) { return db <= -100 ? 0.0f : powf(10.0f, db / 20.0f); }

// The default host: plain files, a sleeping wait, and no logging.
std::string host::cache_key(const std::string& filename, const pack_interface*) { return filename; }
std::unique_ptr<std::istream> host::open(const std::string& cache_key) {
	auto f = std::make_unique<std::ifstream>(cache_key, std::ios::binary);
	if (!*f) return nullptr;
	return f;
}
void host::wait(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void set_host(std::unique_ptr<host> h) { g_host = std::move(h); }
host& get_host() {
	if (!g_host) g_host = std::make_unique<host>();
	return *g_host;
}

node::~node() {
	delete resampler;
	delete src;
}

static WDL_Resampler* make_resampler() {
	WDL_Resampler* r = new WDL_Resampler();
	r->SetMode(false, 0, true, 16, 32);
	// Sized up front, on the script side, so the audio thread never grows its buffers.
	r->Prealloc(2, max_render_frames + 64, max_render_frames);
	return r;
}

// Scratch buffers for the audio thread, one set per depth of the mixer tree, since a mixer's buffer must survive while its children render. Only one thread renders at a time, so they're shared, and sized when the engine starts.
static constexpr int scratch_depths = 16;
static std::vector<float> g_scratch[2][scratch_depths + 1];
static void prepare_scratch() {
	for (int d = 0; d <= scratch_depths; d++) {
		g_scratch[0][d].assign(size_t(max_render_frames) * 2, 0.0f);
		g_scratch[1][d].assign(size_t(max_render_frames) * max_source_channels, 0.0f);
	}
}
static float* scratch(int which, int depth, size_t floats) {
	// Deeper than any real tree. Growing here would allocate, which is counted.
	if (depth > scratch_depths) depth = scratch_depths;
	auto& b = g_scratch[which][depth];
	if (b.size() < floats) {
		if (on_audio_thread()) note_audio_thread_alloc();
		b.resize(floats);
	}
	return b.data();
}

// Rendering. Everything from here to apply() is the audio thread's.

// Without HRTF, the legacy engine's positioning: lose a decibel per unit of distance scaled by volume_step, pan by the x offset scaled by pan_step.
static void spatialize(node* n, float*, int, const spatial_params& p, bool is_positioned, float& vol_db, float& pan_value) {
	vol_db = 0;
	pan_value = 0;
	(void)n;
	if (!is_positioned) return;
	float dx = p.x - p.listener_x, dy = p.y - p.listener_y, dz = p.z - p.listener_z;
	if (p.rotation > 0) {
		float rx = cosf(p.rotation) * dx - sinf(p.rotation) * dy;
		float ry = sinf(p.rotation) * dx + cosf(p.rotation) * dy;
		dx = rx;
		dy = ry;
	}
	float v = 1.0f - floorf(sqrtf(dx * dx + dy * dy + dz * dz)) / (125.0f / p.volume_step);
	vol_db = std::clamp(v, 0.0f, 1.0f) * 100.0f - 100.0f;
	pan_value = std::clamp(dx / (125.0f / p.pan_step), -1.0f, 1.0f) * 100.0f;
}
// The left and right gains for a volume and pans. The script's pan and the positioning pan were applied by separate stages under BASS, so their attenuations multiply rather than cancel.
static void stereo_gains(float vol_db, float pan_value, float position_pan, float& gl, float& gr) {
	gl = gr = db_to_gain(vol_db);
	for (float p : {pan_value, position_pan}) {
		if (p < 0) gr *= db_to_gain(p);
		else if (p > 0) gl *= db_to_gain(-p);
	}
}
// Mixes stereo input into the output with the given volume and pans, ramping from the previous block's gains so that changes don't click.
static void apply_gain(node* n, const float* in, float* out, int frames, float vol_db, float pan_value, float position_pan) {
	float gl, gr;
	stereo_gains(vol_db, pan_value, position_pan, gl, gr);
	if (n->last_gain_left < 0) {
		n->last_gain_left = gl;
		n->last_gain_right = gr;
	}
	float sl = n->last_gain_left, sr = n->last_gain_right, dl = (gl - sl) / frames, dr = (gr - sr) / frames;
	for (int f = 0; f < frames; f++) {
		sl += dl;
		sr += dr;
		out[f * 2] += in[f * 2] * sl;
		out[f * 2 + 1] += in[f * 2 + 1] * sr;
	}
	n->last_gain_left = gl;
	n->last_gain_right = gr;
}
// The node's volume, pan and pitch at now, published for the script side's getters.
static void read_params(node* n, uint64_t now, float& vol, float& pn, float& pt) {
	vol = n->volume.at(now);
	pn = n->pan.at(now);
	pt = n->pitch.at(now);
	node_status& s = *n->st;
	s.volume.store(vol, std::memory_order_relaxed);
	s.pan.store(pn, std::memory_order_relaxed);
	s.pitch.store(pt, std::memory_order_relaxed);
	s.volume_sliding.store(n->volume.sliding, std::memory_order_relaxed);
	s.pan_sliding.store(n->pan.sliding, std::memory_order_relaxed);
	s.pitch_sliding.store(n->pitch.sliding, std::memory_order_relaxed);
}
static void render_node(node* n, float* out, int frames, int depth);
static void render_mixer(node* n, float* out, int frames, int depth) {
	if (depth > 64) return; // A cycle can't be built through set_mixer, but never recurse forever.
	uint64_t now = g_frame_clock;
	float vol, pn, pt, pos_vol, pos_pan;
	read_params(n, now, vol, pn, pt);
	bool is_positioned = n->positioned;
	spatial_params sp = n->sp;
	float* acc = scratch(0, depth, size_t(frames) * 2);
	if (!n->resampler) {
		memset(acc, 0, frames * 2 * sizeof(float));
		for (node* c = n->first_child; c; c = c->next) render_node(c, acc, frames, depth + 1);
	} else {
		// A pitched mixer speeds up or slows down everything in it, so it asks its children for more or fewer frames and resamples the mix.
		n->resampler->SetRates(double(g_sample_rate) * pt / 100.0, g_sample_rate);
		WDL_ResampleSample* in;
		int need = (std::min)(n->resampler->ResamplePrepare(frames, 2, &in), max_render_frames);
		memset(in, 0, need * 2 * sizeof(float));
		for (node* c = n->first_child; c; c = c->next) render_node(c, in, need, depth + 1);
		int got = n->resampler->ResampleOut(acc, need, frames, 2);
		if (got < frames) memset(acc + got * 2, 0, (frames - got) * 2 * sizeof(float));
	}
	spatialize(n, acc, frames, sp, is_positioned, pos_vol, pos_pan);
	apply_gain(n, acc, out, frames, vol + pos_vol + n->dry_volume, pn, pos_pan);
}
static void render_sound(node* n, float* out, int frames, int depth) {
	if (!n->src || !n->playing) return;
	source* src = n->src->get();
	uint64_t now = g_frame_clock;
	float vol, pn, pt, pos_vol, pos_pan;
	read_params(n, now, vol, pn, pt);
	bool is_positioned = n->positioned;
	spatial_params sp = n->sp;
	unsigned int ch = src->channels();
	float* raw = scratch(1, depth, size_t(max_render_frames) * ch);
	float* st = scratch(0, depth, size_t(frames) * 2);
	bool src_ended = false;
	auto pull = [&](float* dest, int count) {
		src->read(raw, count, src_ended);
		if (ch == 1) {
			for (int f = 0; f < count; f++) dest[f * 2] = dest[f * 2 + 1] = raw[f];
		} else {
			for (int f = 0; f < count; f++) {
				dest[f * 2] = raw[f * ch];
				dest[f * 2 + 1] = raw[f * ch + 1];
			}
		}
	};
	if (!n->resampler) pull(st, frames);
	else {
		n->resampler->SetRates(double(src->sample_rate()) * pt / 100.0, g_sample_rate);
		WDL_ResampleSample* in;
		int need = (std::min)(n->resampler->ResamplePrepare(frames, 2, &in), max_render_frames);
		pull(in, need);
		int got = n->resampler->ResampleOut(st, need, frames, 2);
		if (got < frames) memset(st + got * 2, 0, (frames - got) * 2 * sizeof(float));
	}
	spatialize(n, st, frames, sp, is_positioned, pos_vol, pos_pan);
	apply_gain(n, st, out, frames, vol + pos_vol + n->dry_volume, pn, pos_pan);
	if (src_ended) {
		n->playing = false;
		n->ended = true;
		n->st->playing.store(false);
		n->st->ended.store(true);
	}
}
static void render_node(node* n, float* out, int frames, int depth) {
	if (n->is_mixer) render_mixer(n, out, frames, depth);
	else render_sound(n, out, frames, depth);
}

static node* g_master_node = nullptr;
static void unlink(node* n) {
	node* p = n->parent;
	if (!p) return;
	if (n->prev) n->prev->next = n->next;
	else p->first_child = n->next;
	if (n->next) n->next->prev = n->prev;
	else p->last_child = n->prev;
	n->parent = n->prev = n->next = nullptr;
}
static void link(node* n, node* p) {
	unlink(n);
	if (!p) return;
	n->parent = p;
	n->prev = p->last_child;
	if (p->last_child) p->last_child->next = n;
	else p->first_child = n;
	p->last_child = n;
}
static void publish_slider(const slider& s, std::atomic<bool>& sliding, std::atomic<uint64_t>& end) {
	sliding.store(s.sliding);
	end.store(s.end);
}
static void apply(const command& c) {
	node* n = c.target;
	node_status& s = *n->st;
	switch (c.kind) {
		case op::add:
			link(n, c.other);
			break;
		case op::destroy: {
			// Whatever was playing through a destroyed mixer carries on through the master, as it did under BASS.
			while (node* child = n->first_child) link(child, n == g_master_node ? nullptr : g_master_node);
			unlink(n);
			retire(retire_as(n));
			break;
		}
		case op::set_parent:
			link(n, c.other);
			break;
		case op::set_volume:
			n->volume.set(c.value);
			s.volume.store(c.value);
			s.volume_seq.store(c.seq);
			publish_slider(n->volume, s.volume_sliding, s.volume_end);
			break;
		case op::slide_volume:
			n->volume.slide(c.value, g_frame_clock, c.frames);
			s.volume_seq.store(c.seq);
			publish_slider(n->volume, s.volume_sliding, s.volume_end);
			break;
		case op::set_pan:
			n->pan.set(c.value);
			s.pan.store(c.value);
			s.pan_seq.store(c.seq);
			publish_slider(n->pan, s.pan_sliding, s.pan_end);
			break;
		case op::slide_pan:
			n->pan.slide(c.value, g_frame_clock, c.frames);
			s.pan_seq.store(c.seq);
			publish_slider(n->pan, s.pan_sliding, s.pan_end);
			break;
		case op::set_pitch:
			n->pitch.set(c.value);
			s.pitch.store(c.value);
			s.pitch_seq.store(c.seq);
			publish_slider(n->pitch, s.pitch_sliding, s.pitch_end);
			break;
		case op::slide_pitch:
			n->pitch.slide(c.value, g_frame_clock, c.frames);
			s.pitch_seq.store(c.seq);
			publish_slider(n->pitch, s.pitch_sliding, s.pitch_end);
			break;
		case op::set_resampler:
			if (n->resampler) retire(retire_as(n->resampler));
			n->resampler = static_cast<WDL_Resampler*>(c.ptr);
			break;
		case op::set_position:
			n->sp = c.sp;
			n->positioned = c.flag;
			break;
		case op::set_dry_volume:
			n->dry_volume = c.value;
			break;
		case op::attach_source:
			if (n->src) retire(retire_as(n->src));
			n->src = static_cast<std::shared_ptr<source>*>(c.ptr);
			n->playing = n->ended = n->looping = false;
			n->last_gain_left = n->last_gain_right = -1;
			s.playing.store(false);
			s.ended.store(false);
			s.transport_seq.store(c.seq);
			break;
		case op::detach_source:
			if (n->src) retire(retire_as(n->src));
			n->src = nullptr;
			n->playing = n->ended = false;
			s.playing.store(false);
			s.ended.store(false);
			s.transport_seq.store(c.seq);
			break;
		case op::play:
			n->ended = false;
			n->playing = true;
			s.ended.store(false);
			s.playing.store(true);
			s.transport_seq.store(c.seq);
			break;
		case op::pause:
			n->playing = false;
			s.playing.store(false);
			s.transport_seq.store(c.seq);
			break;
		case op::stop:
			n->playing = false;
			n->ended = false;
			s.playing.store(false);
			s.ended.store(false);
			s.transport_seq.store(c.seq);
			break;
		case op::set_looping:
			n->looping = c.flag;
			break;
	}
}

// One block of the master mix, the way the output device gets it. buf holds block_frames stereo frames.
static void render_block(float* buf) {
	audio_scope scope;
	g_commands.take(apply);
	g_garbage.flush_overflow();
	memset(buf, 0, sizeof(float) * block_frames * 2);
	if (g_master_node) render_node(g_master_node, buf, block_frames, 0);
	g_frame_clock += block_frames;
	// Master volume was linear under BASS rather than in decibels, so it stays that way.
	float gain = std::clamp(1.0f + g_master_volume / 100.0f, 0.0f, 1.0f);
	for (int i = 0; i < block_frames * 2; i++) buf[i] = std::clamp(buf[i] * gain, -1.0f, 1.0f);
}
static void SDLCALL audio_callback(void*, SDL_AudioStream* stream, int additional, int) {
	float buf[block_frames * 2];
	for (int needed = additional / int(sizeof(float) * 2); needed > 0; needed -= block_frames) {
		render_block(buf);
		SDL_PutAudioStreamData(stream, buf, sizeof(buf));
	}
}

// Offline rendering. The script renders blocks itself between its own changes, so the same script makes the same file every time.
static bool g_offline = false;
static std::ofstream g_offline_file;
static uint64_t g_offline_frames_written = 0;
static double g_offline_owed = 0;
static int64_t g_offline_clock_start = 0;
static void write_wav_header(std::ostream& f, uint64_t frames) {
	auto u32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
	auto u16 = [&f](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
	uint32_t data_bytes = uint32_t(frames * 2 * sizeof(float));
	f.write("RIFF", 4);
	u32(36 + data_bytes);
	f.write("WAVEfmt ", 8);
	u32(16);
	u16(3);
	u16(2);
	u32(g_sample_rate);
	u32(g_sample_rate * 2 * sizeof(float));
	u16(2 * sizeof(float));
	u16(32);
	f.write("data", 4);
	u32(data_bytes);
}
bool offline_stop() {
	if (!g_offline_file.is_open()) return false;
	g_offline_file.seekp(0);
	write_wav_header(g_offline_file, g_offline_frames_written);
	g_offline_file.close();
	return true;
}
static void start_offline(int sample_rate) {
	g_offline = true;
	g_sample_rate = sample_rate > 0 ? sample_rate : 48000;
	g_output_device = 0;
	g_offline_clock_start = get_host().now_micros();
	get_host().set_clock(g_offline_clock_start);
}
bool offline_begin(int sample_rate) {
	if (g_offline) return true;
	if (g_initialized || g_shut_down) return false;
	start_offline(sample_rate);
	return init();
}
bool offline_record(const std::string& path) {
	if (!g_offline) return false;
	offline_stop();
	g_offline_file.open(path, std::ios::binary | std::ios::trunc);
	if (!g_offline_file) return false;
	g_offline_frames_written = 0;
	write_wav_header(g_offline_file, 0);
	return true;
}
bool offline_render(double ms) {
	if (!g_offline || !init() || ms < 0) return false;
	g_offline_owed += ms * g_sample_rate / 1000.0;
	float buf[block_frames * 2];
	while (g_offline_owed >= block_frames) {
		feed_streams_now();
		render_block(buf);
		g_offline_owed -= block_frames;
		get_host().set_clock(g_offline_clock_start + int64_t(g_frame_clock * 1000000 / g_sample_rate));
		if (g_offline_file.is_open()) {
			g_offline_file.write(reinterpret_cast<const char*>(buf), sizeof(buf));
			g_offline_frames_written += block_frames;
		}
	}
	collect_garbage();
	return true;
}

// The device.
static bool open_output(unsigned int index) {
	// Destroying the stream waits for its callback to finish. No lock is held here, so that can't deadlock.
	if (g_output_stream) {
		SDL_DestroyAudioStream(g_output_stream);
		g_output_stream = nullptr;
	}
	if (index == 0) return true;
	SDL_AudioDeviceID dev = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
	if (index > 1) {
		int count = 0;
		SDL_AudioDeviceID* devs = SDL_GetAudioPlaybackDevices(&count);
		if (!devs) return false;
		// Unsigned compare, so a script passing -1 (0xffffffff) can't index before the array.
		if (count > 0 && index - 2 < unsigned(count)) dev = devs[index - 2];
		SDL_free(devs);
		if (dev == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) return false;
	}
	SDL_AudioSpec spec{SDL_AUDIO_F32, 2, g_sample_rate};
	g_output_stream = SDL_OpenAudioDeviceStream(dev, &spec, audio_callback, nullptr);
	if (!g_output_stream) {
		get_host().log_error(std::string("could not open the audio output device: ") + SDL_GetError());
		return false;
	}
	SDL_ResumeAudioStreamDevice(g_output_stream);
	return true;
}

// Deletes what the audio thread retired, a few times a second, while sound plays through a device.
static std::thread g_janitor;
static std::atomic<bool> g_janitor_stop{false};
static void janitor_loop() {
	while (!g_janitor_stop) {
		collect_garbage();
		g_commands.reserve_for_consumer();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

bool init() {
	if (g_initialized) return true;
	if (g_shut_down) return false;
	const char* rate = SDL_getenv("NVGT_SOUND_OFFLINE");
	if (rate && *rate && !g_offline) start_offline(SDL_atoi(rate));
	if (!(SDL_WasInit(0) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		get_host().log_error(std::string("could not start audio: ") + SDL_GetError());
		return false;
	}
	// The engine runs at the default device's own rate so nothing is resampled on the way out.
	SDL_AudioSpec spec;
	int frames;
	if (!g_offline && SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames) && spec.freq > 0) g_sample_rate = spec.freq;
	prepare_scratch();
	g_initialized = true;
	if (!g_master) g_master = new mixer(true);
	if (g_offline) return true;
	g_janitor_stop = false;
	g_janitor = std::thread(janitor_loop);
	if (!open_output(g_output_device)) {
		g_output_device = 1;
		open_output(1);
	}
	return true;
}
void shutdown() {
	release_played(true);
	g_shut_down = true;
#ifndef NVSOUND_STANDALONE
	close_all_microphones();
#endif
	if (g_output_stream) SDL_DestroyAudioStream(g_output_stream);
	g_output_stream = nullptr;
	stop_stream_feeder();
	offline_stop();
	if (g_janitor.joinable()) {
		g_janitor_stop = true;
		g_janitor.join();
	}
	collect_garbage();
	g_initialized = false;
}
bool is_available() { return init() && (g_output_stream || g_offline); }
float get_master_volume() { return g_master_volume; }
void set_master_volume(float v) { g_master_volume = std::clamp(v, -100.0f, 0.0f); }
unsigned int get_output_device() { return g_output_device; }
bool set_output_device(unsigned int index) {
	if (!init()) return false;
	if (g_offline) return true; // Opening a device would render alongside the script.
	if (!open_output(index)) {
		open_output(g_output_device);
		return false;
	}
	g_output_device = index;
	return true;
}
std::vector<std::string> get_output_device_names() {
	std::vector<std::string> names{"No sound", "Default"};
	if (!init()) return names;
	int count = 0;
	SDL_AudioDeviceID* devs = SDL_GetAudioPlaybackDevices(&count);
	for (int i = 0; devs && i < count; i++) {
		const char* name = SDL_GetAudioDeviceName(devs[i]);
		names.push_back(name ? name : "");
	}
	SDL_free(devs);
	return names;
}
unsigned int get_input_device() { return g_input_device; }
void set_input_device(unsigned int index) { g_input_device = index; }
std::vector<std::string> get_input_device_names() {
	std::vector<std::string> names;
	if (!init()) return names;
	int count = 0;
	SDL_AudioDeviceID* devs = SDL_GetAudioRecordingDevices(&count);
	for (int i = 0; devs && i < count; i++) {
		const char* name = SDL_GetAudioDeviceName(devs[i]);
		names.push_back(name ? name : "");
	}
	SDL_free(devs);
	return names;
}
bool get_global_hrtf() { return g_global_hrtf; }
void set_global_hrtf(bool enable) { g_global_hrtf = enable; }
void set_impulse_response_quality(float, int) {} // Item 3.

// The script side. Nothing below runs on the audio thread.

static node* new_node(bool is_mixer) {
	node* n = new node();
	n->is_mixer = is_mixer;
	n->st = std::make_shared<node_status>();
	return n;
}
sound_base::sound_base(node* audio_node) : n(audio_node), st(audio_node->st) {
	command c{op::add};
	c.target = n;
	send(c);
}
sound_base::~sound_base() {}
void sound_base::add_ref() { ref_count++; }
void sound_base::release() {
	if (--ref_count == 0) delete this;
}
bool sound_base::set_parent(mixer* m, bool) {
	if (parent) {
		auto& c = parent->children;
		c.erase(std::remove(c.begin(), c.end(), this), c.end());
	}
	parent = m;
	if (m) m->children.push_back(this);
	command c{op::set_parent};
	c.target = n;
	c.other = m ? m->n : nullptr;
	send(c);
	return true;
}
float sound_base::get_volume() { return st->volume_seq != volume_seq && !volume_slide_asked ? volume_asked : st->volume.load(); }
float sound_base::get_pan() { return st->pan_seq != pan_seq && !pan_slide_asked ? pan_asked : st->pan.load(); }
float sound_base::get_pitch() { return st->pitch_seq != pitch_seq && !pitch_slide_asked ? pitch_asked : st->pitch.load(); }
static void send_value(node* n, op kind, float value, uint64_t frames, uint32_t seq) {
	command c{kind};
	c.target = n;
	c.value = value;
	c.frames = frames;
	c.seq = seq;
	send(c);
}
bool sound_base::set_volume(float v) {
	volume_asked = std::clamp(v, -100.0f, 0.0f);
	volume_slide_asked = false;
	send_value(n, op::set_volume, volume_asked, 0, ++volume_seq);
	return true;
}
bool sound_base::slide_volume(float v, unsigned int ms) {
	if (v < -100 || v > 0) return false;
	volume_slide_asked = ms > 0;
	volume_asked = v;
	send_value(n, ms > 0 ? op::slide_volume : op::set_volume, v, ms_to_frames(ms), ++volume_seq);
	return true;
}
bool sound_base::set_pan(float v) {
	if (v < -100 || v > 100) return false;
	pan_asked = v;
	pan_slide_asked = false;
	send_value(n, op::set_pan, v, 0, ++pan_seq);
	return true;
}
bool sound_base::slide_pan(float v, unsigned int ms) {
	if (v < -100 || v > 100) return false;
	pan_slide_asked = ms > 0;
	pan_asked = v;
	send_value(n, ms > 0 ? op::slide_pan : op::set_pan, v, ms_to_frames(ms), ++pan_seq);
	return true;
}
void sound_base::send_resampler() {
	command c{op::set_resampler};
	c.target = n;
	c.ptr = make_resampler();
	send(c);
	has_resampler = true;
}
bool sound_base::set_pitch(float v) {
	if (v < 5 || v > 500) return false;
	if (!has_resampler && v != 100) send_resampler();
	pitch_asked = v;
	pitch_slide_asked = false;
	send_value(n, op::set_pitch, v, 0, ++pitch_seq);
	return true;
}
bool sound_base::slide_pitch(float v, unsigned int ms) {
	if (v < 5 || v > 500) return false;
	if (!has_resampler) send_resampler();
	pitch_slide_asked = ms > 0;
	pitch_asked = v;
	send_value(n, ms > 0 ? op::slide_pitch : op::set_pitch, v, ms_to_frames(ms), ++pitch_seq);
	return true;
}
// A slide asked for and not applied yet is already sliding, as far as the script is concerned.
bool sound_base::is_volume_sliding() { return st->volume_seq != volume_seq ? volume_slide_asked : st->volume_sliding && st->volume_end > g_frame_clock; }
bool sound_base::is_pan_sliding() { return st->pan_seq != pan_seq ? pan_slide_asked : st->pan_sliding && st->pan_end > g_frame_clock; }
bool sound_base::is_pitch_sliding() { return st->pitch_seq != pitch_seq ? pitch_slide_asked : st->pitch_sliding && st->pitch_end > g_frame_clock; }
bool sound_base::is_sliding() { return is_volume_sliding() || is_pan_sliding() || is_pitch_sliding(); }
bool sound_base::set_position(float lx, float ly, float lz, float sx, float sy, float sz, float rot, float pstep, float vstep) {
	command c{op::set_position};
	c.target = n;
	c.sp = spatial_params{lx, ly, lz, sx, sy, sz, rot, pstep, vstep};
	positioned = c.flag = lx != sx || ly != sy || lz != sz;
	send(c);
	return true;
}
void sound_base::set_hrtf(bool enable) { use_hrtf = enable; } // Item 3.
int sound_base::set_fx(const std::string&, int) { return -1; } // Item 3.
bool sound_base::set_impulse_response(const std::string&, float, float, const std::string&, float) { return false; } // Item 3.
bool sound_base::set_impulse_response_params(float, float, float) { return false; } // Item 3.
bool sound_base::set_send(mixer*, float, bool) { return false; } // Item 3.
bool sound_base::remove_send(mixer*) { return false; } // Item 3.
float sound_base::get_send(mixer*) { return -100; } // Item 3.
float sound_base::get_dry_volume() { return dry_asked; }
void sound_base::set_dry_volume(float v) {
	dry_asked = std::clamp(v, -100.0f, 0.0f);
	command c{op::set_dry_volume};
	c.target = n;
	c.value = dry_asked;
	send(c);
}

// mixer
mixer::mixer(bool is_master) : sound_base(new_node(true)) {
	if (is_master) {
		g_master_node = n;
		return;
	}
	init();
	set_parent(g_master, true);
}
mixer::~mixer() {
	// The audio thread moves this mixer's children to the master itself. The script side's view follows.
	std::vector<sound_base*> orphans = children;
	mixer* to = this == g_master ? nullptr : g_master;
	for (sound_base* c : orphans) {
		c->parent = to;
		if (to) to->children.push_back(c);
	}
	children.clear();
	if (parent) {
		auto& c = parent->children;
		c.erase(std::remove(c.begin(), c.end(), this), c.end());
		parent = nullptr;
	}
	command c{op::destroy};
	c.target = n;
	send(c);
}
bool mixer::is_ancestor_of(mixer* m) {
	for (; m; m = m->parent) {
		if (m == this) return true;
	}
	return false;
}
bool mixer::set_mixer(mixer* m) {
	if (!m) m = g_master;
	if (this == g_master || is_ancestor_of(m)) return false;
	return set_parent(m);
}

// File and decoded data caches, so a sound loaded many times is read and decoded once. Entries nobody is using expire after a couple of minutes.
struct cache_entry {
	std::shared_ptr<const pcm_data> pcm;
	std::shared_ptr<const std::string> bytes;
	std::chrono::steady_clock::time_point last_used;
};
static std::mutex g_cache_mutex;
static std::unordered_map<std::string, cache_entry> g_cache;
static void prune_cache() {
	auto now = std::chrono::steady_clock::now();
	for (auto it = g_cache.begin(); it != g_cache.end();) {
		bool in_use = (it->second.pcm && it->second.pcm.use_count() > 1) || (it->second.bytes && it->second.bytes.use_count() > 1);
		if (!in_use && now - it->second.last_used > std::chrono::seconds(120)) it = g_cache.erase(it);
		else ++it;
	}
}
static std::unique_ptr<std::istream> open_file(const std::string& key) {
	std::unique_ptr<std::istream> s;
	try {
		s = get_host().open(key);
	} catch (std::exception&) {}
	get_host().done(key);
	return s;
}
std::unique_ptr<std::istream> open_sound_file(const std::string& filename, const pack_interface* pack) {
	if (!init()) return nullptr;
	std::string key = get_host().cache_key(filename, pack);
	return key.empty() ? nullptr : open_file(key);
}
static std::shared_ptr<const std::string> read_all(std::istream& in) {
	auto data = std::make_shared<std::string>();
	char buf[65536];
	while (in.read(buf, sizeof(buf)) || in.gcount() > 0) data->append(buf, size_t(in.gcount()));
	return data;
}
static bool is_url(const std::string& name) {
	for (const char* scheme : {"http://", "https://", "ftp://"}) {
		size_t len = strlen(scheme);
		if (name.size() > len && SDL_strncasecmp(name.c_str(), scheme, len) == 0) return true;
	}
	return false;
}
static std::shared_ptr<const pcm_data> decode_all(decoder& dec) {
	auto pcm = std::make_shared<pcm_data>();
	pcm->channels = dec.channels;
	uint64_t len = dec.length();
	std::vector<float> samples;
	samples.reserve(len * dec.channels);
	std::vector<float> chunk(16384 * dec.channels);
	uint64_t frames = 0;
	while (uint64_t got = dec.read(chunk.data(), 16384)) {
		samples.insert(samples.end(), chunk.begin(), chunk.begin() + got * dec.channels);
		frames += got;
	}
	if (dec.sample_rate != unsigned(g_sample_rate)) {
		pcm->samples = resample_interleaved(samples.data(), frames, dec.channels, dec.sample_rate, g_sample_rate, pcm->frames);
	} else {
		pcm->samples = std::move(samples);
		pcm->frames = frames;
	}
	pcm->sample_rate = g_sample_rate;
	return pcm;
}

// sound
sound::sound() : sound_base(new_node(false)) { init(); }
sound::~sound() {
	if (bytes_pushing) abort_push_bytes(bytes_pushing.get());
	if (parent) {
		auto& c = parent->children;
		c.erase(std::remove(c.begin(), c.end(), this), c.end());
		parent = nullptr;
	}
	command c{op::destroy};
	c.target = n;
	send(c);
	// The audio thread's reference to the source goes with the node, when it's collected. This one goes now.
}
bool sound::set_mixer(mixer* m) { return set_parent(m ? m : g_master); }
bool sound::playing_now() { return st->transport_seq != transport_seq ? playing_asked : st->playing.load(); }
bool sound::ended_now() { return st->transport_seq != transport_seq ? ended_asked : st->ended.load(); }
void sound::attach_source(std::shared_ptr<source> s, const std::string& filename) {
	src = s;
	command c{op::attach_source};
	c.target = n;
	c.ptr = new std::shared_ptr<source>(s);
	c.seq = ++transport_seq;
	playing_asked = ended_asked = false;
	send(c);
	// As before, a sound at another rate or already pitched gets a fresh resampler, and one that doesn't need it loses its own.
	if (s->sample_rate() != unsigned(g_sample_rate) || get_pitch() != 100) send_resampler();
	else if (has_resampler) {
		command r{op::set_resampler};
		r.target = n;
		send(r);
		has_resampler = false;
	}
	loaded_filename = filename;
	if (!parent) set_parent(get_default_mixer());
}
// Plays an encoded file from memory: decoded completely if it is short, otherwise decoded as it plays.
bool sound::load_bytes(std::shared_ptr<const std::string> bytes, const std::string& cache_key, const std::string& name, bool cache) {
	std::unique_ptr<decoder> dec = open_decoder(open_memory_stream(bytes));
	if (!dec) return false;
	std::shared_ptr<const pcm_data> pcm;
	uint64_t len = dec->length();
	if (len > 0 && len <= g_preload_max_seconds * dec->sample_rate) {
		pcm = decode_all(*dec);
		bytes.reset();
		attach_source(make_pcm_source(pcm), name);
	} else attach_source(make_stream_source(std::move(dec)), name);
	if (cache) {
		std::lock_guard<std::mutex> l(g_cache_mutex);
		prune_cache();
		g_cache[cache_key] = cache_entry{pcm, bytes, std::chrono::steady_clock::now()};
	}
	return true;
}
bool sound::load_encoded(std::shared_ptr<const std::string> bytes, const std::string& cache_key, const std::string& name, bool cache) {
	if (!init()) return false;
	close();
	return load_bytes(std::move(bytes), cache_key, name, cache);
}
bool sound::attach_cached(const std::string& cache_key, const std::string& name) {
	std::shared_ptr<const pcm_data> pcm;
	std::shared_ptr<const std::string> bytes;
	{
		std::lock_guard<std::mutex> l(g_cache_mutex);
		auto it = g_cache.find(cache_key);
		if (it == g_cache.end()) return false;
		it->second.last_used = std::chrono::steady_clock::now();
		pcm = it->second.pcm;
		bytes = it->second.bytes;
	}
	if (pcm) {
		attach_source(make_pcm_source(pcm), name);
		return true;
	}
	std::unique_ptr<decoder> dec = bytes ? open_decoder(open_memory_stream(bytes)) : nullptr;
	if (!dec) return false;
	attach_source(make_stream_source(std::move(dec)), name);
	return true;
}
bool sound::load(const std::string& filename, const pack_interface* pack, bool allow_preloads) {
	if (!init()) return false;
	if (is_url(filename)) return load_url(filename); // As the legacy engine did.
	close();
	std::string key = get_host().cache_key(filename, pack);
	if (key.empty()) return false;
	if (allow_preloads && attach_cached(key, filename)) {
		get_host().done(key);
		return true;
	}
	std::unique_ptr<std::istream> in = open_file(key);
	if (!in) {
		get_host().log_error("could not open " + filename);
		return false;
	}
	if (load_bytes(read_all(*in), key, filename, allow_preloads)) return true;
	get_host().log_error("could not load " + filename + ": it isn't audio in a supported format, or it is damaged");
	return false;
}
bool sound::stream(const std::string& filename, const pack_interface* pack) {
	if (!init()) return false;
	close();
	std::string key = get_host().cache_key(filename, pack);
	if (key.empty()) return false;
	std::unique_ptr<std::istream> in = open_file(key);
	if (!in) {
		get_host().log_error("could not open " + filename);
		return false;
	}
	std::unique_ptr<decoder> dec = open_decoder(std::move(in));
	if (!dec) {
		get_host().log_error("could not stream " + filename + ": it isn't audio in a supported format, or it is damaged");
		return false;
	}
	attach_source(make_stream_source(std::move(dec)), filename);
	return true;
}
bool sound::load_url(const std::string& url) {
	if (!init()) return false;
	close();
	std::function<void()> abort;
	std::unique_ptr<std::istream> in = get_host().open_url(url, abort);
	if (!in) return false;
	std::unique_ptr<decoder> dec = open_decoder(std::move(in));
	if (!dec) return false;
	unsigned int rate = dec->sample_rate;
	attach_source(make_stream_source(std::move(dec), true, abort, rate / 10), url);
	return true;
}
bool sound::load_memory(std::string& data, unsigned int size, const std::string& preload_filename, bool legacy_encrypt) {
	if (!init()) return false;
	close();
	std::string key = "\x1ememory\x1e" + preload_filename, name = preload_filename.empty() ? "script_stream" : preload_filename;
	if (!preload_filename.empty() && attach_cached(key, name)) return true;
	size_t count = std::min<size_t>(size, data.size());
	auto bytes = std::make_shared<std::string>(data, 0, count);
	if (legacy_encrypt) get_host().legacy_decrypt(*bytes);
	return load_bytes(bytes, key, name, !preload_filename.empty());
}
// Opens the decoder for a pushed file once enough of it has arrived to recognise. Until then pushes just accumulate.
bool sound::start_pushed_decoder() {
	std::unique_ptr<decoder> dec = open_decoder(open_push_stream(bytes_pushing));
	if (!dec) {
		if (push_bytes_size(bytes_pushing.get()) > 4 * 1024 * 1024) bytes_pushing.reset(); // Four megabytes without a recognisable header isn't audio.
		return false;
	}
	set_push_blocking(bytes_pushing.get());
	std::shared_ptr<push_bytes> p = bytes_pushing;
	attach_source(make_stream_source(std::move(dec), true, [p] { abort_push_bytes(p.get()); }, 0), "");
	return true;
}
bool sound::push_memory(const std::string& data, bool end_stream, int pcm_rate, int pcm_channels) {
	if (!init()) return false;
	if (!end_stream && data.empty()) return false;
	if (!loaded_filename.empty()) {
		if (data.empty()) return false;
		close(); // Pushing into a sound that holds a loaded file starts a new push stream, as under BASS.
	}
	if (pcm_pushing) {
		// Raw pushes are 32 bit float samples, which is what BASS_SAMPLE_FLOAT push streams took.
		pcm_push(pcm_pushing.get(), (const float*)data.data(), data.size() / sizeof(float));
		if (end_stream) pcm_push_end(pcm_pushing.get());
		return true;
	}
	if (bytes_pushing) {
		push_bytes_append(bytes_pushing.get(), data.data(), data.size(), end_stream);
		if (!src) start_pushed_decoder();
		return true;
	}
	if (pcm_rate > 0) {
		if (pcm_channels < 1 || pcm_channels > max_source_channels) return false;
		pcm_pushing = make_pcm_push_source(pcm_rate, pcm_channels);
		attach_source(as_source(pcm_pushing), "");
		return push_memory(data, end_stream, pcm_rate, pcm_channels);
	}
	if (data.size() < 768) return false; // As under BASS: a pushed file must begin with enough of itself to be recognised.
	bytes_pushing = make_push_bytes();
	push_bytes_append(bytes_pushing.get(), data.data(), data.size(), end_stream);
	start_pushed_decoder();
	return true;
}
bool sound::push_float_samples(const float* samples, size_t count, bool end_stream, int pcm_rate, int pcm_channels) {
	if (!init()) return false;
	if (!end_stream && count == 0) return false;
	if (!loaded_filename.empty()) {
		if (count == 0) return false;
		close();
	}
	if (bytes_pushing) return false; // An encoded push stream can't take raw samples.
	if (!pcm_pushing) {
		if (pcm_rate <= 0 || pcm_channels < 1 || pcm_channels > max_source_channels) return false;
		pcm_pushing = make_pcm_push_source(pcm_rate, pcm_channels);
		attach_source(as_source(pcm_pushing), "");
	}
	if (count) pcm_push(pcm_pushing.get(), samples, count);
	if (end_stream) pcm_push_end(pcm_pushing.get());
	return true;
}
bool sound::load_pcm(const float* samples, uint64_t frames, unsigned int sample_rate, unsigned int channels, const std::string& name) {
	if (!init() || !samples || !frames || !sample_rate || channels < 1 || channels > max_source_channels) return false;
	close();
	auto pcm = std::make_shared<pcm_data>();
	pcm->channels = channels;
	pcm->sample_rate = g_sample_rate;
	if (sample_rate != unsigned(g_sample_rate)) pcm->samples = resample_interleaved(samples, frames, channels, sample_rate, g_sample_rate, pcm->frames);
	else {
		pcm->samples.assign(samples, samples + frames * channels);
		pcm->frames = frames;
	}
	attach_source(make_pcm_source(pcm), name);
	return true;
}
bool sound::load_source(std::shared_ptr<source> s, const std::string& name) {
	if (!init() || !s) return false;
	close();
	attach_source(s, name);
	return true;
}
bool sound::close() {
	if (!src && !bytes_pushing && !pcm_pushing) return false;
	if (bytes_pushing) abort_push_bytes(bytes_pushing.get());
	command c{op::detach_source};
	c.target = n;
	c.seq = ++transport_seq;
	playing_asked = ended_asked = false;
	send(c);
	src.reset();
	bytes_pushing.reset();
	pcm_pushing.reset();
	forced_length = 0;
	loaded_filename.clear();
	return true;
}
static void send_transport(node* n, op kind, uint32_t seq) {
	command c{kind};
	c.target = n;
	c.seq = seq;
	send(c);
}
bool sound::play(bool reset_loop_state) {
	if (!src) return false;
	if (!loaded_filename.empty() && playing_now()) return false;
	// The source was built for its position to be moved from here, as it always was.
	if (ended_now()) src->seek(0);
	if (reset_loop_state) src->set_loop(false);
	if (!parent) set_parent(get_default_mixer());
	playing_asked = true;
	ended_asked = false;
	send_transport(n, op::play, ++transport_seq);
	return true;
}
bool sound::play_wait() {
	if (!play()) return false;
	while (playing_now() && src) get_host().wait(5);
	return true;
}
bool sound::play_looped() {
	if (!src || (!loaded_filename.empty() && playing_now())) return false;
	src->set_loop(true); // Before play, so the sound can't reach its end in between and stop.
	return play(false);
}
bool sound::pause() {
	if (!src || !playing_now()) return false;
	playing_asked = false;
	ended_asked = ended_now();
	send_transport(n, op::pause, ++transport_seq);
	return true;
}
bool sound::stop() {
	if (!src) return false;
	playing_asked = false;
	ended_asked = false;
	send_transport(n, op::stop, ++transport_seq);
	src->seek(0);
	return true;
}
bool sound::seek(float ms) {
	if (!src || ms < 0) return false;
	if (!src->seek(uint64_t(double(ms) / 1000.0 * src->sample_rate()))) return false;
	if (ended_now()) {
		// Seeking clears ended, as before. Sent as a pause or a stop would be, so the order with them holds.
		ended_asked = false;
		playing_asked = playing_now();
		send_transport(n, playing_asked ? op::play : op::stop, ++transport_seq);
	}
	return true;
}
bool sound::is_active() { return src && parent; }
bool sound::is_playing() { return src && parent && playing_now(); }
bool sound::is_paused() { return src && parent && !playing_now() && !ended_now(); }
float sound::get_length() {
	if (forced_length > 0) return forced_length;
	if (!src) return -1;
	uint64_t len = src->length();
	return len ? float(double(len) * 1000.0 / src->sample_rate()) : -1;
}
float sound::get_position() {
	if (!src) return -1;
	return float(double(src->position()) * 1000.0 / src->sample_rate());
}

// sound_play keeps each sound it starts alive until it finishes, so a script can fire one and forget it. Finished ones are let go the next time sound_play is called, and at shutdown.
static std::mutex g_played_mutex;
static std::vector<sound*> g_played;
static void release_played(bool all) {
	std::vector<sound*> done;
	{
		std::lock_guard<std::mutex> l(g_played_mutex);
		auto keep = std::partition(g_played.begin(), g_played.end(), [all](sound* s) { return !all && s->is_playing(); });
		done.assign(keep, g_played.end());
		g_played.erase(keep, g_played.end());
	}
	for (sound* s : done) s->release();
}
sound* sound_play(const std::string& path, float x, float y, float z, bool positioned, float volume, float pan, float pitch, mixer* mix, const pack_interface* pack, bool autoplay) {
	release_played(false);
	sound* s = new sound();
	if (!s->load(path, pack)) {
		s->release();
		return nullptr;
	}
	if (mix) s->set_mixer(mix);
	if (positioned) s->set_position(0, 0, 0, x, y, z, 0, 1, 1);
	s->set_volume(volume);
	s->set_pan(pan);
	s->set_pitch(pitch);
	if (autoplay) s->play();
	s->add_ref();
	{
		std::lock_guard<std::mutex> l(g_played_mutex);
		g_played.push_back(s);
	}
	return s;
}
sound* make_sound() { return new sound(); }
mixer* make_mixer() { return new mixer(); }

} // namespace nvsound
