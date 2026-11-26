/* core.h - the new nvsound core's public side: the sound and mixer objects scripts hold, and the engine functions. See DESIGN.md.
 * The classes and methods are the ones NVGT registers today, so its scripts, and Golden Crayon's, call them unchanged. None of them waits on the audio thread: changes go to it as commands, and getters read what it publishes.
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
#include "../nvsound_common.h"
#include <atomic>
#include <functional>
#include <istream>
#include <memory>
#include <string>
#include <vector>

class asIScriptEngine;

namespace nvsound {

struct node;
struct node_status;
struct send_entry;
struct phonon_state;
class convolver;
class effect;
class mixer;

// Shared by sounds and mixers. The node it drives belongs to the audio thread. This object keeps the script's own view: the values it set, and the shape of the mixer tree, so checks like "would this make a loop" never ask the audio thread.
class sound_base {
protected:
	std::atomic<int> ref_count{1};
	node* n;
	std::shared_ptr<node_status> st;
	// What the script last asked for, for getters to answer until the audio thread has caught up.
	float volume_asked = 0, pan_asked = 0, pitch_asked = 100;
	uint32_t volume_seq = 0, pan_seq = 0, pitch_seq = 0;
	bool volume_slide_asked = false, pan_slide_asked = false, pitch_slide_asked = false;
	bool has_resampler = false;
	bool positioned = false;
	float dry_asked = 0;
	// The script side's view of the set_fx chain, the sends, the convolver and HRTF. The audio thread has its own copies of each and never reads these.
	struct fx_item {
		std::string id;
		std::string type;
		effect* e;
	};
	std::vector<fx_item> fx;
	convolver* ir = nullptr;
	float ir_dry = 1, ir_wet = 0.35f, ir_spread = 1;
	phonon_state* phonon = nullptr;
	bool has_send_input = false; // Mixers only: whether the audio thread has the buffers sends into this mixer need.
	void ensure_phonon();
	void send_fx_chain(const std::vector<effect*>& removed);
	void send_sends(const std::vector<send_entry*>& removed);
	void destroy_node();
	explicit sound_base(node* audio_node);
	void send_resampler();
	friend class mixer;
public:
	mixer* parent = nullptr;
	std::vector<sound_base*> children; // The script side's view of a mixer's children. Only mixers have any.
	// The script side's view of this node's sends.
	struct send_item {
		mixer* target;
		send_entry* entry;
	};
	std::vector<send_item> sends;
	bool use_hrtf = false;
	virtual ~sound_base();
	void add_ref();
	void release();
	bool set_parent(mixer* m, bool internal = false);
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
	void set_hrtf(bool enable);
	int set_fx(const std::string& fx, int index = -1);
	bool set_impulse_response(const std::string& response, float dry = 1.0f, float wet = 0.35f, const std::string& response_right = "", float spread = 1.0f);
	bool set_impulse_response_params(float dry, float wet, float spread = 1.0f);
	bool set_send(mixer* target, float volume = 0, bool after_distance = false);
	bool remove_send(mixer* target = nullptr);
	float get_send(mixer* target);
	float get_dry_volume();
	void set_dry_volume(float v);
	virtual bool set_mixer(mixer* m) = 0;
};

class mixer : public sound_base {
public:
	mixer(bool is_master = false);
	~mixer();
	bool set_mixer(mixer* m) override;
	bool is_ancestor_of(mixer* m);
};

class sound : public sound_base {
	std::shared_ptr<source> src; // The script's own reference, for length and rate. The audio thread holds another.
	std::shared_ptr<pcm_push_source> pcm_pushing;
	std::shared_ptr<push_bytes> bytes_pushing;
	float forced_length = 0;
	// The script's view of playing and ended, kept until the audio thread confirms what the last play, pause or stop did.
	bool playing_asked = false, ended_asked = false;
	uint32_t transport_seq = 0;
	double seek_asked_ms = -1;
	uint32_t seek_seq = 0;
	void attach_source(std::shared_ptr<source> s, const std::string& filename);
	bool load_bytes(std::shared_ptr<const std::string> bytes, const std::string& cache_key, const std::string& name, bool cache);
	bool attach_cached(const std::string& cache_key, const std::string& name);
	bool start_pushed_decoder();
	bool playing_now();
	bool ended_now();
public:
	std::string loaded_filename;
	sound();
	~sound();
	bool set_mixer(mixer* m) override;
	bool load(const std::string& filename, const pack_interface* pack, bool allow_preloads = true);
	bool stream(const std::string& filename, const pack_interface* pack);
	bool load_url(const std::string& url);
	bool load_memory(std::string& data, unsigned int size, const std::string& preload_filename = "", bool legacy_encrypt = false);
	bool push_memory(const std::string& data, bool end_stream = false, int pcm_rate = 0, int pcm_channels = 0);
	// For the embedder's own loaders: push_floats from a script array, and load_callbacks once it has read the whole file through the script's callbacks.
	bool push_float_samples(const float* samples, size_t count, bool end_stream, int pcm_rate, int pcm_channels);
	bool load_encoded(std::shared_ptr<const std::string> bytes, const std::string& cache_key, const std::string& name, bool cache);
	bool attach_cached_file(const std::string& cache_key, const std::string& name) { return attach_cached(cache_key, name); }
	bool load_pcm(const float* samples, uint64_t frames, unsigned int sample_rate, unsigned int channels, const std::string& name = "pcm");
	bool load_source(std::shared_ptr<source> s, const std::string& name = "");
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

// Engine level functions, as today's nvsound.h has them.
void shutdown();
bool is_available();
mixer* get_master();
mixer* get_default_mixer();
void set_default_mixer(mixer* m);
mixer** default_mixer_slot(); // For NVGT's sound_default_mixer global, which scripts assign directly.
float get_master_volume();
void set_master_volume(float v);
unsigned int get_output_device();
bool set_output_device(unsigned int index);
std::vector<std::string> get_output_device_names();
unsigned int get_input_device();
void set_input_device(unsigned int index);
std::vector<std::string> get_input_device_names();
bool get_global_hrtf();
void set_global_hrtf(bool enable);
void set_impulse_response_quality(float tail_seconds = 1.5f, int rate_divisor = 1);
void close_all_microphones();
sound* make_sound();
mixer* make_mixer();
sound* sound_play(const std::string& path, float x, float y, float z, bool positioned, float volume, float pan, float pitch, mixer* mix, const pack_interface* pack, bool autoplay);

// What the core needs from whoever embeds it: NVGT, or Golden Crayon's Python module. The default reads plain files and does nothing else.
struct host {
	virtual ~host() {}
	// A cache key names a file by where it comes from, so two packs holding the same name never share an entry. Empty for a name that can't be opened.
	virtual std::string cache_key(const std::string& filename, const pack_interface* pack);
	virtual std::unique_ptr<std::istream> open(const std::string& cache_key);
	virtual const pack_interface* default_pack() { return nullptr; } // What impulse responses load through, as sound.load's default argument does.
	virtual void done(const std::string& cache_key) {} // The key won't be opened again.
	// A network stream, and what makes a read blocked on it return. Null when URLs aren't supported.
	virtual std::unique_ptr<std::istream> open_url(const std::string& url, std::function<void()>& abort) { return nullptr; }
	// load_memory's legacy_encrypt: BASS era sound data obfuscation.
	virtual void legacy_decrypt(std::string& bytes) {}
	virtual void wait(int ms);
	virtual void log_error(const std::string& message) {}
	// While offline, the embedder's clock follows the audio rendered. now() is the clock's reading in microseconds when offline mode begins, and set_clock() moves it.
	virtual int64_t now_micros() { return 0; }
	virtual void set_clock(int64_t micros) {}
};
void set_host(std::unique_ptr<host> h);
host& get_host();

// Offline rendering, as on the nvsound-offline-render branch.
bool offline_begin(int sample_rate);
bool offline_record(const std::string& path);
bool offline_render(double ms);
bool offline_stop();

// Script registration, in the embedder's adapter (nvgt_adapter.cpp for NVGT) and the files shared with today's core.
void RegisterNvsound(asIScriptEngine* engine);
void RegisterNvsoundMicrophone(asIScriptEngine* engine);
void RegisterNvsoundEnvironment(asIScriptEngine* engine);
void RegisterNvsoundDecoder(asIScriptEngine* engine);
void set_default_pack(const pack_interface* pack);
const pack_interface* get_default_pack();
void set_decryption_key(const std::string& key);

// Deletes what the audio thread has finished with. Run on a timer by the engine, and at times by script calls, so it never piles up.
size_t collect_garbage();

} // namespace nvsound
