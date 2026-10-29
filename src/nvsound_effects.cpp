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

// echo4: dry mix, wet mix (each -2 to 2), feedback (-1 to 1), delay in seconds (up to 6), and 1 for a stereo echo whose repeats cross from side to side.
class echo_effect : public effect {
	float dry = 1, wet = 0, feedback = 0;
	bool cross = false;
	std::vector<float> line;
	size_t len = 0, pos = 0;
public:
	const char* type() const override { return "echo4"; }
	bool configure(const std::vector<std::string>& args) override {
		dry = std::clamp(arg_float(args, 1), -2.0f, 2.0f);
		wet = std::clamp(arg_float(args, 2), -2.0f, 2.0f);
		feedback = std::clamp(arg_float(args, 3), -1.0f, 1.0f);
		size_t frames = std::max<size_t>(1, size_t(std::clamp(arg_float(args, 4), 0.0f, 6.0f) * get_sample_rate()));
		cross = args.size() > 5 && args[5] == "1";
		if (frames != len) {
			len = frames;
			line.assign(len * 2, 0.0f);
			pos = 0;
		}
		return true;
	}
	void process(float* frames, int count) override {
		for (int f = 0; f < count; f++) {
			float* d = &line[pos * 2];
			float l = d[0], r = d[1], xl = frames[f * 2], xr = frames[f * 2 + 1];
			d[0] = xl + feedback * (cross ? r : l);
			d[1] = xr + feedback * (cross ? l : r);
			frames[f * 2] = xl * dry + l * wet;
			frames[f * 2 + 1] = xr * dry + r * wet;
			if (++pos == len) pos = 0;
		}
	}
};

// chorus: dry mix, wet mix (each -2 to 2), feedback (-1 to 1), the shortest and longest delay in milliseconds (0 to 6000), and how fast the delay travels between them in milliseconds per second (0 to 1000). A sweep of a few milliseconds makes it a flanger.
class chorus_effect : public effect {
	float dry = 1, wet = 0, feedback = 0, min_ms = 0, max_ms = 0, rate = 0;
	double delay_ms = -1, direction = 1;
	std::vector<float> line;
	size_t len = 0, pos = 0;
public:
	const char* type() const override { return "chorus"; }
	bool configure(const std::vector<std::string>& args) override {
		dry = std::clamp(arg_float(args, 1), -2.0f, 2.0f);
		wet = std::clamp(arg_float(args, 2), -2.0f, 2.0f);
		feedback = std::clamp(arg_float(args, 3), -1.0f, 1.0f);
		min_ms = std::clamp(arg_float(args, 4), 0.0f, 6000.0f);
		max_ms = std::clamp(arg_float(args, 5), 0.0f, 6000.0f);
		if (min_ms > max_ms) std::swap(min_ms, max_ms);
		rate = std::clamp(arg_float(args, 6), 0.0f, 1000.0f);
		size_t frames = size_t(max_ms * 0.001f * get_sample_rate()) + 4; // Room for the interpolation either side of the longest delay.
		if (frames > len) {
			len = frames;
			line.assign(len * 2, 0.0f);
			pos = 0;
		}
		if (delay_ms < 0) delay_ms = min_ms;
		delay_ms = std::clamp(delay_ms, double(min_ms), double(max_ms));
		return true;
	}
	void process(float* frames, int count) override {
		double rate_hz = get_sample_rate(), step = rate / rate_hz, frames_per_ms = rate_hz * 0.001;
		for (int f = 0; f < count; f++) {
			double back = std::max(delay_ms * frames_per_ms, 1.0); // At least a frame, since this frame's slot hasn't been written yet.
			size_t whole = size_t(back);
			float frac = float(back - whole);
			size_t a = (pos + len - whole) % len, b = (a + len - 1) % len; // The two frames either side of the delayed point.
			float out[2];
			for (int c = 0; c < 2; c++) out[c] = line[a * 2 + c] + (line[b * 2 + c] - line[a * 2 + c]) * frac;
			for (int c = 0; c < 2; c++) {
				float x = frames[f * 2 + c];
				line[pos * 2 + c] = x + feedback * out[c];
				frames[f * 2 + c] = x * dry + out[c] * wet;
			}
			if (++pos == len) pos = 0;
			// The delay travels back and forth between the limits at a steady speed.
			delay_ms += step * direction;
			if (delay_ms >= max_ms) {
				delay_ms = max_ms;
				direction = -1;
			} else if (delay_ms <= min_ms) {
				delay_ms = min_ms;
				direction = 1;
			}
		}
	}
};

