/* nvsound_microphone.cpp - audio capture for NVGT's sound engine, built on SDL3 recording streams.
 * Keeps the polled interface scripts already use for voice chat: read, available_read, sample_rate, channels, reset, set_device, active.
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
#include <algorithm>
#include <cstring>
#include <angelscript.h>
#include <scriptarray.h>
#include <SDL3/SDL.h>
#include "nvgt.h" // g_ScriptEngine

namespace nvsound {

class microphone;
// Every live microphone, so shutdown can close their streams before SDL_Quit frees them.
static std::mutex g_microphones_mutex;
static std::vector<microphone*> g_microphones;

class microphone {
	std::atomic<int> ref_count{1};
	SDL_AudioStream* stream = nullptr;
	int device;
	unsigned int requested_rate, requested_channels;
	unsigned int channels = 0, sample_rate = 0;
	float volume = 1.0f;
	// Captured audio waits here until the script reads it. It holds two seconds and drops the oldest audio when full, so a script that stops reading neither grows memory nor hears stale audio when it starts again.
	std::mutex ring_mutex;
	std::vector<float> ring;
	size_t capacity = 0, rpos = 0, fill = 0; // In frames.
	static void SDLCALL on_audio(void* ud, SDL_AudioStream* s, int additional, int) {
		microphone* m = (microphone*)ud;
		if (additional <= 0) return;
		thread_local std::vector<float> buf;
		buf.resize(size_t(additional) / sizeof(float));
		int got = SDL_GetAudioStreamData(s, buf.data(), additional);
		if (got > 0) m->write(buf.data(), size_t(got) / sizeof(float) / m->channels);
	}
	void write(const float* in, size_t frames) {
		std::lock_guard<std::mutex> l(ring_mutex);
		if (frames > capacity) {
			in += (frames - capacity) * channels;
			frames = capacity;
		}
		if (frames > capacity - fill) {
			size_t drop = frames - (capacity - fill);
			rpos = (rpos + drop) % capacity;
			fill -= drop;
		}
		size_t wpos = (rpos + fill) % capacity;
		for (size_t done = 0; done < frames;) {
			size_t n = std::min(frames - done, capacity - wpos);
			memcpy(ring.data() + wpos * channels, in + done * channels, n * channels * sizeof(float));
			wpos = (wpos + n) % capacity;
			done += n;
		}
		fill += frames;
	}
public:
	void close() {
		// Not under ring_mutex: destroying the stream waits for on_audio, which takes it.
		if (stream) SDL_DestroyAudioStream(stream);
		stream = nullptr;
	}
	microphone(int device, unsigned int sample_rate, unsigned int channels) : device(device), requested_rate(sample_rate), requested_channels(channels) {
		std::lock_guard<std::mutex> l(g_microphones_mutex);
		g_microphones.push_back(this);
	}
	~microphone() {
		{
			std::lock_guard<std::mutex> l(g_microphones_mutex);
			g_microphones.erase(std::remove(g_microphones.begin(), g_microphones.end(), this), g_microphones.end());
		}
		close();
	}
	void add_ref() { ref_count++; }
	void release() {
		if (--ref_count == 0) delete this;
	}
	bool open() {
		close();
		if (!init()) return false;
		SDL_AudioDeviceID dev = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
		unsigned int index = device >= 0 ? unsigned(device) : get_input_device();
		if (index != 0xffffffff) {
			int count = 0;
			SDL_AudioDeviceID* devs = SDL_GetAudioRecordingDevices(&count);
			if (devs && int(index) < count) dev = devs[index];
			SDL_free(devs);
			if (dev == SDL_AUDIO_DEVICE_DEFAULT_RECORDING && device >= 0) return false;
		}
		// Unless the script asks for a format, capture in the device's own so SDL doesn't convert. Asking for one (say 48000 mono for opus) makes SDL convert for us.
		SDL_AudioSpec native{SDL_AUDIO_F32, 1, 48000};
		SDL_GetAudioDeviceFormat(dev, &native, nullptr);
		SDL_AudioSpec spec{SDL_AUDIO_F32, int(requested_channels ? requested_channels : std::clamp(native.channels, 1, 2)), int(requested_rate ? requested_rate : (native.freq > 0 ? native.freq : 48000))};
		{
			std::lock_guard<std::mutex> l(ring_mutex);
			channels = spec.channels;
			sample_rate = spec.freq;
			capacity = size_t(sample_rate) * 2;
			ring.assign(capacity * channels, 0.0f);
			rpos = fill = 0;
		}
		stream = SDL_OpenAudioDeviceStream(dev, &spec, on_audio, this);
		if (!stream) return false;
		SDL_SetAudioStreamGain(stream, volume);
		SDL_ResumeAudioStreamDevice(stream);
		return true;
	}
	CScriptArray* read(unsigned int frames) {
		std::lock_guard<std::mutex> l(ring_mutex);
		size_t n = frames == 0 ? fill : std::min<size_t>(frames, fill);
		CScriptArray* a = CScriptArray::Create(g_ScriptEngine->GetTypeInfoByDecl("array<float>"), asUINT(n * channels));
		float* out = n ? (float*)a->At(0) : nullptr;
		for (size_t done = 0; done < n;) {
			size_t c = std::min(n - done, capacity - rpos);
			memcpy(out + done * channels, ring.data() + rpos * channels, c * channels * sizeof(float));
			rpos = (rpos + c) % capacity;
			done += c;
		}
		fill -= n;
		return a;
	}
	unsigned int get_available_read() {
		std::lock_guard<std::mutex> l(ring_mutex);
		return unsigned(fill);
	}
	void reset() {
		if (stream) SDL_ClearAudioStream(stream);
		std::lock_guard<std::mutex> l(ring_mutex);
		rpos = fill = 0;
	}
	unsigned int get_channels() const { return channels; }
	unsigned int get_sample_rate() const { return sample_rate; }
	bool get_active() const { return stream && !SDL_AudioDevicePaused(SDL_GetAudioStreamDevice(stream)); }
	bool set_device(int d) {
		int old = device;
		device = d;
		if (open()) return true;
		device = old;
		open();
		return false;
	}
	int get_device() const { return device; }
	void set_volume(float v) {
		volume = std::max(v, 0.0f);
		if (stream) SDL_SetAudioStreamGain(stream, volume);
	}
	float get_volume() const { return volume; }
	bool start() { return stream && SDL_ResumeAudioStreamDevice(stream); }
	bool stop() { return stream && SDL_PauseAudioStreamDevice(stream); }
};

void close_all_microphones() {
	std::lock_guard<std::mutex> l(g_microphones_mutex);
	for (microphone* m : g_microphones) m->close();
}

static microphone* script_microphone_factory(int device, unsigned int sample_rate, unsigned int channels) {
	microphone* m = new microphone(device, sample_rate, channels);
	if (!m->open()) {
		m->release();
		if (asIScriptContext* ctx = asGetActiveContext()) ctx->SetException("unable to open microphone");
		return nullptr;
	}
	return m;
}

void RegisterNvsoundMicrophone(asIScriptEngine* engine) {
	engine->RegisterObjectType("microphone", 0, asOBJ_REF);
	engine->RegisterObjectBehaviour("microphone", asBEHAVE_FACTORY, "microphone@ m(int device = -1, uint sample_rate = 0, uint channels = 0)", asFUNCTION(script_microphone_factory), asCALL_CDECL);
	engine->RegisterObjectBehaviour("microphone", asBEHAVE_ADDREF, "void f()", asMETHOD(microphone, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("microphone", asBEHAVE_RELEASE, "void f()", asMETHOD(microphone, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "float[]@ read(uint frame_count = 0)", asMETHOD(microphone, read), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "uint get_available_read() const property", asMETHOD(microphone, get_available_read), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "void reset()", asMETHOD(microphone, reset), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "uint get_channels() const property", asMETHOD(microphone, get_channels), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "uint get_sample_rate() const property", asMETHOD(microphone, get_sample_rate), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "bool get_active() const property", asMETHOD(microphone, get_active), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "bool set_device(int device)", asMETHOD(microphone, set_device), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "int get_device() const property", asMETHOD(microphone, get_device), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "void set_volume(float volume) property", asMETHOD(microphone, set_volume), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "float get_volume() const property", asMETHOD(microphone, get_volume), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "bool start()", asMETHOD(microphone, start), asCALL_THISCALL);
	engine->RegisterObjectMethod("microphone", "bool stop()", asMETHOD(microphone, stop), asCALL_THISCALL);
}

} // namespace nvsound
