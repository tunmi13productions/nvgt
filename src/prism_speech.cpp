/* prism_speech.cpp - speech through Prism (https://github.com/ethindp/prism): the screen_reader_* functions, and a tts_voice engine for each speech synthesizer Prism offers.
 * Built where NVGT_USE_PRISM is defined, which replaces the platform's own screen reader and text to speech code.
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

#ifdef NVGT_USE_PRISM
#include <prism.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "logging.h"
#include "tts.h"
#include "UI.h" // wait
#include "xplatform.h"

// The Prism context. Engines inside tts_voice objects keep a reference, since a script can hold a voice past screen_reader_unload, and Prism must not be shut down while any backend is alive.
struct prism_context {
	PrismContext* ctx = nullptr;
	~prism_context() {
		if (ctx) prism_shutdown(ctx);
	}
};
static std::mutex g_prism_mutex;
static std::shared_ptr<prism_context> g_prism;
static std::atomic<bool> g_screen_reader_stale{true}; // Set whenever a backend becomes available or unavailable, so the next screen reader call chooses again.
static void PRISM_CALL on_availability_change(void*, PrismBackendId, const char*, bool) { g_screen_reader_stale = true; }
static std::shared_ptr<prism_context> get_prism() {
	std::lock_guard<std::mutex> l(g_prism_mutex);
	if (g_prism) return g_prism;
	#ifdef _WIN32
	// prism.dll is delay loaded, so calling into it when it's missing would crash rather than fail. Loading it here first finds it the same way the delay load will.
	static bool loadable = LoadLibraryW(L"prism.dll") != nullptr;
	if (!loadable) {
		NVGT_ERROR("nvgt", "prism.dll could not be loaded, so there is no speech");
		return nullptr;
	}
	#endif
	PrismConfig cfg = prism_config_init();
	cfg.availability_callback = on_availability_change;
	cfg.availability_backoff_max_ms = 4000; // Checks every second while things change, settling to every 4 seconds while they don't.
	auto p = std::make_shared<prism_context>();
	p->ctx = prism_init(&cfg);
	if (!p->ctx) {
		NVGT_ERROR("nvgt", "could not start Prism, so there is no speech");
		return nullptr;
	}
	g_prism = p;
	return p;
}
// Synthesizers speak in their own voice and become tts_voice engines; screen readers speak through the user's, and are what the screen_reader_* functions use, as NVGT always has.
static bool is_synthesizer(PrismBackendId id) {
	return id == PRISM_BACKEND_SAPI || id == PRISM_BACKEND_ONE_CORE || id == PRISM_BACKEND_AV_SPEECH || id == PRISM_BACKEND_ANDROID_TTS || id == PRISM_BACKEND_WEB_SPEECH || id == PRISM_BACKEND_SPIEL || id == PRISM_BACKEND_SPEECH_DISPATCHER;
}
static bool is_screen_reader(PrismBackendId id) {
	#ifdef __linux__
	if (id == PRISM_BACKEND_SPEECH_DISPATCHER) return true; // Where Orca's remote control isn't available, Speech Dispatcher is how a Linux screen reader user's own speech is reached, as it was before Prism.
	#endif
	return !is_synthesizer(id);
}

// Screen readers.
static std::mutex g_sr_mutex; // A Prism backend must only be used by one thread at a time.
static PrismBackend* g_sr = nullptr;
static uint64_t g_sr_features = 0;
// Must be called with g_sr_mutex held. Finds the highest priority screen reader that is running.
static PrismBackend* screen_reader() {
	if (!g_screen_reader_stale) return g_sr;
	g_screen_reader_stale = false;
	if (g_sr) prism_backend_free(g_sr);
	g_sr = nullptr;
	g_sr_features = 0;
	std::shared_ptr<prism_context> p = get_prism();
	if (!p) return nullptr;
	std::vector<PrismBackendId> ids;
	for (size_t i = 0, n = prism_registry_count(p->ctx); i < n; i++) {
		PrismBackendId id = prism_registry_id_at(p->ctx, i);
		if (is_screen_reader(id)) ids.push_back(id);
	}
	std::stable_sort(ids.begin(), ids.end(), [&](PrismBackendId a, PrismBackendId b) { return prism_registry_priority(p->ctx, a) > prism_registry_priority(p->ctx, b); });
	for (PrismBackendId id : ids) {
		PrismBackend* b = prism_registry_create(p->ctx, id);
		if (!b) continue;
		uint64_t features = prism_backend_get_features(b);
		if ((features & PRISM_BACKEND_IS_SUPPORTED_AT_RUNTIME) && prism_backend_initialize(b) == PRISM_OK) {
			g_sr = b;
			g_sr_features = prism_backend_get_features(b);
			break;
		}
		prism_backend_free(b);
	}
	return g_sr;
}
// Runs one call on the screen reader, choosing again next time if it has gone away.
template <class F> static bool with_screen_reader(F call) {
	std::lock_guard<std::mutex> l(g_sr_mutex);
	PrismBackend* b = screen_reader();
	if (!b) return false;
	PrismError e = call(b);
	if (e == PRISM_ERROR_BACKEND_NOT_AVAILABLE || e == PRISM_ERROR_NOT_INITIALIZED) g_screen_reader_stale = true;
	return e == PRISM_OK;
}
bool screen_reader_load() { return bool(get_prism()); }
void screen_reader_unload() {
	{
		std::lock_guard<std::mutex> l(g_sr_mutex);
		if (g_sr) prism_backend_free(g_sr);
		g_sr = nullptr;
		g_screen_reader_stale = true;
	}
	std::lock_guard<std::mutex> l(g_prism_mutex);
	g_prism.reset(); // Shuts Prism down unless a tts_voice still holds an engine.
}
std::string screen_reader_detect() {
	std::lock_guard<std::mutex> l(g_sr_mutex);
	PrismBackend* b = screen_reader();
	return b ? prism_backend_name(b) : "";
}
bool screen_reader_has_speech() {
	std::lock_guard<std::mutex> l(g_sr_mutex);
	return screen_reader() && (g_sr_features & PRISM_BACKEND_SUPPORTS_SPEAK);
}
bool screen_reader_has_braille() {
	std::lock_guard<std::mutex> l(g_sr_mutex);
	return screen_reader() && (g_sr_features & PRISM_BACKEND_SUPPORTS_BRAILLE);
}
bool screen_reader_is_speaking() {
	bool speaking = false;
	return with_screen_reader([&](PrismBackend* b) { return prism_backend_is_speaking(b, &speaking); }) && speaking;
}
bool screen_reader_output(const std::string& text, bool interrupt) {
	return with_screen_reader([&](PrismBackend* b) { return prism_backend_output(b, text.c_str(), interrupt); });
}
bool screen_reader_speak(const std::string& text, bool interrupt) {
	return with_screen_reader([&](PrismBackend* b) { return prism_backend_speak(b, text.c_str(), interrupt); });
}
bool screen_reader_braille(const std::string& text) {
	return with_screen_reader([&](PrismBackend* b) { return prism_backend_braille(b, text.c_str()); });
}
bool screen_reader_silence() {
	return with_screen_reader([](PrismBackend* b) { return prism_backend_stop(b); });
}

// A tts_voice engine for one of Prism's synthesizers. Prism takes rate, pitch and volume from 0 to 1, with 0.5 the default.
class prism_tts_engine : public tts_engine_impl {
	std::shared_ptr<prism_context> prism;
	PrismBackend* backend = nullptr;
	uint64_t features = 0;
	std::mutex m;
	struct capture {
		std::vector<float> samples;
		size_t channels = 0, sample_rate = 0;
	};
	static void PRISM_CALL on_audio(void* userdata, const float* samples, size_t count, size_t channels, size_t sample_rate) {
		capture* c = (capture*)userdata;
		c->samples.insert(c->samples.end(), samples, samples + count);
		c->channels = channels;
		c->sample_rate = sample_rate;
	}
	float get(PrismError (*fn)(PrismBackend*, float*)) {
		std::lock_guard<std::mutex> l(m);
		float v = 0.5f;
		return backend && fn(backend, &v) == PRISM_OK ? v : 0.5f;
	}
	void set(PrismError (*fn)(PrismBackend*, float), float v) {
		std::lock_guard<std::mutex> l(m);
		if (backend) (void)fn(backend, std::clamp(v, 0.0f, 1.0f));
	}
public:
	prism_tts_engine(const std::string& name, std::shared_ptr<prism_context> p, PrismBackendId id) : tts_engine_impl(name), prism(p) {
		PrismBackend* b = prism_registry_create(p->ctx, id);
		if (!b) return;
		if (prism_backend_initialize(b) != PRISM_OK) {
			prism_backend_free(b);
			return;
		}
		backend = b;
		features = prism_backend_get_features(b);
	}
	~prism_tts_engine() {
		if (backend) prism_backend_free(backend);
	}
	bool is_available() override { return backend != nullptr; }
	tts_pcm_generation_state get_pcm_generation_state() override { return (features & PRISM_BACKEND_SUPPORTS_SPEAK_TO_MEMORY) ? PCM_PREFERRED : PCM_UNSUPPORTED; }
	// Prism hands back 32 bit float samples.
	tts_audio_data* speak_to_pcm(const std::string& text) override {
		if (text.empty() || !backend) return nullptr;
		capture c;
		{
			std::lock_guard<std::mutex> l(m);
			if (prism_backend_speak_to_memory(backend, text.c_str(), on_audio, &c) != PRISM_OK) return nullptr;
		}
		if (c.samples.empty() || !c.channels || !c.sample_rate) return nullptr;
		size_t bytes = c.samples.size() * sizeof(float);
		void* data = malloc(bytes);
		if (!data) return nullptr;
		memcpy(data, c.samples.data(), bytes);
		return new tts_audio_data(this, data, unsigned(bytes), unsigned(c.sample_rate), unsigned(c.channels), 32);
	}
	bool speak(const std::string& text, bool interrupt, bool blocking) override {
		{
			std::lock_guard<std::mutex> l(m);
			if (!backend || prism_backend_speak(backend, text.c_str(), interrupt) != PRISM_OK) return false;
		}
		while (blocking && is_speaking()) wait(10);
		return true;
	}
	bool is_speaking() override {
		std::lock_guard<std::mutex> l(m);
		bool speaking = false;
		return backend && prism_backend_is_speaking(backend, &speaking) == PRISM_OK && speaking;
	}
	bool stop() override {
		std::lock_guard<std::mutex> l(m);
		return backend && prism_backend_stop(backend) == PRISM_OK;
	}
	float get_rate() override { return get(prism_backend_get_rate); }
	float get_pitch() override { return get(prism_backend_get_pitch); }
	float get_volume() override { return get(prism_backend_get_volume); }
	void set_rate(float v) override { set(prism_backend_set_rate, v); }
	void set_pitch(float v) override { set(prism_backend_set_pitch, v); }
	void set_volume(float v) override { set(prism_backend_set_volume, v); }
	bool get_rate_range(float& minimum, float& midpoint, float& maximum) override { return range(PRISM_BACKEND_SUPPORTS_SET_RATE, minimum, midpoint, maximum); }
	bool get_pitch_range(float& minimum, float& midpoint, float& maximum) override { return range(PRISM_BACKEND_SUPPORTS_SET_PITCH, minimum, midpoint, maximum); }
	bool get_volume_range(float& minimum, float& midpoint, float& maximum) override { return range(PRISM_BACKEND_SUPPORTS_SET_VOLUME, minimum, midpoint, maximum); }
	bool range(uint64_t feature, float& minimum, float& midpoint, float& maximum) {
		minimum = 0;
		midpoint = 0.5f;
		maximum = 1;
		return (features & feature) != 0;
	}
	int get_voice_count() override {
		std::lock_guard<std::mutex> l(m);
		size_t n = 0;
		return backend && prism_backend_count_voices(backend, &n) == PRISM_OK ? int(n) : 0;
	}
	std::string get_voice_name(int index) override {
		std::lock_guard<std::mutex> l(m);
		const char* name = nullptr;
		return backend && index >= 0 && prism_backend_get_voice_name(backend, size_t(index), &name) == PRISM_OK && name ? name : "";
	}
	std::string get_voice_language(int index) override {
		std::lock_guard<std::mutex> l(m);
		const char* lang = nullptr;
		return backend && index >= 0 && prism_backend_get_voice_language(backend, size_t(index), &lang) == PRISM_OK && lang ? lang : "";
	}
	bool set_voice(int voice) override {
		std::lock_guard<std::mutex> l(m);
		return backend && voice >= 0 && prism_backend_set_voice(backend, size_t(voice)) == PRISM_OK;
	}
	int get_current_voice() override {
		std::lock_guard<std::mutex> l(m);
		size_t v = 0;
		return backend && prism_backend_get_voice(backend, &v) == PRISM_OK ? int(v) : -1;
	}
};

// Registers a tts_voice engine for every Prism synthesizer. SAPI keeps the name "sapi5" it had before Prism, so scripts asking for it by name still get it.
void register_native_tts() {
	std::shared_ptr<prism_context> p = get_prism();
	if (!p) return;
	for (size_t i = 0, n = prism_registry_count(p->ctx); i < n; i++) {
		PrismBackendId id = prism_registry_id_at(p->ctx, i);
		if (!is_synthesizer(id)) continue;
		const char* raw_name = prism_registry_name(p->ctx, id);
		std::string name = id == PRISM_BACKEND_SAPI ? "sapi5" : (raw_name ? raw_name : "prism");
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
		tts_engine_register(name, [name, id]() -> std::shared_ptr<tts_engine> {
			std::shared_ptr<prism_context> p = get_prism();
			if (!p) return nullptr;
			return std::make_shared<prism_tts_engine>(name, p, id);
		});
	}
}

#endif // NVGT_USE_PRISM