// The sweep shared by phaser and autowah: a frequency that rises from freq to range octaves above it and back, rate times a second.
struct sweep {
	float freq = 100, range = 0, rate = 0;
	double phase = 0;
	void configure(const std::vector<std::string>& args) {
		rate = std::clamp(arg_float(args, 4), 0.0f, 10.0f);
		range = std::clamp(arg_float(args, 5), 0.0f, 10.0f);
		freq = std::clamp(arg_float(args, 6), 1.0f, 1000.0f);
	}
	// Moves the sweep on by frames and returns the frequency it reached, kept safely under Nyquist.
	float advance(int frames) {
		double rate_hz = get_sample_rate();
		phase = fmod(phase + double(rate) * frames / rate_hz, 1.0);
		float position = 0.5f - 0.5f * cosf(2 * pi * float(phase));
		return std::min(freq * powf(2.0f, range * position), float(rate_hz) * 0.45f);
	}
};

// phaser: dry mix, wet mix (each -2 to 2), feedback (-1 to 1), sweep rate in cycles per second (0 to 10), sweep range in octaves (0 to 10), and the frequency the sweep starts from (up to 1000 Hz). Six allpass stages give three moving notches when dry and wet are mixed equally.
class phaser_effect : public effect {
	static const int stages = 6, control_frames = 16; // The allpass coefficient follows the sweep every 16 frames.
	float dry = 1, wet = 0, feedback = 0;
	sweep lfo;
	float z[2][stages] = {}, last[2] = {0, 0};
public:
	const char* type() const override { return "phaser"; }
	bool configure(const std::vector<std::string>& args) override {
		dry = std::clamp(arg_float(args, 1), -2.0f, 2.0f);
		wet = std::clamp(arg_float(args, 2), -2.0f, 2.0f);
		feedback = std::clamp(arg_float(args, 3), -0.99f, 0.99f);
		lfo.configure(args);
		return true;
	}
	void process(float* frames, int count) override {
		float rate_hz = float(get_sample_rate());
		for (int start = 0; start < count; start += control_frames) {
			int n = std::min(control_frames, count - start);
			float t = tanf(pi * lfo.advance(n) / rate_hz), a = (t - 1) / (t + 1);
			for (int f = start; f < start + n; f++) {
				for (int c = 0; c < 2; c++) {
					float x = frames[f * 2 + c], y = x + feedback * last[c];
					for (int s = 0; s < stages; s++) {
						float out = a * y + z[c][s];
						z[c][s] = y - a * out;
						y = out;
					}
					last[c] = y;
					frames[f * 2 + c] = x * dry + y * wet;
				}
			}
		}
	}
};

// autowah: the same parameters as phaser, sweeping a resonant bandpass filter instead.
class autowah_effect : public effect {
	static const int control_frames = 16;
	static constexpr float q = 4; // How sharply the filter peaks; the BASS parameters don't set one.
	float dry = 1, wet = 0, feedback = 0;
	sweep lfo;
	float ic1[2] = {0, 0}, ic2[2] = {0, 0}, last[2] = {0, 0};
public:
	const char* type() const override { return "autowah"; }
	bool configure(const std::vector<std::string>& args) override {
		dry = std::clamp(arg_float(args, 1), -2.0f, 2.0f);
		wet = std::clamp(arg_float(args, 2), -2.0f, 2.0f);
		feedback = std::clamp(arg_float(args, 3), -0.99f, 0.99f);
		lfo.configure(args);
		return true;
	}
	void process(float* frames, int count) override {
		float rate_hz = float(get_sample_rate()), k = 1 / q;
		for (int start = 0; start < count; start += control_frames) {
			int n = std::min(control_frames, count - start);
			// A trapezoidal state variable filter, which stays stable while its frequency moves.
			float g = tanf(pi * lfo.advance(n) / rate_hz), a1 = 1 / (1 + g * (g + k)), a2 = g * a1, a3 = g * a2;
			for (int f = start; f < start + n; f++) {
				for (int c = 0; c < 2; c++) {
					float x = frames[f * 2 + c], in = x + feedback * last[c];
					float v3 = in - ic2[c], v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
					ic1[c] = 2 * v1 - ic1[c];
					ic2[c] = 2 * v2 - ic2[c];
					float y = k * v1; // Bandpass scaled to 0 dB at its peak.
					last[c] = y;
					frames[f * 2 + c] = x * dry + y * wet;
				}
			}
		}
	}
};

