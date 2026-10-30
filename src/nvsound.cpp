/* nvsound.cpp - NVGT's sound engine: SDL3 device output, the mixer tree, sounds and their script bindings.
 * Volume, pan and pitch follow the legacy BASS based engine exactly: volume is in decibels from -100 to 0, pan attenuates the opposite side by that many decibels from -100 to 100, pitch is a percentage from 5 to 500.
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

#include "nvsound.h"
#include "nvsound_convolver.h"
#include "nvsound_effects.h"
#include "nvsound_phonon.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <angelscript.h>
#include <reactphysics3d/mathematics/Vector3.h>
#include <scriptarray.h>
#include <SDL3/SDL.h>
#include <Poco/Format.h>
#include <Poco/String.h>
#include <Poco/Timestamp.h>
#include <Poco/URIStreamOpener.h>
#include <resample.h>
#include "datastreams.h" // prebuffer_istream
#include "legacy_pack.h"
#include "legacy_pack_config.h" // sound_data_char_decrypt
#include "logging.h"
#include "nvgt.h" // g_ScriptEngine
#include "sound_service.h"
#include "UI.h" // wait

namespace nvsound {

static std::recursive_mutex g_graph_mutex;
static SDL_AudioStream* g_output_stream = nullptr;
static int g_sample_rate = 48000;
static std::atomic<uint64_t> g_frame_clock{0};
static mixer* g_master = nullptr;
static mixer* g_default_mixer = nullptr; // A script handle (sound_default_mixer); AngelScript manages its reference.
static std::atomic<float> g_master_volume{0};
static unsigned int g_output_device = 1; // 0 is no sound, 1 follows the system default, and real devices start at 2, as they did under BASS.
static unsigned int g_input_device = 0xffffffff; // Means the system default.
static bool g_initialized = false;
static bool g_shut_down = false; // Once the engine has shut down at exit, nothing may start it again, since SDL is about to be torn down.
static bool g_global_hrtf = true;
static void release_played(bool all);
// Loading fully decodes files up to this long. Longer ones keep their compressed bytes in memory and decode as they play.
static const double g_preload_max_seconds = 30.0;

std::recursive_mutex& get_graph_mutex() { return g_graph_mutex; }
int get_sample_rate() { return g_sample_rate; }
uint64_t get_frame_clock() { return g_frame_clock; }
mixer* get_master() { return g_master; }
mixer* get_default_mixer() { return g_default_mixer ? g_default_mixer : g_master; }

static uint64_t ms_to_frames(unsigned int ms) { return uint64_t(ms) * g_sample_rate / 1000; }
static float db_to_gain(float db) { return db <= -100 ? 0.0f : powf(10.0f, db / 20.0f); }

// Scratch buffers for the audio thread, one set per depth of the mixer tree, since a mixer's buffer must survive while its children render.
static float* scratch(int which, int depth, size_t floats) {
	thread_local std::vector<std::vector<float>> pools[2];
	auto& pool = pools[which];
	if (pool.size() <= size_t(depth)) pool.resize(depth + 1);
	if (pool[depth].size() < floats) pool[depth].resize(floats);
	return pool[depth].data();
}

static WDL_Resampler* make_resampler() {
	WDL_Resampler* r = new WDL_Resampler();
	r->SetMode(false, 0, true, 16, 32);
	return r;
}

static void SDLCALL audio_callback(void*, SDL_AudioStream* stream, int additional, int) {
	float buf[block_frames * 2];
	for (int needed = additional / int(sizeof(float) * 2); needed > 0; needed -= block_frames) {
		memset(buf, 0, sizeof(buf));
		{
			std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
			if (g_master) g_master->render(buf, block_frames, 0);
		}
		g_frame_clock += block_frames;
		// Master volume was linear under BASS rather than in decibels, so it stays that way.
		float gain = std::clamp(1.0f + g_master_volume / 100.0f, 0.0f, 1.0f);
		for (float& s : buf) s = std::clamp(s * gain, -1.0f, 1.0f);
		SDL_PutAudioStreamData(stream, buf, sizeof(buf));
	}
}

static bool open_output(unsigned int index) {
	// Never hold the graph mutex here: destroying the stream waits for the audio callback, which takes that mutex.
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
		if (int(index - 2) < count) dev = devs[index - 2];
		SDL_free(devs);
		if (dev == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) return false;
	}
	SDL_AudioSpec spec{SDL_AUDIO_F32, 2, g_sample_rate};
	g_output_stream = SDL_OpenAudioDeviceStream(dev, &spec, audio_callback, nullptr);
	if (!g_output_stream) {
		NVGT_ERROR("nvgt.sound", Poco::format("could not open the audio output device: %s", std::string(SDL_GetError())));
		return false;
	}
	SDL_ResumeAudioStreamDevice(g_output_stream);
	return true;
}

bool init() {
	if (g_initialized) return true;
	if (g_shut_down) return false;
	if (!(SDL_WasInit(0) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		NVGT_ERROR("nvgt.sound", Poco::format("could not start audio: %s", std::string(SDL_GetError())));
		return false;
	}
	// The engine runs at the default device's own rate so nothing is resampled on the way out. Switching to a device with a different rate later lets SDL convert.
	SDL_AudioSpec spec;
	int frames;
	if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &frames) && spec.freq > 0) g_sample_rate = spec.freq;
	if (!g_master) g_master = new mixer(true);
	g_initialized = true;
	if (!open_output(g_output_device)) {
		g_output_device = 1;
		open_output(1);
	}
	return true;
}
void shutdown() {
	release_played(true);
	g_shut_down = true;
	close_all_microphones();
	if (g_output_stream) SDL_DestroyAudioStream(g_output_stream);
	g_output_stream = nullptr;
	stop_stream_feeder();
	g_initialized = false;
}
bool is_available() { return init() && g_output_stream; }

float get_master_volume() { return g_master_volume; }
void set_master_volume(float v) { g_master_volume = std::clamp(v, -100.0f, 0.0f); }
unsigned int get_output_device() { return g_output_device; }
bool set_output_device(unsigned int index) {
	if (!init()) return false;
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
		const char* n = SDL_GetAudioDeviceName(devs[i]);
		names.push_back(n ? n : "");
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
		const char* n = SDL_GetAudioDeviceName(devs[i]);
		names.push_back(n ? n : "");
	}
	SDL_free(devs);
	return names;
}

// sound_base
sound_base::~sound_base() {
	clear_sends();
	release_phonon_state(phonon);
}
void sound_base::add_ref() { ref_count++; }
void sound_base::release() {
	if (--ref_count == 0) delete this;
}
bool sound_base::set_parent(mixer* m, bool) {
	std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
	if (parent) {
		auto& c = parent->children;
		c.erase(std::remove(c.begin(), c.end(), this), c.end());
	}
	parent = m;
	if (m) m->children.push_back(this);
	return true;
}
float sound_base::get_volume() {
	std::lock_guard<spinlock> l(params_lock);
	return volume.value;
}
bool sound_base::set_volume(float v) {
	std::lock_guard<spinlock> l(params_lock);
	volume.set(std::clamp(v, -100.0f, 0.0f));
	return true;
}
bool sound_base::slide_volume(float v, unsigned int ms) {
	if (v < -100 || v > 0) return false;
	std::lock_guard<spinlock> l(params_lock);
	volume.slide(v, g_frame_clock, ms_to_frames(ms));
	return true;
}
float sound_base::get_pan() {
	std::lock_guard<spinlock> l(params_lock);
	return pan.value;
}
bool sound_base::set_pan(float v) {
	if (v < -100 || v > 100) return false;
	std::lock_guard<spinlock> l(params_lock);
	pan.set(v);
	return true;
}
bool sound_base::slide_pan(float v, unsigned int ms) {
	if (v < -100 || v > 100) return false;
	std::lock_guard<spinlock> l(params_lock);
	pan.slide(v, g_frame_clock, ms_to_frames(ms));
	return true;
}
float sound_base::get_pitch() {
	std::lock_guard<spinlock> l(params_lock);
	return pitch.value;
}
bool sound_base::set_pitch(float v) {
	if (v < 5 || v > 500) return false;
	if (!resampler && v != 100) {
		// Built here rather than on the audio thread, which shouldn't allocate.
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		if (!resampler) resampler.reset(make_resampler());
	}
	std::lock_guard<spinlock> l(params_lock);
	pitch.set(v);
	return true;
}
bool sound_base::slide_pitch(float v, unsigned int ms) {
	if (v < 5 || v > 500) return false;
	if (!resampler) {
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		if (!resampler) resampler.reset(make_resampler());
	}
	std::lock_guard<spinlock> l(params_lock);
	pitch.slide(v, g_frame_clock, ms_to_frames(ms));
	return true;
}
bool sound_base::is_volume_sliding() {
	std::lock_guard<spinlock> l(params_lock);
	return volume.sliding && volume.end > g_frame_clock;
}
bool sound_base::is_pan_sliding() {
	std::lock_guard<spinlock> l(params_lock);
	return pan.sliding && pan.end > g_frame_clock;
}
bool sound_base::is_pitch_sliding() {
	std::lock_guard<spinlock> l(params_lock);
	return pitch.sliding && pitch.end > g_frame_clock;
}
bool sound_base::is_sliding() { return is_volume_sliding() || is_pan_sliding() || is_pitch_sliding(); }
bool sound_base::set_position(float lx, float ly, float lz, float sx, float sy, float sz, float rot, float pstep, float vstep) {
	{
		std::lock_guard<spinlock> l(params_lock);
		listener_x = lx;
		listener_y = ly;
		listener_z = lz;
		x = sx;
		y = sy;
		z = sz;
		rotation = rot;
		pan_step = pstep;
		volume_step = vstep;
		positioned = lx != sx || ly != sy || lz != sz;
	}
	ensure_phonon(); // Outside params_lock, since it can wait on the graph mutex, which the audio thread holds while it takes params_lock.
	phonon_set_position(phonon, sx, sy, sz);
	return true;
}
bool sound_base::attach_environment(sound_environment* env) {
	if (!phonon) {
		phonon_state* s = create_phonon_state();
		if (!s) return false;
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		if (phonon) release_phonon_state(s);
		else phonon = s;
	}
	float px, py, pz;
	{
		std::lock_guard<spinlock> l(params_lock);
		px = x;
		py = y;
		pz = z;
	}
	return phonon_attach_environment(phonon, env, px, py, pz);
}
void sound_base::set_hrtf(bool enable) {
	use_hrtf = enable;
	ensure_phonon();
}
// Builds this node's Steam Audio effects the first time a positioned sound needs HRTF. Done here on the script's thread because the audio thread shouldn't allocate.
void sound_base::ensure_phonon() {
	if (phonon || !positioned || !use_hrtf || !g_global_hrtf) return;
	phonon_state* s = create_phonon_state();
	if (!s) return;
	std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
	if (phonon) release_phonon_state(s);
	else phonon = s;
}
spatial_params sound_base::snapshot_position(bool& is_positioned) {
	is_positioned = positioned;
	return spatial_params{listener_x, listener_y, listener_z, x, y, z, rotation, pan_step, volume_step};
}
// With HRTF, Steam Audio places the sound and only distance changes the volume. Without it, the legacy engine's positioning: lose a decibel per unit of distance scaled by volume_step, pan by the x offset scaled by pan_step.
void sound_base::spatialize(float* frames, int count, const spatial_params& p, bool is_positioned, float& vol_db, float& pan_value) {
	vol_db = 0;
	pan_value = 0;
	if (phonon && use_hrtf && g_global_hrtf) {
		phonon_spatialize(phonon, frames, count, p, vol_db);
		return;
	}
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
void sound_base::apply_gain(const float* in, float* out, int frames, float vol_db, float pan_value, float position_pan) {
	float gl, gr;
	stereo_gains(vol_db, pan_value, position_pan, gl, gr);
	if (last_gain_left < 0) {
		last_gain_left = gl;
		last_gain_right = gr;
	}
	float sl = last_gain_left, sr = last_gain_right, dl = (gl - sl) / frames, dr = (gr - sr) / frames;
	for (int f = 0; f < frames; f++) {
		sl += dl;
		sr += dr;
		out[f * 2] += in[f * 2] * sl;
		out[f * 2 + 1] += in[f * 2 + 1] * sr;
	}
	last_gain_left = gl;
	last_gain_right = gr;
}

// Sends
// Whether audio leaving from reaches m's input, up the mixer tree or through sends. Must be called with the graph mutex held.
bool reaches(sound_base* from, mixer* m, int depth) {
	if (depth > 64) return true; // Deeper than any real graph; refusing is the safe answer.
	for (sound_base* n = from; n; n = n->parent) {
		if (n == m) return true;
		for (auto& s : n->sends) {
			if (reaches(s->target, m, depth + 1)) return true;
		}
	}
	return false;
}
// Adds a send to target, or changes the volume of the one already there.
bool sound_base::set_send(mixer* target, float volume, bool after_distance) {
	if (!target || target == this) return false;
	volume = std::clamp(volume, -100.0f, 0.0f);
	std::lock_guard<std::mutex> sl(sends_mutex);
	for (auto& s : sends) {
		if (s->target != target) continue;
		s->volume = volume;
		if (s->after_distance != after_distance) {
			std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
			s->after_distance = after_distance;
		}
		return true;
	}
	std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
	// Refuse anything that would feed back into this node, which with a send's delay would ring or run away rather than recurse.
	if (reaches(target, dynamic_cast<mixer*>(this), 0)) return false;
	for (auto& b : target->send_in) {
		if (b.buf.empty()) b.buf.assign(size_t(max_render_frames) * 2, 0.0f); // Allocated here because the audio thread shouldn't.
	}
	auto s = std::make_unique<send_slot>();
	s->target = target;
	s->volume = volume;
	s->after_distance = after_distance;
	target->add_ref();
	sends.push_back(std::move(s));
	return true;
}
// Removes the send to target, or every send when target is null.
bool sound_base::remove_send(mixer* target) {
	std::vector<std::unique_ptr<send_slot>> removed;
	{
		std::lock_guard<std::mutex> sl(sends_mutex);
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		auto keep = std::stable_partition(sends.begin(), sends.end(), [target](const std::unique_ptr<send_slot>& s) { return target && s->target != target; });
		removed.assign(std::make_move_iterator(keep), std::make_move_iterator(sends.end()));
		sends.erase(keep, sends.end());
	}
	for (auto& s : removed) s->target->release(); // Outside the locks, since releasing the last reference destroys the mixer.
	return !removed.empty();
}
float sound_base::get_send(mixer* target) {
	std::lock_guard<std::mutex> sl(sends_mutex);
	for (auto& s : sends) {
		if (s->target == target) return s->volume;
	}
	return -100;
}
void sound_base::clear_sends() { remove_send(nullptr); }
// Called by render after spatializing, with the same volume and pans the node's own output gets.
void sound_base::render_sends(const float* in, int frames, float vol_db, float position_vol_db, float pan_value, float position_pan) {
	if (sends.empty()) return;
	uint64_t block = g_frame_clock / block_frames;
	for (auto& s : sends) {
		float gl, gr;
		stereo_gains(vol_db + (s->after_distance ? position_vol_db : 0) + s->volume.load(), pan_value, position_pan, gl, gr);
		if (s->last_left < 0) {
			s->last_left = gl;
			s->last_right = gr;
		}
		s->target->add_send_input(in, frames, block, s->last_left, s->last_right, gl, gr);
		s->last_left = gl;
		s->last_right = gr;
	}
}

// mixer
mixer::mixer(bool is_master) {
	if (!is_master) {
		init();
		set_parent(g_master, true);
	}
}
mixer::~mixer() {
	std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
	// Whatever was playing through this mixer carries on through the master, as it did under BASS.
	std::vector<sound_base*> orphans = children;
	for (sound_base* c : orphans) c->set_parent(this == g_master ? nullptr : g_master, true);
	set_parent(nullptr, true);
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
	std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
	// Moving under m must not close a loop through sends either: nothing this mixer's output reaches may send back into it from below.
	sound_base* old_parent = parent;
	set_parent(m);
	bool loops = false;
	for (sound_base* n = m; n && !loops; n = n->parent) {
		for (auto& s : n->sends) {
			if (reaches(s->target, this, 0)) {
				loops = true;
				break;
			}
		}
	}
	if (loops) set_parent(static_cast<mixer*>(old_parent));
	return !loops;
}
// Audio thread. Adds a sender's audio, ramped between its gains, to this block's buffer, clearing the buffer first if it still holds an older block.
void mixer::add_send_input(const float* in, int frames, uint64_t block, float from_left, float from_right, float to_left, float to_right) {
	send_input& b = send_in[block & 1];
	if (b.buf.empty()) return;
	if (b.block != block) {
		std::fill(b.buf.begin(), b.buf.end(), 0.0f);
		b.block = block;
		b.frames = 0;
	}
	// A sender inside a pitched mixer renders more or fewer frames than a block. Its send isn't resampled by that mixer, so only what fits is kept.
	int n = std::min(frames, max_render_frames);
	float sl = from_left, sr = from_right, dl = (to_left - from_left) / n, dr = (to_right - from_right) / n;
	for (int f = 0; f < n; f++) {
		sl += dl;
		sr += dr;
		b.buf[f * 2] += in[f * 2] * sl;
		b.buf[f * 2 + 1] += in[f * 2 + 1] * sr;
	}
	b.frames = std::max(b.frames, n);
}
// Audio thread. Mixes in what was sent here during the previous block.
void mixer::mix_send_input(float* acc, int frames, uint64_t block) {
	const send_input& b = send_in[(block + 1) & 1];
	if (b.buf.empty() || b.block + 1 != block || !b.frames) return;
	int n = std::min(frames, b.frames);
	for (int i = 0; i < n * 2; i++) acc[i] += b.buf[i];
}
void mixer::render(float* out, int frames, int depth) {
	if (depth > 64) return; // A cycle can't be built through set_mixer, but never recurse forever.
	uint64_t now = g_frame_clock;
	float vol, pn, pt, pos_vol, pos_pan;
	bool is_positioned;
	spatial_params sp;
	{
		std::lock_guard<spinlock> l(params_lock);
		vol = volume.at(now);
		pn = pan.at(now);
		pt = pitch.at(now);
		sp = snapshot_position(is_positioned);
	}
	float* acc = scratch(0, depth, size_t(frames) * 2);
	if (!resampler) {
		memset(acc, 0, frames * 2 * sizeof(float));
		for (sound_base* c : children) c->render(acc, frames, depth + 1);
	} else {
		// A pitched mixer speeds up or slows down everything in it, so it asks its children for more or fewer frames and resamples the mix.
		resampler->SetRates(double(g_sample_rate) * pt / 100.0, g_sample_rate);
		WDL_ResampleSample* in;
		int need = std::min(resampler->ResamplePrepare(frames, 2, &in), max_render_frames);
		memset(in, 0, need * 2 * sizeof(float));
		for (sound_base* c : children) c->render(in, need, depth + 1);
		int got = resampler->ResampleOut(acc, need, frames, 2);
		if (got < frames) memset(acc + got * 2, 0, (frames - got) * 2 * sizeof(float));
	}
	mix_send_input(acc, frames, now / block_frames); // Before the effects, so a reverb on this mixer works on what's sent to it.
	process_fx(acc, frames);
	process_ir(acc, frames, depth);
	spatialize(acc, frames, sp, is_positioned, pos_vol, pos_pan);
	render_sends(acc, frames, vol, pos_vol, pn, pos_pan);
	apply_gain(acc, out, frames, vol + pos_vol + dry_volume.load(), pn, pos_pan);
}

// File and decoded data caches, so a sound loaded many times is read and decoded once. Entries nobody is using expire after a couple of minutes.
struct cache_entry {
	std::shared_ptr<const pcm_data> pcm;
	std::shared_ptr<const std::string> bytes;
	Poco::Timestamp last_used;
};
static std::mutex g_cache_mutex;
static std::unordered_map<std::string, cache_entry> g_cache;
static void prune_cache() {
	for (auto it = g_cache.begin(); it != g_cache.end();) {
		bool in_use = (it->second.pcm && it->second.pcm.use_count() > 1) || (it->second.bytes && it->second.bytes.use_count() > 1);
		if (!in_use && it->second.last_used.isElapsed(120 * Poco::Timestamp::resolution())) it = g_cache.erase(it);
		else ++it;
	}
}
// Files are opened through a sound_service, which gives sounds packs, decryption and anything else that registers a protocol or filter.
static std::unique_ptr<sound_service> g_service;
static size_t g_pack_slot = 0, g_encryption_slot = 0;
static std::shared_ptr<const pack_interface> g_default_pack; // An immutable view, as sound_service requires.
static bool init_service() {
	if (g_service) return true;
	g_service = sound_service::make();
	if (!g_service) return false;
	g_service->register_filter(encryption_filter::get_instance(), g_encryption_slot);
	g_service->register_protocol(pack_protocol::get_instance(), g_pack_slot);
	return true;
}
void set_default_pack(const pack_interface* pack) {
	if (pack && pack->get_is_active()) g_default_pack.reset(pack->make_immutable());
	else g_default_pack.reset();
}
const pack_interface* get_default_pack() { return g_default_pack ? g_default_pack->get_mutable() : nullptr; }
void set_decryption_key(const std::string& key) {
	if (!init_service()) return;
	g_service->set_filter_directive(g_encryption_slot, std::make_shared<std::string>(key));
	g_service->set_default_filter(key.empty() ? sound_service::null_filter_slot : g_encryption_slot);
}
// Names a file by where it comes from, so two packs holding the same filename never share a cache entry. Returns an empty string for a name sound_service won't accept.
static std::string prepare_file(const std::string& filename, const pack_interface* pack) {
	if (!init_service()) return "";
	bool from_pack = pack && pack->get_is_active();
	directive_t dir = from_pack ? directive_t(std::shared_ptr<const pack_interface>(pack->make_immutable())) : nullptr;
	return g_service->prepare_triplet(filename, from_pack ? g_pack_slot : sound_service::fs_protocol_slot, dir);
}
// Opens a name from prepare_file, and releases what sound_service kept for it.
static std::unique_ptr<std::istream> open_file(const std::string& triplet) {
	std::unique_ptr<std::istream> s;
	try {
		s.reset(g_service->open_triplet(triplet.c_str()));
	} catch (std::exception&) {}
	g_service->cleanup_triplet(triplet);
	return s;
}
std::unique_ptr<std::istream> open_sound_file(const std::string& filename, const pack_interface* pack) {
	if (!init()) return nullptr;
	std::string key = prepare_file(filename, pack);
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
		size_t n = strlen(scheme);
		if (name.size() > n && Poco::icompare(name.substr(0, n), scheme) == 0) return true;
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

// set_impulse_response. Responses loaded from now on are cut to tail_seconds (0 keeps them whole) and convolved at the sample rate divided by rate_divisor, trading quality for work. 1.5 seconds and full rate are the defaults hide and seek's engine used.
static std::atomic<float> g_ir_tail_seconds{1.5f};
static std::atomic<int> g_ir_rate_divisor{1};
void set_impulse_response_quality(float tail_seconds, int rate_divisor) {
	g_ir_tail_seconds = std::max(tail_seconds, 0.0f);
	g_ir_rate_divisor = std::max(rate_divisor, 1);
}
// Decodes a whole file, through the default pack like sound.load, at the engine's sample rate.
static std::shared_ptr<const pcm_data> decode_file(const std::string& filename) {
	std::string key = prepare_file(filename, g_default_pack.get());
	if (key.empty()) return nullptr;
	std::unique_ptr<std::istream> in = open_file(key);
	std::unique_ptr<decoder> dec = in ? open_decoder(std::move(in)) : nullptr;
	return dec ? decode_all(*dec) : nullptr;
}
bool sound_base::set_impulse_response(const std::string& response, float dry, float wet, const std::string& response_right, float spread) {
	if (!init()) return false;
	if (response.empty()) {
		ir_enabled = false;
		if (ir) ir->clear_ir();
		return true;
	}
	std::shared_ptr<const pcm_data> left = decode_file(response);
	if (!left || left->frames == 0) return false;
	std::vector<float> stereo;
	const float* frames = left->samples.data();
	uint64_t count = left->frames;
	unsigned int channels = left->channels;
	if (!response_right.empty()) {
		// Two responses make one true stereo response: the first file's first channel on the left, the second's on the right.
		std::shared_ptr<const pcm_data> right = decode_file(response_right);
		if (!right || right->frames == 0) return false;
		count = std::max(left->frames, right->frames);
		stereo.assign(size_t(count) * 2, 0.0f);
		for (uint64_t f = 0; f < left->frames; f++) stereo[f * 2] = left->samples[f * left->channels];
		for (uint64_t f = 0; f < right->frames; f++) stereo[f * 2 + 1] = right->samples[f * right->channels];
		frames = stereo.data();
		channels = 2;
	}
	if (!ir) {
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		ir = std::make_unique<convolver>(2, g_sample_rate);
	}
	if (!ir->set_ir(frames, count, channels, g_ir_tail_seconds, unsigned(g_ir_rate_divisor))) return false;
	set_impulse_response_params(dry, wet, spread);
	ir_enabled = true;
	return true;
}
// Cheap enough to call every frame, as a game easing the reverb with distance does: nothing is reloaded.
bool sound_base::set_impulse_response_params(float dry, float wet, float spread) {
	ir_dry = std::max(dry, 0.0f);
	ir_wet = std::max(wet, 0.0f);
	ir_spread = std::clamp(spread, 0.0f, 1.0f);
	return true;
}
// Spread narrows what feeds the convolver toward mono: at 1 each channel is convolved with its own input, so the wet follows the source's pan; at 0 both get the mono sum, so the wet's stereo comes from the response alone and stays centred.
void sound_base::process_ir(float* frames, int count, int depth) {
	if (!ir_enabled || !ir) return;
	float spread = ir_spread;
	const float* conv_in = frames;
	if (spread < 1.0f) {
		float* narrowed = scratch(1, depth, size_t(count) * 2);
		for (int f = 0; f < count; f++) {
			float l = frames[f * 2], r = frames[f * 2 + 1], mono = (l + r) * 0.5f;
			narrowed[f * 2] = mono + spread * (l - mono);
			narrowed[f * 2 + 1] = mono + spread * (r - mono);
		}
		conv_in = narrowed;
	}
	ir->process(conv_in, frames, frames, unsigned(count), ir_dry, ir_wet);
}

// sound
sound::~sound() {
	if (bytes_pushing) abort_push_bytes(bytes_pushing.get());
	std::shared_ptr<source> old;
	{
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		set_parent(nullptr, true);
		old = std::move(src);
	}
	// old goes out of scope here, outside the graph lock: a stream with a thread of its own joins it on destruction.
}
bool sound::set_mixer(mixer* m) { return set_parent(m ? m : g_master); }
void sound::attach_source(std::shared_ptr<source> s, const std::string& filename) {
	std::shared_ptr<source> old;
	{
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		old = std::move(src);
		src = s;
		playing = false;
		ended = false;
		looping = false;
		last_gain_left = last_gain_right = -1;
		if (s->sample_rate() != unsigned(g_sample_rate) || get_pitch() != 100) resampler.reset(make_resampler());
		else resampler.reset();
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
		g_cache[cache_key] = cache_entry{pcm, bytes, Poco::Timestamp()};
	}
	return true;
}
bool sound::attach_cached(const std::string& cache_key, const std::string& name) {
	std::shared_ptr<const pcm_data> pcm;
	std::shared_ptr<const std::string> bytes;
	{
		std::lock_guard<std::mutex> l(g_cache_mutex);
		auto it = g_cache.find(cache_key);
		if (it == g_cache.end()) return false;
		it->second.last_used.update();
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
	std::string key = prepare_file(filename, pack);
	if (key.empty()) return false;
	if (allow_preloads && attach_cached(key, filename)) {
		g_service->cleanup_triplet(key);
		return true;
	}
	std::unique_ptr<std::istream> in = open_file(key);
	if (!in) {
		NVGT_ERROR("nvgt.sound", Poco::format("could not open %s", filename));
		return false;
	}
	if (load_bytes(read_all(*in), key, filename, allow_preloads)) return true;
	NVGT_ERROR("nvgt.sound", Poco::format("could not load %s: it isn't audio in a supported format, or it is damaged", filename));
	return false;
}
bool sound::stream(const std::string& filename, const pack_interface* pack) {
	if (!init()) return false;
	close();
	std::string key = prepare_file(filename, pack);
	if (key.empty()) return false;
	std::unique_ptr<std::istream> in = open_file(key);
	if (!in) {
		NVGT_ERROR("nvgt.sound", Poco::format("could not open %s", filename));
		return false;
	}
	std::unique_ptr<decoder> dec = open_decoder(std::move(in));
	if (!dec) {
		NVGT_ERROR("nvgt.sound", Poco::format("could not stream %s: it isn't audio in a supported format, or it is damaged", filename));
		return false;
	}
	attach_source(make_stream_source(std::move(dec)), filename);
	return true;
}
bool sound::load_url(const std::string& url) {
	if (!init()) return false;
	close();
	std::istream* source = nullptr;
	try {
		source = Poco::URIStreamOpener::defaultOpener().open(url);
	} catch (std::exception& e) {
		NVGT_ERROR("nvgt.sound", Poco::format("could not connect to %s: %s", url, std::string(e.what())));
	}
	if (!source) return false;
	std::unique_ptr<prebuffer_istream> stream;
	try {
		stream = std::make_unique<prebuffer_istream>(*source);
	} catch (std::exception&) {
		delete source;
		return false;
	}
	stream->own_source();
	std::shared_ptr<prebuffer_ring> ring = stream->get_ring();
	// Any data at all means the server is talking; then give format detection a head start so it rarely waits on the network.
	if (!ring->wait_for(1, NETSTREAM_STALL_MS) || !ring->wait_for(32 * 1024, 3000)) {
		NVGT_ERROR("nvgt.sound", Poco::format("could not stream %s: the server sent nothing", url));
		return false;
	}
	std::unique_ptr<decoder> dec = open_decoder(std::move(stream));
	if (!dec) return false;
	unsigned int rate = dec->sample_rate;
	attach_source(make_stream_source(std::move(dec), true, [ring] { ring->abort(); }, rate / 10), url);
	return true;
}
bool sound::load_memory(std::string& data, unsigned int size, const std::string& preload_filename, bool legacy_encrypt) {
	if (!init()) return false;
	close();
	std::string key = "\x1ememory\x1e" + preload_filename, name = preload_filename.empty() ? "script_stream" : preload_filename;
	if (!preload_filename.empty() && attach_cached(key, name)) return true;
	size_t n = std::min<size_t>(size, data.size());
	auto bytes = std::make_shared<std::string>(data, 0, n);
	if (legacy_encrypt) {
		for (size_t i = 0; i < n; i++) (*bytes)[i] = char(sound_data_char_decrypt((unsigned char)(*bytes)[i], unsigned(i), unsigned(n)));
	}
	return load_bytes(bytes, key, name, !preload_filename.empty());
}
// The legacy engine let BASS call these from its own threads, which isn't safe for script code. They are called here, on the script's thread, to read the whole file up front.
bool sound::load_callbacks(asIScriptFunction* close_cb, asIScriptFunction* length_cb, asIScriptFunction* read_cb, asIScriptFunction* seek_cb, const std::string& data, const std::string& preload_filename) {
	struct releaser {
		asIScriptFunction* f[4];
		~releaser() {
			for (asIScriptFunction* x : f) {
				if (x) x->Release();
			}
		}
	} release_callbacks{{close_cb, length_cb, read_cb, seek_cb}};
	if (!init()) return false;
	close();
	std::string key = "\x1ememory\x1e" + preload_filename, name = preload_filename.empty() ? "script_stream" : preload_filename;
	if (!preload_filename.empty() && attach_cached(key, name)) return true;
	if (!read_cb) return false;
	asIScriptEngine* engine = read_cb->GetEngine();
	asIScriptContext* ctx = engine->RequestContext();
	if (!ctx) return false;
	std::string user_data = data;
	unsigned int total = 0;
	if (length_cb && ctx->Prepare(length_cb) >= 0 && ctx->SetArgObject(0, &user_data) >= 0 && ctx->Execute() == asEXECUTION_FINISHED) total = ctx->GetReturnDWord();
	if (total == 0 || total == 0xffffffff) total = 0x7fffffff; // Unknown, so read until the callback runs dry.
	auto bytes = std::make_shared<std::string>();
	while (bytes->size() < total) {
		std::string chunk;
		unsigned int want = unsigned(std::min<size_t>(65536, total - bytes->size()));
		if (ctx->Prepare(read_cb) < 0 || ctx->SetArgAddress(0, &chunk) < 0 || ctx->SetArgDWord(1, want) < 0 || ctx->SetArgObject(2, &user_data) < 0 || ctx->Execute() != asEXECUTION_FINISHED) break;
		if (int(ctx->GetReturnDWord()) <= 0 || chunk.empty()) break;
		if (chunk.size() > want) chunk.resize(want);
		bytes->append(chunk);
	}
	if (close_cb && ctx->Prepare(close_cb) >= 0 && ctx->SetArgObject(0, &user_data) >= 0) ctx->Execute();
	engine->ReturnContext(ctx);
	if (bytes->empty()) return false;
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
bool sound::push_floats(CScriptArray* data, bool end_stream, int pcm_rate, int pcm_channels) {
	if (!init()) return false;
	size_t n = data ? data->GetSize() : 0;
	if (!end_stream && n == 0) return false;
	if (!loaded_filename.empty()) {
		if (n == 0) return false;
		close();
	}
	if (bytes_pushing) return false; // An encoded push stream can't take raw samples.
	if (!pcm_pushing) {
		if (pcm_rate <= 0 || pcm_channels < 1 || pcm_channels > max_source_channels) return false;
		pcm_pushing = make_pcm_push_source(pcm_rate, pcm_channels);
		attach_source(as_source(pcm_pushing), "");
	}
	if (n) pcm_push(pcm_pushing.get(), (const float*)data->At(0), n);
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
	std::shared_ptr<source> old;
	{
		std::lock_guard<std::recursive_mutex> l(g_graph_mutex);
		old = std::move(src);
		playing = false;
	}
	bytes_pushing.reset();
	pcm_pushing.reset();
	ended = false;
	forced_length = 0;
	loaded_filename.clear();
	return true; // old is released here, outside the graph lock.
}
bool sound::play(bool reset_loop_state) {
	if (!src) return false;
	if (!loaded_filename.empty() && playing) return false;
	if (ended) {
		src->seek(0);
		ended = false;
	}
	if (reset_loop_state) {
		looping = false;
		src->set_loop(false);
	}
	if (!parent) set_parent(get_default_mixer());
	playing = true;
	return true;
}
bool sound::play_wait() {
	if (!play()) return false;
	while (playing && src) wait(5);
	return true;
}
bool sound::play_looped() {
	if (!src || (!loaded_filename.empty() && playing)) return false;
	looping = true;
	src->set_loop(true); // Before play, so the sound can't reach its end in between and stop.
	return play(false);
}
bool sound::pause() {
	if (!src || !playing) return false;
	playing = false;
	reset_phonon_state(phonon);
	return true;
}
bool sound::stop() {
	if (!src) return false;
	playing = false;
	src->seek(0);
	ended = false;
	reset_phonon_state(phonon);
	return true;
}
bool sound::seek(float ms) {
	if (!src || ms < 0) return false;
	if (!src->seek(uint64_t(double(ms) / 1000.0 * src->sample_rate()))) return false;
	ended = false;
	reset_phonon_state(phonon);
	return true;
}
bool sound::is_active() { return src && parent; }
bool sound::is_playing() { return src && parent && playing; }
bool sound::is_paused() { return src && parent && !playing && !ended; }
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
void sound::render(float* out, int frames, int depth) {
	if (!src || !playing) return;
	uint64_t now = g_frame_clock;
	float vol, pn, pt, pos_vol, pos_pan;
	bool is_positioned;
	spatial_params sp;
	{
		std::lock_guard<spinlock> l(params_lock);
		vol = volume.at(now);
		pn = pan.at(now);
		pt = pitch.at(now);
		sp = snapshot_position(is_positioned);
	}
	unsigned int ch = src->channels();
	float* raw = scratch(1, depth, size_t(max_render_frames) * ch);
	float* st = scratch(0, depth, size_t(frames) * 2);
	bool src_ended = false;
	auto pull = [&](float* dest, int n) {
		src->read(raw, n, src_ended);
		if (ch == 1) {
			for (int f = 0; f < n; f++) dest[f * 2] = dest[f * 2 + 1] = raw[f];
		} else {
			for (int f = 0; f < n; f++) {
				dest[f * 2] = raw[f * ch];
				dest[f * 2 + 1] = raw[f * ch + 1];
			}
		}
	};
	if (!resampler && src->sample_rate() != unsigned(g_sample_rate)) resampler.reset(make_resampler()); // Only reached if the setters were bypassed.
	if (!resampler) pull(st, frames);
	else {
		resampler->SetRates(double(src->sample_rate()) * pt / 100.0, g_sample_rate);
		WDL_ResampleSample* in;
		int need = std::min(resampler->ResamplePrepare(frames, 2, &in), max_render_frames);
		pull(in, need);
		int got = resampler->ResampleOut(st, need, frames, 2);
		if (got < frames) memset(st + got * 2, 0, (frames - got) * 2 * sizeof(float));
	}
	process_fx(st, frames);
	spatialize(st, frames, sp, is_positioned, pos_vol, pos_pan);
	render_sends(st, frames, vol, pos_vol, pn, pos_pan);
	apply_gain(st, out, frames, vol + pos_vol + dry_volume.load(), pn, pos_pan);
	if (src_ended) {
		playing = false;
		ended = true;
	}
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
	for (sound* s : done) s->release(); // Outside the lock, since releasing the last reference takes the graph lock.
}
// position, if given, is relative to a listener at the origin facing north, as set_position takes it. A sound that isn't playing yet (autoplay false) is let go on the next call unless the script holds on to it.
static sound* sound_play(const std::string& path, const reactphysics3d::Vector3& position, float volume, float pan, float pitch, mixer* mix, const pack_interface* pack, bool autoplay) {
	release_played(false);
	sound* s = new sound();
	if (!s->load(path, pack)) {
		s->release();
		return nullptr;
	}
	if (mix) s->set_mixer(mix);
	if (position.x != FLT_MAX || position.y != FLT_MAX || position.z != FLT_MAX) s->set_position(0, 0, 0, position.x, position.y, position.z, 0, 1, 1);
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
// How far a URL stream reads ahead of what is playing. The gauge is for the most recently active one.
static void set_netstream_buffer_size(unsigned int bytes) { g_netstream_buffer_size = bytes; }
static unsigned int get_netstream_buffer_size() { return unsigned(g_netstream_buffer_size); }
static unsigned int get_netstream_buffered() { return unsigned(g_netstream_buffered.load()); }

// Script bindings.
sound* make_sound() { return new sound(); }
mixer* make_mixer() { return new mixer(); }
template <bool (sound_base::*F)(float)> static void void_setter(sound_base* obj, float v) { (obj->*F)(v); }
static CScriptArray* to_script_array(const std::vector<std::string>& v) {
	CScriptArray* a = CScriptArray::Create(g_ScriptEngine->GetTypeInfoByDecl("array<string>"), asUINT(v.size()));
	for (asUINT i = 0; i < v.size(); i++) *(std::string*)a->At(i) = v[i];
	return a;
}
static CScriptArray* script_output_devices() { return to_script_array(get_output_device_names()); }
static CScriptArray* script_input_devices() { return to_script_array(get_input_device_names()); }
static unsigned int script_output_device_count() { return unsigned(get_output_device_names().size()); }
static unsigned int script_input_device_count() { return unsigned(get_input_device_names().size()); }
static void script_set_output_device(unsigned int d) { set_output_device(d); }
bool get_global_hrtf() { return g_global_hrtf; }
void set_global_hrtf(bool enable) {
	g_global_hrtf = enable;
	if (enable) phonon_init();
}
// A plain array handle parameter hands its reference to the callee.
static bool script_push_floats(sound* s, CScriptArray* data, bool end_stream, int pcm_rate, int pcm_channels) {
	bool r = s->push_floats(data, end_stream, pcm_rate, pcm_channels);
	if (data) data->Release();
	return r;
}

// Registered as bool, as the legacy engine was. That engine returned the effect's index through it, which made adding the first effect read as false; this returns whether it worked.
static bool script_set_fx(sound_base* obj, const std::string& fx, int index) { return obj->set_fx(fx, index) >= 0; }
template <class T> static void register_sound_base(asIScriptEngine* engine, const char* type) {
	engine->RegisterObjectMethod(type, "bool set_fx(const string&in fx, int index = -1)", asFUNCTION(script_set_fx), asCALL_CDECL_OBJFIRST);
	engine->RegisterObjectBehaviour(type, asBEHAVE_ADDREF, "void f()", asMETHOD(sound_base, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour(type, asBEHAVE_RELEASE, "void f()", asMETHOD(sound_base, release), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool set_position(float listener_x, float listener_y, float listener_z, float sound_x, float sound_y, float sound_z, float rotation = 0.0, float pan_step = 1.0, float volume_step = 1.0)", asMETHOD(sound_base, set_position), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool set_mixer(mixer@+ mixer = null)", asMETHOD(T, set_mixer), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool set_send(mixer@+ target, float volume = 0, bool after_distance = false)", asMETHOD(sound_base, set_send), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool remove_send(mixer@+ target = null)", asMETHOD(sound_base, remove_send), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "float get_send(mixer@+ target) const", asMETHOD(sound_base, get_send), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "float get_dry_volume() const property", asMETHOD(sound_base, get_dry_volume), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "void set_dry_volume(float volume) property", asMETHOD(sound_base, set_dry_volume), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "void set_hrtf(bool enable = true)", asMETHOD(sound_base, set_hrtf), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool get_sliding() const property", asMETHOD(sound_base, is_sliding), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool get_pan_sliding() const property", asMETHOD(sound_base, is_pan_sliding), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool get_pitch_sliding() const property", asMETHOD(sound_base, is_pitch_sliding), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "bool get_volume_sliding() const property", asMETHOD(sound_base, is_volume_sliding), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "float get_pitch() const property", asMETHOD(sound_base, get_pitch), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "void set_pitch(float pitch) property", asFUNCTION(void_setter<&sound_base::set_pitch>), asCALL_CDECL_OBJFIRST);
	engine->RegisterObjectMethod(type, "bool slide_pitch(float pitch, uint time)", asMETHOD(sound_base, slide_pitch), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "float get_pan() const property", asMETHOD(sound_base, get_pan), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "void set_pan(float pan) property", asFUNCTION(void_setter<&sound_base::set_pan>), asCALL_CDECL_OBJFIRST);
	engine->RegisterObjectMethod(type, "bool slide_pan(float pan, uint time)", asMETHOD(sound_base, slide_pan), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "float get_volume() const property", asMETHOD(sound_base, get_volume), asCALL_THISCALL);
	engine->RegisterObjectMethod(type, "void set_volume(float volume) property", asFUNCTION(void_setter<&sound_base::set_volume>), asCALL_CDECL_OBJFIRST);
	engine->RegisterObjectMethod(type, "bool slide_volume(float volume, uint time)", asMETHOD(sound_base, slide_volume), asCALL_THISCALL);
}

void RegisterNvsound(asIScriptEngine* engine) {
	// Default arguments are resolved from the script's namespace, so name our own sound_default_pack explicitly or a global one of the same name wins.
	std::string ns = engine->GetDefaultNamespace();
	std::string default_pack = ns.empty() ? "sound_default_pack" : ns + "::sound_default_pack";
	RegisterLegacyPack(engine);
	engine->RegisterGlobalFunction("void set_impulse_response_quality(float tail_seconds = 1.5, int rate_divisor = 1)", asFUNCTION(set_impulse_response_quality), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_default_pack(pack_interface@+ pack) property", asFUNCTION(set_default_pack), asCALL_CDECL);
	engine->RegisterGlobalFunction("pack_interface@ get_sound_default_pack() property", asFUNCTION(get_default_pack), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_default_decryption_key(const string&in key) property", asFUNCTION(set_decryption_key), asCALL_CDECL);
	engine->RegisterFuncdef("void sound_close_callback(string user_data)");
	engine->RegisterFuncdef("uint sound_length_callback(string user_data)");
	engine->RegisterFuncdef("int sound_read_callback(string &out data, uint length, string user_data)");
	engine->RegisterFuncdef("bool sound_seek_callback(uint offset, string user_data)");
	engine->RegisterObjectType("mixer", 0, asOBJ_REF);
	engine->RegisterObjectType("sound", 0, asOBJ_REF);
	engine->RegisterObjectBehaviour("mixer", asBEHAVE_FACTORY, "mixer@ m()", asFUNCTION(make_mixer), asCALL_CDECL);
	register_sound_base<mixer>(engine, "mixer");
	engine->RegisterObjectMethod("mixer", "bool set_impulse_response(const string&in response, float dry = 1.0f, float wet = 0.35f, const string&in response_right = \"\", float spread = 1.0f)", asMETHOD(sound_base, set_impulse_response), asCALL_THISCALL);
	engine->RegisterObjectMethod("mixer", "bool set_impulse_response_params(float dry, float wet, float spread = 1.0f)", asMETHOD(sound_base, set_impulse_response_params), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("sound", asBEHAVE_FACTORY, "sound@ s()", asFUNCTION(make_sound), asCALL_CDECL);
	register_sound_base<sound>(engine, "sound");
	engine->RegisterObjectMethod("sound", "const string& get_loaded_filename() const property", asMETHOD(sound, get_loaded_filename), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", ("bool load(const string&in filename, const pack_interface@+ pack = " + default_pack + ", bool allow_preloads = true)").c_str(), asMETHOD(sound, load), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool load(sound_close_callback@ close_cb, sound_length_callback@ length_cb, sound_read_callback@ read_cb, sound_seek_callback@ seek_cb, const string&in user_data, const string&in preload_filename = \"\")", asMETHOD(sound, load_callbacks), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool load(string& data, uint size, const string&in preload_filename = \"\", bool legacy_encrypt = false)", asMETHOD(sound, load_memory), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool load_url(const string&in url)", asMETHOD(sound, load_url), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", ("bool stream(const string&in filename, const pack_interface@+ pack = " + default_pack + ")").c_str(), asMETHOD(sound, stream), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool push_memory(const string&in data, bool end_stream = false, int pcm_rate = 0, int pcm_channels = 0)", asMETHOD(sound, push_memory), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool push_memory(const float[]@ data, bool end_stream = false, int pcm_rate = 0, int pcm_channels = 0)", asFUNCTION(script_push_floats), asCALL_CDECL_OBJFIRST);
	engine->RegisterObjectMethod("sound", "bool close()", asMETHOD(sound, close), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "void set_length(float length = 0.0)", asMETHOD(sound, set_length), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool play(bool reset_loop_state = true)", asMETHOD(sound, play), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool play_wait()", asMETHOD(sound, play_wait), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool play_looped()", asMETHOD(sound, play_looped), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool pause()", asMETHOD(sound, pause), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool stop()", asMETHOD(sound, stop), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool seek(float position)", asMETHOD(sound, seek), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool get_active() const property", asMETHOD(sound, is_active), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool get_playing() const property", asMETHOD(sound, is_playing), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "bool get_paused() const property", asMETHOD(sound, is_paused), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "float get_length() const property", asMETHOD(sound, get_length), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "float get_position() const property", asMETHOD(sound, get_position), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound", "double get_pitch_lower_limit() const property", asMETHOD(sound, get_pitch_lower_limit), asCALL_THISCALL);
	engine->RegisterGlobalFunction("bool get_SOUND_AVAILABLE() property", asFUNCTION(is_available), asCALL_CDECL);
	engine->RegisterGlobalFunction("float get_sound_master_volume() property", asFUNCTION(get_master_volume), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_master_volume(float volume) property", asFUNCTION(set_master_volume), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_input_device() property", asFUNCTION(get_input_device), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_input_device(uint device) property", asFUNCTION(set_input_device), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_input_device_count() property", asFUNCTION(script_input_device_count), asCALL_CDECL);
	engine->RegisterGlobalFunction("array<string>@ get_sound_input_devices() property", asFUNCTION(script_input_devices), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_output_device() property", asFUNCTION(get_output_device), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_output_device(uint device) property", asFUNCTION(script_set_output_device), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_output_device_count() property", asFUNCTION(script_output_device_count), asCALL_CDECL);
	engine->RegisterGlobalFunction("array<string>@ get_sound_output_devices() property", asFUNCTION(script_output_devices), asCALL_CDECL);
	engine->RegisterGlobalFunction("bool get_sound_global_hrtf() property", asFUNCTION(get_global_hrtf), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_global_hrtf(bool enable) property", asFUNCTION(set_global_hrtf), asCALL_CDECL);
	engine->RegisterGlobalProperty("mixer@ sound_default_mixer", &g_default_mixer);
	engine->RegisterGlobalFunction(("sound@ sound_play(const string&in path, const vector&in position = vector(FLOAT_MAX, FLOAT_MAX, FLOAT_MAX), float volume = 0.0, float pan = 0.0, float pitch = 100.0, mixer@+ mix = null, const pack_interface@+ pack_file = " + default_pack + ", bool autoplay = true)").c_str(), asFUNCTION(sound_play), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_netstream_buffer_size(uint bytes) property", asFUNCTION(set_netstream_buffer_size), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_netstream_buffer_size() property", asFUNCTION(get_netstream_buffer_size), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_netstream_buffered() property", asFUNCTION(get_netstream_buffered), asCALL_CDECL);
}

} // namespace nvsound
