/* nvsound.h - NVGT's sound engine: SDL3 device output, our own mixer tree, Steam Audio for spatialization.
 * It replaced a MiniAudio based engine, and exposes the API that the legacy_sound plugin did (sound, mixer, sound_environment).
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
#include "nvsound_common.h"

class asIScriptEngine;
class asIScriptFunction;
class CScriptArray;
class WDL_Resampler;
class pack_interface;

namespace nvsound {

class convolver;
class effect;
class mixer;
struct phonon_state;
struct spatial_params;
class sound_environment;
// A send (set_send): a copy of a node's output mixed into another mixer as well as its own. The volume is atomic so the script can move it every frame without waiting on the audio thread.
struct send_slot {
	mixer* target;
	std::atomic<float> volume;
	bool after_distance;
	float last_left = -1, last_right = -1; // The audio thread's last gains, ramped from so volume changes don't click.
};
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
	// set_impulse_response's convolution reverb. The convolver is created once and never replaced, so the audio thread can read the pointer without a lock; responses are swapped inside it.
	std::unique_ptr<convolver> ir;
	std::atomic<bool> ir_enabled{false};
	std::atomic<float> ir_dry{1.0f}, ir_wet{0.35f}, ir_spread{1.0f};
	void process_ir(float* frames, int count, int depth);
	void apply_gain(const float* in, float* out, int frames, float vol_db, float pan_value, float position_pan);
	phonon_state* phonon = nullptr; // Created when HRTF is first needed, and only touched by the audio thread after that.
	void spatialize(float* frames, int count, const spatial_params& p, bool is_positioned, float& vol_db, float& pan_value);
	void ensure_phonon();
	spatial_params snapshot_position(bool& is_positioned); // Must be called with params_lock held.
	// Sends. Adding or removing one takes the graph mutex as well as sends_mutex, so the audio thread can read the list while it renders; sends_mutex alone lets the script find a send to change its volume.
	std::mutex sends_mutex;
	std::vector<std::unique_ptr<send_slot>> sends;
	std::atomic<float> dry_volume{0}; // Scales the node's own output but not its sends, for a send that should be heard in place of the dry signal.
	void render_sends(const float* frames, int count, float vol_db, float position_vol_db, float pan_value, float position_pan);
	void clear_sends();
	friend bool reaches(sound_base* from, mixer* m, int depth);
	friend class mixer; // set_mixer checks the sends along its new parents.
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
	void set_hrtf(bool enable);
	bool attach_environment(sound_environment* env);
	int set_fx(const std::string& fx, int index = -1);
	bool set_impulse_response(const std::string& response, float dry = 1.0f, float wet = 0.35f, const std::string& response_right = "", float spread = 1.0f);
	bool set_impulse_response_params(float dry, float wet, float spread = 1.0f);
	bool set_send(mixer* target, float volume = 0, bool after_distance = false);
	bool remove_send(mixer* target = nullptr);
	float get_send(mixer* target);
	float get_dry_volume() { return dry_volume; }
	void set_dry_volume(float v) { dry_volume = std::clamp(v, -100.0f, 0.0f); }
	virtual bool set_mixer(mixer* m) = 0;
};

class mixer : public sound_base {
	std::vector<sound_base*> children;
	friend class sound_base;
	// Sends into this mixer, double buffered by block: senders add to the buffer for the block being rendered, and the mixer plays the previous block's. That makes every send exactly one block late whichever order the tree renders in.
	struct send_input {
		std::vector<float> buf;
		uint64_t block = 0;
		int frames = 0;
	} send_in[2];
	void add_send_input(const float* in, int frames, uint64_t block, float from_left, float from_right, float to_left, float to_right);
	void mix_send_input(float* acc, int frames, uint64_t block);
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
	// For C++ callers such as text to speech and tone_synth. load_pcm copies the samples and converts them to the engine's rate; load_source plays any source, such as a push source the caller keeps feeding.
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

void set_impulse_response_quality(float tail_seconds = 1.5f, int rate_divisor = 1);
bool get_global_hrtf();
void set_global_hrtf(bool enable);
void close_all_microphones(); // Called by shutdown, so no microphone touches SDL after SDL_Quit.

sound* make_sound(); // For code that can't see the types sounds and mixers own.
mixer* make_mixer();

void RegisterNvsound(asIScriptEngine* engine);
void RegisterNvsoundMicrophone(asIScriptEngine* engine);
void RegisterNvsoundEnvironment(asIScriptEngine* engine);
void RegisterNvsoundDecoder(asIScriptEngine* engine);

} // namespace nvsound