// distortion: drive (0 to 5), dry mix and wet mix (each -5 to 5), feedback (-1 to 1), and output volume (0 to 2). The wet path is a soft clipper that drive pushes harder.
class distortion_effect : public effect {
	float drive = 0, dry = 1, wet = 0, feedback = 0, volume = 1;
	float last[2] = {0, 0};
public:
	const char* type() const override { return "distortion"; }
	bool configure(const std::vector<std::string>& args) override {
		drive = std::clamp(arg_float(args, 1), 0.0f, 5.0f);
		dry = std::clamp(arg_float(args, 2), -5.0f, 5.0f);
		wet = std::clamp(arg_float(args, 3), -5.0f, 5.0f);
		feedback = std::clamp(arg_float(args, 4), -0.99f, 0.99f);
		volume = std::clamp(arg_float(args, 5), 0.0f, 2.0f);
		return true;
	}
	void process(float* frames, int count) override {
		for (int i = 0; i < count * 2; i++) {
			int c = i & 1;
			float x = frames[i], d = x + feedback * last[c];
			d = d * (1 + drive) / (1 + drive * fabsf(d)); // Unchanged at drive 0, and never past 1 + 1/drive however hot the input.
			last[c] = d;
			frames[i] = (x * dry + d * wet) * volume;
		}
	}
};

// compressor2: output gain in dB, threshold in dB (-60 to 0), ratio (1 and up), attack and release in milliseconds. Both channels are compressed together so the image doesn't shift.
class compressor_effect : public effect {
	float makeup = 1, threshold = 0, slope = 0, attack = 0, release = 0;
	float reduction = 0; // dB of gain currently taken off.
public:
	const char* type() const override { return "compressor2"; }
	bool configure(const std::vector<std::string>& args) override {
		float rate_hz = float(get_sample_rate());
		makeup = db_gain(std::clamp(arg_float(args, 1), -60.0f, 60.0f));
		threshold = std::clamp(arg_float(args, 2), -60.0f, 0.0f);
		slope = 1 - 1 / std::max(arg_float(args, 3), 1.0f);
		attack = expf(-1 / (std::clamp(arg_float(args, 4), 0.01f, 1000.0f) * 0.001f * rate_hz));
		release = expf(-1 / (std::clamp(arg_float(args, 5), 0.01f, 5000.0f) * 0.001f * rate_hz));
		return true;
	}
	void process(float* frames, int count) override {
		for (int f = 0; f < count; f++) {
			float peak = std::max(fabsf(frames[f * 2]), fabsf(frames[f * 2 + 1]));
			float over = peak > 1e-6f ? 20 * log10f(peak) - threshold : -1;
			float target = over > 0 ? over * slope : 0;
			float coeff = target > reduction ? attack : release;
			reduction = target + coeff * (reduction - target);
			float g = makeup * db_gain(-reduction);
			frames[f * 2] *= g;
			frames[f * 2 + 1] *= g;
		}
	}
};

