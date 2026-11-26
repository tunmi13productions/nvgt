/* nvgt_adapter.cpp - NVGT's side of the new nvsound core: its host (packs, decryption, URL streams, wait, logging and NVGT's clock), the loaders that need AngelScript, and the script registration, the same as today's.
 * Built into NVGT with nvsound_core=1 in place of nvsound.cpp. Golden Crayon's Python module has its own adapter and doesn't build this.
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
#include "realtime.h"
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <angelscript.h>
#include <reactphysics3d/mathematics/Vector3.h>
#include <scriptarray.h>
#include <Poco/Format.h>
#include <Poco/URIStreamOpener.h>
#include "../datastreams.h" // prebuffer_istream
#include "../legacy_pack.h"
#include "../legacy_pack_config.h" // sound_data_char_decrypt
#include "../logging.h"
#include "../nvgt.h" // g_ScriptEngine
#include "../sound_service.h"
#include "../timestuff.h" // microticks, set_virtual_clock
#include "../UI.h" // wait

#ifdef NVSOUND_COUNT_ALLOCS
// A checking build (nvsound_count_allocs=1): every allocation goes through here, so one made while rendering is counted. Set NVSOUND_REPORT to print the counts at exit.
#include <cstdlib>
#include <new>
void* operator new(size_t n) {
	if (nvsound::on_audio_thread()) nvsound::note_audio_thread_alloc();
	if (void* p = std::malloc(n ? n : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
#endif

namespace nvsound {

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

struct nvgt_host : host {
	std::string cache_key(const std::string& filename, const pack_interface* pack) override {
		if (!init_service()) return "";
		bool from_pack = pack && pack->get_is_active();
		directive_t dir = from_pack ? directive_t(std::shared_ptr<const pack_interface>(pack->make_immutable())) : nullptr;
		return g_service->prepare_triplet(filename, from_pack ? g_pack_slot : sound_service::fs_protocol_slot, dir);
	}
	std::unique_ptr<std::istream> open(const std::string& key) override { return std::unique_ptr<std::istream>(g_service->open_triplet(key.c_str())); }
	void done(const std::string& key) override { g_service->cleanup_triplet(key); }
	const pack_interface* default_pack() override { return g_default_pack.get(); }
	std::unique_ptr<std::istream> open_url(const std::string& url, std::function<void()>& abort) override {
		std::istream* source = nullptr;
		try {
			source = Poco::URIStreamOpener::defaultOpener().open(url);
		} catch (std::exception& e) {
			NVGT_ERROR("nvgt.sound", Poco::format("could not connect to %s: %s", url, std::string(e.what())));
		}
		if (!source) return nullptr;
		std::unique_ptr<prebuffer_istream> stream;
		try {
			stream = std::make_unique<prebuffer_istream>(*source);
		} catch (std::exception&) {
			delete source;
			return nullptr;
		}
		stream->own_source();
		std::shared_ptr<prebuffer_ring> ring = stream->get_ring();
		// Any data at all means the server is talking; then give format detection a head start so it rarely waits on the network.
		if (!ring->wait_for(1, NETSTREAM_STALL_MS) || !ring->wait_for(32 * 1024, 3000)) {
			NVGT_ERROR("nvgt.sound", Poco::format("could not stream %s: the server sent nothing", url));
			return nullptr;
		}
		abort = [ring] { ring->abort(); };
		return stream;
	}
	void legacy_decrypt(std::string& bytes) override {
		unsigned n = unsigned(bytes.size());
		for (unsigned i = 0; i < n; i++) bytes[i] = char(sound_data_char_decrypt((unsigned char)bytes[i], i, n));
	}
	void wait(int ms) override { ::wait(ms); }
	void log_error(const std::string& message) override { NVGT_ERROR("nvgt.sound", message); }
	int64_t now_micros() override { return int64_t(microticks(false)); }
	void set_clock(int64_t micros) override { set_virtual_clock(micros); }
};

// The legacy engine let BASS call these from its own threads, which isn't safe for script code. They are called here, on the script's thread, to read the whole file up front.
static bool script_load_callbacks(sound* s, asIScriptFunction* close_cb, asIScriptFunction* length_cb, asIScriptFunction* read_cb, asIScriptFunction* seek_cb, const std::string& data, const std::string& preload_filename) {
	struct releaser {
		asIScriptFunction* f[4];
		~releaser() {
			for (asIScriptFunction* x : f) {
				if (x) x->Release();
			}
		}
	} release_callbacks{{close_cb, length_cb, read_cb, seek_cb}};
	if (!init()) return false;
	s->close();
	std::string key = "\x1ememory\x1e" + preload_filename, name = preload_filename.empty() ? "script_stream" : preload_filename;
	if (!preload_filename.empty() && s->attach_cached_file(key, name)) return true;
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
	return s->load_encoded(bytes, key, name, !preload_filename.empty());
}
// A plain array handle parameter hands its reference to the callee.
static bool script_push_floats(sound* s, CScriptArray* data, bool end_stream, int pcm_rate, int pcm_channels) {
	size_t n = data ? data->GetSize() : 0;
	bool r = s->push_float_samples(n ? (const float*)data->At(0) : nullptr, n, end_stream, pcm_rate, pcm_channels);
	if (data) data->Release();
	return r;
}
static sound* script_sound_play(const std::string& path, const reactphysics3d::Vector3& position, float volume, float pan, float pitch, mixer* mix, const pack_interface* pack, bool autoplay) {
	bool positioned = position.x != FLT_MAX || position.y != FLT_MAX || position.z != FLT_MAX;
	return sound_play(path, position.x, position.y, position.z, positioned, volume, pan, pitch, mix, pack, autoplay);
}
// How far a URL stream reads ahead of what is playing. The gauge is for the most recently active one.
static void set_netstream_buffer_size(unsigned int bytes) { g_netstream_buffer_size = bytes; }
static unsigned int get_netstream_buffer_size() { return unsigned(g_netstream_buffer_size); }
static unsigned int get_netstream_buffered() { return unsigned(g_netstream_buffered.load()); }

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
	set_host(std::make_unique<nvgt_host>());
#ifdef NVSOUND_COUNT_ALLOCS
	// Proves the counter sees allocations in this build: one made on purpose while marked as the audio thread must be counted.
	{
		size_t before = audio_thread_alloc_count();
		{
			audio_scope scope;
			delete new int(1);
		}
		bool works = audio_thread_alloc_count() == before + 1;
		reset_audio_thread_counts();
		if (getenv("NVSOUND_REPORT")) fprintf(stderr, "nvsound allocation counter %s\n", works ? "works" : "DOES NOT WORK");
	}
#endif
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
	engine->RegisterObjectMethod("sound", "bool load(sound_close_callback@ close_cb, sound_length_callback@ length_cb, sound_read_callback@ read_cb, sound_seek_callback@ seek_cb, const string&in user_data, const string&in preload_filename = \"\")", asFUNCTION(script_load_callbacks), asCALL_CDECL_OBJFIRST);
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
	engine->RegisterGlobalFunction("bool sound_offline_begin(int sample_rate = 48000)", asFUNCTION(offline_begin), asCALL_CDECL);
	engine->RegisterGlobalFunction("bool sound_offline_record(const string&in path)", asFUNCTION(offline_record), asCALL_CDECL);
	engine->RegisterGlobalFunction("bool sound_offline_render(double ms)", asFUNCTION(offline_render), asCALL_CDECL);
	engine->RegisterGlobalFunction("bool sound_offline_stop()", asFUNCTION(offline_stop), asCALL_CDECL);
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
	engine->RegisterGlobalProperty("mixer@ sound_default_mixer", default_mixer_slot());
	engine->RegisterGlobalFunction(("sound@ sound_play(const string&in path, const vector&in position = vector(FLOAT_MAX, FLOAT_MAX, FLOAT_MAX), float volume = 0.0, float pan = 0.0, float pitch = 100.0, mixer@+ mix = null, const pack_interface@+ pack_file = " + default_pack + ", bool autoplay = true)").c_str(), asFUNCTION(script_sound_play), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_sound_netstream_buffer_size(uint bytes) property", asFUNCTION(set_netstream_buffer_size), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_netstream_buffer_size() property", asFUNCTION(get_netstream_buffer_size), asCALL_CDECL);
	engine->RegisterGlobalFunction("uint get_sound_netstream_buffered() property", asFUNCTION(get_netstream_buffered), asCALL_CDECL);
#ifdef NVSOUND_COUNT_ALLOCS
	// For the checking build's tests only.
	engine->RegisterGlobalFunction("void sound_simulate_device_loss()", asFUNCTION(simulate_output_device_loss), asCALL_CDECL);
	engine->RegisterGlobalFunction("int64 sound_live_nodes()", asFUNCTION(live_node_count), asCALL_CDECL);
#endif
}

// Item 3 brings sound_environment back. Golden Crayon doesn't use it.
void RegisterNvsoundEnvironment(asIScriptEngine*) {}

} // namespace nvsound
