/* nvsound_effects.cpp - the effects that sound.set_fx and mixer.set_fx apply, and the effect string parser.
 * Effect strings are the legacy engine's: an optional $id, the effect's name, then its parameters, separated by colons, e.g. "$muffle:lowpass:800:0:0.7".
 * Parameters mean what they meant to BASS_FX so existing scripts sound the same, though the processing is our own.
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
#include "nvsound_effects.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
// The implementations of these two are compiled in dep/ma_reverb_node.c and dep/ma_plate_node.c for as long as those files exist.
#include <plateverb.h>
#include <verblib.h>

namespace nvsound {

static const float pi = 3.14159265358979f;
static float arg_float(const std::vector<std::string>& args, size_t i, float def = 0) { return i < args.size() ? strtof(args[i].c_str(), nullptr) : def; }
static float db_gain(float db) { return powf(10.0f, db / 20.0f); }
static float millibel_gain(float mb) { return powf(10.0f, mb / 2000.0f); }

// highpass, lowpass and bandpass: center frequency, bandwidth in octaves (used when non-zero), Q (used otherwise). These are the RBJ cookbook filters BASS_FX's BQF is based on.
class biquad_effect : public effect {
public:
	enum kind_t { lowpass, highpass, bandpass } kind;
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
	float z1[2] = {0, 0}, z2[2] = {0, 0};
	biquad_effect(kind_t k) : kind(k) {}
	const char* type() const override { return "bqf"; }
	bool configure(const std::vector<std::string>& args) override {
		float rate = float(get_sample_rate());
		float freq = std::clamp(arg_float(args, 1), 1.0f, rate * 0.49f), bandwidth = arg_float(args, 2), q = arg_float(args, 3);
		float w0 = 2 * pi * freq / rate, cs = cosf(w0), sn = sinf(w0);
		float alpha = bandwidth > 0 ? sn * sinhf(logf(2.0f) / 2 * bandwidth * w0 / sn) : sn / (2 * std::max(q, 0.01f));
		float a0 = 1 + alpha;
		switch (kind) {
			case lowpass:
				b0 = (1 - cs) / 2;
				b1 = 1 - cs;
				b2 = (1 - cs) / 2;
				break;
			case highpass:
				b0 = (1 + cs) / 2;
				b1 = -(1 + cs);
				b2 = (1 + cs) / 2;
				break;
			case bandpass: // Constant 0 dB peak gain, as BASS_BFX_BQF_BANDPASS.
				b0 = alpha;
				b1 = 0;
				b2 = -alpha;
				break;
		}
		b0 /= a0;
		b1 /= a0;
		b2 /= a0;
		a1 = -2 * cs / a0;
		a2 = (1 - alpha) / a0;
		return true;
	}
	void process(float* frames, int count) override {
		for (int i = 0; i < count * 2; i++) {
			int c = i & 1;
			float x = frames[i], y = b0 * x + z1[c];
			z1[c] = b1 * x - a1 * y + z2[c];
			z2[c] = b2 * x - a2 * y;
			frames[i] = y;
		}
	}
};

// volume takes a level from 0 to 1 on the legacy decibel curve (0.5 is -50 dB); lvolume takes a linear gain.
class volume_effect : public effect {
	bool linear;
	float gain = 1, last = -1;
public:
	volume_effect(bool linear) : linear(linear) {}
	const char* type() const override { return "volume"; }
	bool configure(const std::vector<std::string>& args) override {
		float v = arg_float(args, 1);
		gain = linear ? v : powf(10.0f, (v * 100.0f - 100.0f) / 20.0f);
		return true;
	}
	void process(float* frames, int count) override {
		if (last < 0) last = gain;
		float g = last, step = (gain - last) / count;
		for (int f = 0; f < count; f++) {
			g += step;
			frames[f * 2] *= g;
			frames[f * 2 + 1] *= g;
		}
		last = gain;
	}
};

// rotate: carries the sound around from one side to the other and back, rate times a second. A negative rate turns the other way.
class rotate_effect : public effect {
	float rate = 0, phase = 0;
public:
	const char* type() const override { return "rotate"; }
	bool configure(const std::vector<std::string>& args) override {
		rate = arg_float(args, 1);
		return true;
	}
	void process(float* frames, int count) override {
		float step = 2 * pi * rate / get_sample_rate();
		for (int f = 0; f < count; f++) {
			float m = (frames[f * 2] + frames[f * 2 + 1]) * 0.5f, angle = (sinf(phase) + 1) * pi / 4;
			frames[f * 2] = m * cosf(angle) * 1.41421356f;
			frames[f * 2 + 1] = m * sinf(angle) * 1.41421356f;
			phase = fmodf(phase + step, 2 * pi);
		}
	}
};

// damp: dynamic amplification. target level, quiet level below which nothing is amplified, adjustment rate, starting gain, and how long in seconds the level must stay low before the gain rises again.
class damp_effect : public effect {
	float target = 0.9f, quiet = 0, rate = 0.1f, gain = 1, delay = 0;
	float envelope = 0;
	double held = 0; // Seconds since the gain last had to come down.
public:
	const char* type() const override { return "damp"; }
	bool configure(const std::vector<std::string>& args) override {
		target = std::clamp(arg_float(args, 1), 0.001f, 1.0f);
		quiet = std::clamp(arg_float(args, 2), 0.0f, 1.0f);
		rate = std::clamp(arg_float(args, 3), 0.0f, 1.0f);
		gain = std::max(arg_float(args, 4), 0.0f);
		delay = std::max(arg_float(args, 5), 0.0f);
		return true;
	}
	void process(float* frames, int count) override {
		float peak = 0;
		for (int i = 0; i < count * 2; i++) peak = std::max(peak, fabsf(frames[i]));
		envelope = std::max(peak, envelope * 0.99f);
		float before = gain;
		if (envelope * gain > target) {
			gain = target / envelope; // Too loud: come down at once.
			held = 0;
		} else if (envelope > quiet) {
			held += double(count) / get_sample_rate();
			if (held >= delay) gain += (target / envelope - gain) * rate * 0.1f; // Quiet for long enough: rise gradually.
		}
		float g = before, step = (gain - before) / count;
		for (int f = 0; f < count; f++) {
			g += step;
			frames[f * 2] *= g;
			frames[f * 2 + 1] *= g;
		}
	}
};

// The reverbs below are built on verblib, a Freeverb implementation.
class freeverb_effect : public effect {
protected:
	verblib verb;
	bool ok = false;
public:
	freeverb_effect() { ok = verblib_initialize(&verb, get_sample_rate(), 2) != 0; }
	const char* type() const override { return "freeverb"; }
	// freeverb: dry mix (0 to 1), wet mix (0 to 3), room size, damping, width (each 0 to 1), and 1 to freeze. BASS took the mixes as gains, and verblib scales its own by 2 and 3, so they are divided back down here.
	bool configure(const std::vector<std::string>& args) override {
		if (!ok) return false;
		verblib_set_dry(&verb, arg_float(args, 1) / 2.0f);
		verblib_set_wet(&verb, arg_float(args, 2) / 3.0f);
		verblib_set_room_size(&verb, std::clamp(arg_float(args, 3), 0.0f, 1.0f));
		verblib_set_damping(&verb, std::clamp(arg_float(args, 4), 0.0f, 1.0f));
		verblib_set_width(&verb, std::clamp(arg_float(args, 5), 0.0f, 1.0f));
		verblib_set_mode(&verb, args.size() > 6 && args[6] == "1" ? 1.0f : 0.0f);
		return true;
	}
	void process(float* frames, int count) override {
		if (ok) verblib_process(&verb, frames, frames, count);
	}
	// Finds the room size whose decay is closest to the given number of seconds. Freeverb's usual 0 to 1 can't decay faster than about 0.8 seconds, but verblib doesn't clamp, and below 0 just means less feedback (-2.5 would be none).
	void set_decay_seconds(float seconds) {
		float lo = -2.4f, hi = 1;
		double want = double(seconds) * get_sample_rate();
		for (int i = 0; i < 24; i++) {
			float mid = (lo + hi) / 2;
			verblib_set_room_size(&verb, mid);
			if (verblib_get_decay_time_in_frames(&verb) < want) lo = mid;
			else hi = mid;
		}
		verblib_set_room_size(&verb, (lo + hi) / 2);
	}
};

// Approximates DirectX's reverbs on top of Freeverb: the dry signal passes untouched, and the wet path can be delayed before it reaches the room.
class directx_reverb_effect : public freeverb_effect {
	float dry = 1, input = 1;
	std::vector<float> delay_line, wet;
	size_t delay_frames = 0, delay_pos = 0;
	bool i3dl2;
public:
	directx_reverb_effect(bool i3dl2) : i3dl2(i3dl2) {}
	const char* type() const override { return i3dl2 ? "i3dl2reverb" : "reverb"; }
	bool configure(const std::vector<std::string>& args) override {
		if (!ok) return false;
		verblib_set_dry(&verb, 0);
		verblib_set_width(&verb, 1);
		float wet_gain, decay, hf_ratio, predelay = 0, hf_attenuation = 0;
		if (!i3dl2) {
			// reverb: input gain (dB), reverb mix (dB), reverb time (ms), high frequency decay ratio.
			input = db_gain(arg_float(args, 1));
			wet_gain = db_gain(arg_float(args, 2));
			decay = arg_float(args, 3) / 1000.0f;
			hf_ratio = arg_float(args, 4);
		} else {
			// i3DL2reverb: room, room HF (mB), rolloff, decay time (s), decay HF ratio, reflections (mB), reflections delay (s), reverb (mB), reverb delay (s), diffusion, density, HF reference.
			input = 1;
			hf_attenuation = std::clamp(-arg_float(args, 2) / 10000.0f, 0.0f, 1.0f);
			decay = arg_float(args, 4);
			hf_ratio = arg_float(args, 5);
			float reflections = millibel_gain(arg_float(args, 6)), reverb = millibel_gain(arg_float(args, 8));
			wet_gain = millibel_gain(arg_float(args, 1)) * std::max(reverb, reflections * 0.5f);
			predelay = std::clamp(arg_float(args, 7) + arg_float(args, 9), 0.0f, 0.4f);
		}
		verblib_set_wet(&verb, wet_gain / 3.0f);
		verblib_set_damping(&verb, std::clamp(1.0f - std::min(hf_ratio, 1.0f) + hf_attenuation * 0.5f, 0.0f, 1.0f));
		set_decay_seconds(std::clamp(decay, 0.1f, 20.0f));
		size_t frames = size_t(predelay * get_sample_rate());
		if (frames != delay_frames) {
			delay_frames = frames;
			delay_line.assign(frames * 2, 0.0f);
			delay_pos = 0;
		}
		return true;
	}
	void process(float* frames, int count) override {
		if (!ok) return;
		wet.resize(size_t(count) * 2);
		for (int i = 0; i < count * 2; i++) {
			float x = frames[i] * input;
			if (delay_frames) {
				float delayed = delay_line[delay_pos];
				delay_line[delay_pos] = x;
				delay_pos = (delay_pos + 1) % delay_line.size();
				x = delayed;
			}
			wet[i] = x;
		}
		verblib_process(&verb, wet.data(), wet.data(), count);
		for (int i = 0; i < count * 2; i++) frames[i] = frames[i] * dry + wet[i];
	}
};

// plate: dry mix, wet mix, size (0.1 to 2), decay (0 to 0.999), damping (0 to 1), then optionally width (0 to 1) and predelay in seconds (up to 0.25). New in this engine, it stands in for audio_plate_reverb_node.
class plate_effect : public effect {
	plateverb verb;
	std::vector<char> memory;
	bool ok = false;
public:
	plate_effect() {
		size_t needed = plateverb_get_required_memory(get_sample_rate());
		if (!needed) return;
		memory.resize(needed);
		ok = plateverb_initialize(&verb, get_sample_rate(), 2, memory.data()) != 0;
	}
	const char* type() const override { return "plate"; }
	bool configure(const std::vector<std::string>& args) override {
		if (!ok) return false;
		plateverb_set_dry(&verb, arg_float(args, 1));
		plateverb_set_wet(&verb, arg_float(args, 2));
		plateverb_set_size(&verb, arg_float(args, 3));
		plateverb_set_decay(&verb, arg_float(args, 4));
		plateverb_set_damping(&verb, arg_float(args, 5));
		plateverb_set_width(&verb, std::clamp(arg_float(args, 6, 1.0f), 0.0f, 1.0f));
		plateverb_set_predelay(&verb, arg_float(args, 7, 0.0f));
		return true;
	}
	void process(float* frames, int count) override {
		if (ok) plateverb_process(&verb, frames, frames, count);
	}
};

// Builds an effect from its name, or returns null if the name is unknown or too few parameters were given. The minimum counts are the legacy engine's.
static std::unique_ptr<effect> make_effect(const std::vector<std::string>& args) {
	const std::string& name = args[0];
	size_t params = args.size() - 1;
	std::unique_ptr<effect> e;
	if (name == "lowpass" && params >= 3) e = std::make_unique<biquad_effect>(biquad_effect::lowpass);
	else if (name == "highpass" && params >= 3) e = std::make_unique<biquad_effect>(biquad_effect::highpass);
	else if (name == "bandpass" && params >= 3) e = std::make_unique<biquad_effect>(biquad_effect::bandpass);
	else if (name == "volume" && params >= 1) e = std::make_unique<volume_effect>(false);
	else if (name == "lvolume" && params >= 1) e = std::make_unique<volume_effect>(true);
	else if (name == "rotate" && params >= 1) e = std::make_unique<rotate_effect>();
	else if (name == "damp" && params >= 5) e = std::make_unique<damp_effect>();
	else if (name == "freeverb" && params >= 5) e = std::make_unique<freeverb_effect>();
	else if (name == "reverb" && params >= 4) e = std::make_unique<directx_reverb_effect>(false);
	else if (name == "i3DL2reverb" && params >= 12) e = std::make_unique<directx_reverb_effect>(true);
	else if (name == "plate" && params >= 5) e = std::make_unique<plate_effect>();
	if (e && !e->configure(args)) e.reset();
	return e;
}

// Mirrors the legacy engine's set_fx exactly, including its rules for ids and indexes:
// An empty string removes the effect at index, or every effect when index is -1.
// "$id" alone removes every effect whose id starts with it.
// "$id:name:params" changes the effect with that id in place if there is one (it must be the same type), and otherwise adds a new one.
// A new effect goes in at index if that is a valid position, otherwise at the end.
// Returns the index of the effect added or changed, or -1 on failure.
int sound_base::set_fx(const std::string& fx, int index) {
	std::vector<std::unique_ptr<effect>> removed; // Destroyed after the lock is let go.
	if (fx.empty()) {
		std::lock_guard<std::mutex> l(fx_mutex);
		if (index >= 0 && index < int(effects.size())) {
			removed.push_back(std::move(effects[index]));
			effects.erase(effects.begin() + index);
		} else if (index == -1) {
			for (auto& e : effects) removed.push_back(std::move(e));
			effects.clear();
		} else return -1;
		effect_count = int(effects.size());
		return 1;
	}
	// strtok in the legacy engine skipped empty fields, so "a::b" was "a:b".
	std::vector<std::string> args;
	for (size_t start = 0; start <= fx.size();) {
		size_t end = fx.find(':', start);
		if (end == std::string::npos) end = fx.size();
		if (end > start) args.push_back(fx.substr(start, end - start));
		start = end + 1;
	}
	if (args.empty()) return -1;
	if (args.size() == 1 && args[0][0] == '$') {
		std::lock_guard<std::mutex> l(fx_mutex);
		for (size_t i = 0; i < effects.size();) {
			if (effects[i]->id.compare(0, args[0].size(), args[0]) == 0) {
				removed.push_back(std::move(effects[i]));
				effects.erase(effects.begin() + i);
			} else i++;
		}
		effect_count = int(effects.size());
		return 1;
	}
	std::string id;
	if (args[0][0] == '$') {
		id = args[0].substr(0, 31); // The legacy engine kept ids in 32 byte buffers.
		args.erase(args.begin());
		if (args.empty()) return -1;
	}
	std::unique_ptr<effect> e = make_effect(args);
	if (!e) return -1;
	e->id = id;
	std::lock_guard<std::mutex> l(fx_mutex);
	if (id.size() >= 2) {
		for (size_t i = 0; i < effects.size(); i++) {
			if (effects[i]->id != id) continue;
			if (strcmp(effects[i]->type(), e->type()) != 0) return -1;
			// Reconfigure rather than replace, so a filter or reverb being adjusted every frame keeps its state and doesn't click.
			if (!effects[i]->configure(args)) return -1;
			return int(i);
		}
	}
	if (index >= 0 && index < int(effects.size())) {
		effects.insert(effects.begin() + index, std::move(e));
	} else {
		index = int(effects.size());
		effects.push_back(std::move(e));
	}
	effect_count = int(effects.size());
	return index;
}

void sound_base::process_fx(float* frames, int count) {
	if (effect_count == 0) return;
	std::lock_guard<std::mutex> l(fx_mutex);
	for (auto& e : effects) e->process(frames, count);
}

} // namespace nvsound