// pitchshift: a pitch factor (0.5 is an octave down, 2 an octave up), then optionally semitones on top. The length is unchanged. A phase vocoder with a 2048 point window, the size the legacy engine asked BASS for, overlapped 8 times: BASS was asked for 16, but locking phases around each peak (below) gets the same clarity from half the work. It delays the sound by 1792 frames.
class pitchshift_effect : public effect {
	static const int size = 2048, oversample = 8, hop = size / oversample, bins = size / 2 + 1, latency = size - hop;
	float ratio = 1;
	int rover = latency;
	std::vector<unsigned int> rev;
	std::vector<float> cos_t, sin_t, window;
	struct channel {
		std::vector<float> in, out, accum, last_phase, sum_phase;
		std::vector<float> spec_re, spec_im, mag, phase, freq, syn_mag, syn_phase; // This hop's spectrum, from 0 Hz to Nyquist.
	} ch[2];
	std::vector<float> re, im;
	std::vector<int> peaks;
	void fft(bool inverse) {
		for (int i = 1; i < size; i++) {
			int j = rev[i];
			if (i < j) {
				std::swap(re[i], re[j]);
				std::swap(im[i], im[j]);
			}
		}
		float sign = inverse ? 1.0f : -1.0f;
		for (int len = 2; len <= size; len <<= 1) {
			int half = len >> 1, step = size / len;
			for (int k = 0; k < half; k++) {
				float wr = cos_t[k * step], wi = sign * sin_t[k * step];
				for (int a = k; a < size; a += len) {
					int b = a + half;
					float vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
					re[b] = re[a] - vr;
					im[b] = im[a] - vi;
					re[a] += vr;
					im[a] += vi;
				}
			}
		}
	}
	// One hop for both channels. Two real signals share each complex transform, the left as its real part and the right as its imaginary part, which halves the transforms needed.
	void process_hop() {
		bool silent = true;
		for (int i = 0; i < size; i++) {
			re[i] = ch[0].in[i] * window[i];
			im[i] = ch[1].in[i] * window[i];
			if (re[i] != 0 || im[i] != 0) silent = false;
		}
		if (silent) { // Nothing in the window, so nothing to add: just move the output on.
			for (channel& h : ch) next_hop(h);
			return;
		}
		fft(false);
		for (int k = 0; k < bins; k++) {
			int n = (size - k) & (size - 1);
			ch[0].spec_re[k] = (re[k] + re[n]) * 0.5f;
			ch[0].spec_im[k] = (im[k] - im[n]) * 0.5f;
			ch[1].spec_re[k] = (im[k] + im[n]) * 0.5f;
			ch[1].spec_im[k] = (re[n] - re[k]) * 0.5f;
		}
		shift(ch[0]);
		shift(ch[1]);
		// Rebuild both from their positive frequencies, mirroring each into the negative ones so it comes back real.
		for (int k = 0; k < bins; k++) {
			float ar = ch[0].spec_re[k], ai = ch[0].spec_im[k], br = ch[1].spec_re[k], bi = ch[1].spec_im[k];
			if (k == 0 || k == size / 2) ai = bi = 0;
			re[k] = ar - bi;
			im[k] = ai + br;
			if (k > 0 && k < size / 2) {
				re[size - k] = ar + bi;
				im[size - k] = br - ai;
			}
		}
		fft(true);
		// Undoes the gain of the transforms and of the analysis and synthesis windows, which overlap oversample times to 0.375 * oversample, so an unshifted partial comes back at its original level.
		float scale = 1.0f / (size * 0.375f * oversample);
		for (int c = 0; c < 2; c++) {
			channel& h = ch[c];
			const std::vector<float>& out = c == 0 ? re : im;
			for (int i = 0; i < size; i++) h.accum[i] += window[i] * out[i] * scale;
			next_hop(h);
		}
	}
	// Hands the finished hop of output over to be played, and slides both buffers along by a hop.
	void next_hop(channel& h) {
		for (int i = 0; i < hop; i++) h.out[i] = h.accum[i];
		std::memmove(h.accum.data(), h.accum.data() + hop, (size_t(size) * 2 - hop) * sizeof(float));
		std::fill(h.accum.end() - hop, h.accum.end(), 0.0f);
		std::memmove(h.in.data(), h.in.data() + hop, size_t(latency) * sizeof(float));
	}
	// Moves every partial in a channel's spectrum to ratio times its frequency, leaving the new spectrum in spec_re and spec_im.
	void shift(channel& c) {
		const double expected = 2 * 3.14159265358979 / oversample, bin_hz = 1.0 / size; // Phase each bin advances per hop, and bin spacing as a fraction of the sample rate.
		std::vector<float>&mag = c.mag, &phase = c.phase, &freq = c.freq, &syn_mag = c.syn_mag, &syn_phase = c.syn_phase;
		for (int k = 0; k < bins; k++) {
			phase[k] = atan2f(c.spec_im[k], c.spec_re[k]);
			double delta = double(phase[k]) - c.last_phase[k] - k * expected;
			c.last_phase[k] = phase[k];
			delta -= 2 * 3.14159265358979 * floor(delta / (2 * 3.14159265358979) + 0.5); // Wrap to -pi..pi.
			mag[k] = sqrtf(c.spec_re[k] * c.spec_re[k] + c.spec_im[k] * c.spec_im[k]);
			freq[k] = float((k + delta / expected) * bin_hz); // The partial's true frequency, as a fraction of the sample rate.
		}
		// Each partial shows up as a peak a few bins wide. Its whole region of bins is moved, unchanged in shape, to ratio times the peak's frequency, and the bins around it keep their phase relative to the peak (identity phase locking, after Laroche and Dolson). Moving or stretching bins one by one instead loses the peak's shape and much of its level.
		peaks.clear();
		float floor_mag = 0;
		for (int k = 0; k < bins; k++) floor_mag = std::max(floor_mag, mag[k]);
		floor_mag *= 1e-5f; // About -100 dB under the loudest partial: anything quieter is left out.
		for (int k = 1; k < bins - 1; k++)
			if (mag[k] > floor_mag && mag[k] > mag[k - 1] && mag[k] >= mag[k + 1]) peaks.push_back(k);
		std::fill(syn_mag.begin(), syn_mag.end(), 0.0f);
		for (size_t p = 0; p < peaks.size(); p++) {
			int k = peaks[p];
			// The region runs to the quietest bin between this peak and each neighbour.
			int lo = p == 0 ? 0 : peaks[p - 1], hi = p + 1 == peaks.size() ? bins - 1 : peaks[p + 1];
			lo = int(std::min_element(mag.begin() + lo, mag.begin() + k) - mag.begin());
			hi = int(std::min_element(mag.begin() + k + 1, mag.begin() + hi + 1) - mag.begin());
			int to = int(lrintf(freq[k] * size * ratio));
			if (to < 0 || to >= bins) continue;
			// The peak's phase carries on from where that bin's left off, moving at the new frequency.
			float peak_phase = float(fmod(c.sum_phase[to] + 2 * 3.14159265358979 * freq[k] * ratio * hop, 2 * 3.14159265358979));
			for (int b = lo; b <= hi; b++) {
				int j = b + to - k;
				if (j < 0 || j >= bins || mag[b] <= syn_mag[j]) continue; // Where shifted regions overlap the louder bin wins.
				syn_mag[j] = mag[b];
				syn_phase[j] = peak_phase + (phase[b] - phase[k]);
			}
		}
		for (int k = 0; k < bins; k++) {
			if (syn_mag[k] > 0) {
				c.sum_phase[k] = syn_phase[k];
				c.spec_re[k] = syn_mag[k] * cosf(syn_phase[k]);
				c.spec_im[k] = syn_mag[k] * sinf(syn_phase[k]);
			} else c.sum_phase[k] = c.spec_re[k] = c.spec_im[k] = 0;
		}
	}
public:
	pitchshift_effect() {
		rev.resize(size);
		int bits = 0;
		while ((1 << bits) < size) bits++;
		for (int i = 0; i < size; i++) {
			int r = 0;
			for (int b = 0; b < bits; b++) r |= ((i >> b) & 1) << (bits - 1 - b);
			rev[i] = r;
		}
		cos_t.resize(size / 2);
		sin_t.resize(size / 2);
		for (int i = 0; i < size / 2; i++) {
			cos_t[i] = float(cos(2 * 3.14159265358979 * i / size));
			sin_t[i] = float(sin(2 * 3.14159265358979 * i / size));
		}
		window.resize(size);
		for (int i = 0; i < size; i++) window[i] = float(0.5 - 0.5 * cos(2 * 3.14159265358979 * i / size));
		for (channel& c : ch) {
			c.in.assign(size, 0.0f);
			c.out.assign(hop, 0.0f);
			c.accum.assign(size_t(size) * 2, 0.0f);
			for (std::vector<float>* v : {&c.last_phase, &c.sum_phase, &c.spec_re, &c.spec_im, &c.mag, &c.phase, &c.freq, &c.syn_mag, &c.syn_phase}) v->assign(bins, 0.0f);
		}
		re.resize(size);
		im.resize(size);
		peaks.reserve(bins);
	}
	const char* type() const override { return "pitchshift"; }
	bool configure(const std::vector<std::string>& args) override {
		float factor = arg_float(args, 1, 1.0f);
		if (factor <= 0) factor = 1;
		ratio = std::clamp(factor * powf(2.0f, arg_float(args, 2) / 12.0f), 0.25f, 4.0f);
		return true;
	}
	void process(float* frames, int count) override {
		for (int f = 0; f < count; f++) {
			for (int c = 0; c < 2; c++) {
				ch[c].in[rover] = frames[f * 2 + c];
				frames[f * 2 + c] = ch[c].out[rover - latency];
			}
			if (++rover == size) {
				rover = latency;
				process_hop();
			}
		}
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
	else if (name == "echo4" && params >= 5) e = std::make_unique<echo_effect>();
	else if (name == "chorus" && params >= 6) e = std::make_unique<chorus_effect>();
	else if (name == "phaser" && params >= 6) e = std::make_unique<phaser_effect>();
	else if (name == "autowah" && params >= 6) e = std::make_unique<autowah_effect>();
	else if (name == "distortion" && params >= 5) e = std::make_unique<distortion_effect>();
	else if (name == "compressor2" && params >= 5) e = std::make_unique<compressor_effect>();
	else if (name == "pitchshift" && params >= 1) e = std::make_unique<pitchshift_effect>();
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
