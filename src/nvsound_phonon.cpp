/* nvsound_phonon.cpp - Steam Audio spatialization and sound environments for NVGT's sound engine.
 * Follows the legacy engine's phonon_dsp: with HRTF on, distance still lowers the volume on the legacy curve while Steam Audio places the sound in direction. Inside a sound_environment, Steam Audio simulates the rest: distance, air absorption, occlusion, transmission through walls, and reflections.
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
#include "nvsound_phonon.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <angelscript.h>
#include <phonon.h>

namespace nvsound {

static std::mutex g_phonon_mutex;
static bool g_phonon_tried = false;
static IPLContext g_context = nullptr;
static IPLHRTF g_hrtf = nullptr;
static IPLAudioSettings g_audio_settings{48000, block_frames};
static const int g_ambisonics_order = 2, g_ambisonics_channels = 9; // (order + 1) squared.

// Steam Audio is delay loaded on Windows, so if phonon.dll is missing the first call into it raises an exception rather than failing. Catch that here, where nothing needs unwinding, so a missing DLL means no HRTF rather than a crash.
static IPLerror create_context(IPLContextSettings* settings, IPLContext* context) {
#ifdef _MSC_VER
	__try {
		return iplContextCreate(settings, context);
	} __except (1) {
		return IPL_STATUS_FAILURE;
	}
#else
	return iplContextCreate(settings, context);
#endif
}

bool phonon_init() {
	std::lock_guard<std::mutex> l(g_phonon_mutex);
	if (g_context) return true;
	if (g_phonon_tried || !init()) return false;
	g_phonon_tried = true;
	g_audio_settings = {get_sample_rate(), block_frames};
	IPLContextSettings context_settings{};
	context_settings.version = STEAMAUDIO_VERSION;
	if (create_context(&context_settings, &g_context) != IPL_STATUS_SUCCESS) {
		g_context = nullptr;
		return false;
	}
	IPLHRTFSettings hrtf_settings{};
	hrtf_settings.type = IPL_HRTFTYPE_DEFAULT;
	hrtf_settings.volume = 1.0f;
	if (iplHRTFCreate(g_context, &g_audio_settings, &hrtf_settings, &g_hrtf) != IPL_STATUS_SUCCESS) {
		iplContextRelease(&g_context);
		g_context = nullptr;
		return false;
	}
	return true;
}
bool phonon_available() {
	std::lock_guard<std::mutex> l(g_phonon_mutex);
	return g_context != nullptr;
}

static IPLCoordinateSpace3 listener_space(float x, float y, float z, float rotation) { return IPLCoordinateSpace3{IPLVector3{1, 0, 0}, IPLVector3{0, 0, 1}, IPLVector3{sinf(rotation), cosf(rotation), 0}, IPLVector3{x, y, z}}; }

// A sound or mixer's place in a sound_environment: its Steam Audio source and the effects that render what the simulation finds.
struct env_source {
	sound_environment* env = nullptr;
	IPLSource source = nullptr;
	IPLDirectEffect direct = nullptr;
	IPLReflectionEffect reflection = nullptr;
	IPLAmbisonicsDecodeEffect decode = nullptr;
};

struct phonon_state {
	IPLBinauralEffect binaural = nullptr;
	std::atomic<bool> reset{false};
	bool active = false; // Whether the last block went through the effect, so it can be reset when the sound comes back to the listener.
	env_source* env = nullptr; // Read by the audio thread, so only changed under the graph mutex.
};

class sound_environment {
	std::atomic<int> ref_count{1};
	IPLScene scene = nullptr;
	IPLSimulator sim = nullptr;
	std::unordered_map<std::string, IPLMaterial> materials;
	std::vector<IPLStaticMesh> meshes;
	int source_count = 0;
	// The environment's thread owns the simulator: it applies what the script changed, then runs the simulation. Anything else that touches the scene or adds and removes sources takes sim_mutex first, which keeps every Steam Audio commit off the simulation's toes.
	std::mutex sim_mutex;
	std::thread thread;
	std::atomic<bool> stopping{false};
	std::mutex wake_mutex;
	std::condition_variable wake;
	// What the script changed since the last pass. set_position lands here rather than taking sim_mutex, so a game moving sounds every frame never waits on a simulation.
	std::mutex pending_mutex;
	std::unordered_map<IPLSource, IPLVector3> pending_positions;
	bool scene_dirty = false, sources_dirty = false, listener_dirty = true;
	// The listener, which the audio thread reads every block.
	spinlock listener_lock;
	float lx = 0, ly = 0, lz = 0, lrot = 0;
	void set_source_inputs(IPLSource source, IPLVector3 position) {
		IPLSimulationInputs inputs{};
		inputs.flags = IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
		inputs.directFlags = IPLDirectSimulationFlags(IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
		inputs.source = IPLCoordinateSpace3{IPLVector3{1, 0, 0}, IPLVector3{0, 0, 1}, IPLVector3{0, 1, 0}, position};
		inputs.distanceAttenuationModel = IPLDistanceAttenuationModel{IPL_DISTANCEATTENUATIONTYPE_DEFAULT};
		inputs.airAbsorptionModel = IPLAirAbsorptionModel{IPL_AIRABSORPTIONTYPE_DEFAULT};
		inputs.occlusionType = IPL_OCCLUSIONTYPE_RAYCAST;
		inputs.numTransmissionRays = 3; // How many surfaces sound may pass through on its way to the listener.
		iplSourceSetInputs(source, IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS), &inputs);
	}
	void thread_loop() {
		auto last_reflections = std::chrono::steady_clock::now() - std::chrono::seconds(1);
		while (!stopping) {
			{
				std::lock_guard<std::mutex> sl(sim_mutex);
				std::unordered_map<IPLSource, IPLVector3> positions;
				bool scene_changed, sources_changed, listener_changed;
				{
					std::lock_guard<std::mutex> pl(pending_mutex);
					positions.swap(pending_positions);
					scene_changed = scene_dirty;
					sources_changed = sources_dirty;
					listener_changed = listener_dirty;
					scene_dirty = sources_dirty = listener_dirty = false;
				}
				if (scene_changed) iplSceneCommit(scene);
				for (auto& p : positions) set_source_inputs(p.first, p.second);
				if (listener_changed) {
					IPLSimulationSharedInputs shared{};
					{
						std::lock_guard<spinlock> ll(listener_lock);
						shared.listener = listener_space(lx, ly, lz, lrot);
					}
					shared.numRays = 2048;
					shared.numBounces = 32;
					shared.duration = 2.0f;
					shared.order = g_ambisonics_order;
					shared.irradianceMinDistance = 1.0f;
					iplSimulatorSetSharedInputs(sim, IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS), &shared);
				}
				if (scene_changed || sources_changed) iplSimulatorCommit(sim);
				if (source_count > 0) {
					iplSimulatorRunDirect(sim);
					// Reflections cost far more than direct sound and change slowly, so they are traced at most twenty times a second.
					auto now = std::chrono::steady_clock::now();
					if (now - last_reflections >= std::chrono::milliseconds(50)) {
						iplSimulatorRunReflections(sim);
						last_reflections = now;
					}
				}
			}
			// The legacy engine ran this loop flat out on a core of its own; resting between passes costs nothing audible.
			std::unique_lock<std::mutex> wl(wake_mutex);
			wake.wait_for(wl, std::chrono::milliseconds(10), [this] { return stopping.load(); });
		}
	}
public:
	const int ir_size;
	sound_environment() : ir_size(int(2.0f * get_sample_rate())) {
		IPLSimulationSettings settings{};
		settings.flags = IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
		settings.sceneType = IPL_SCENETYPE_DEFAULT;
		settings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
		settings.maxNumRays = 2048;
		settings.numDiffuseSamples = 128;
		settings.maxDuration = 2.0f;
		settings.maxOrder = g_ambisonics_order;
		settings.maxNumSources = 64;
		settings.numThreads = std::max(1, int(std::thread::hardware_concurrency()) / 2);
		settings.samplingRate = g_audio_settings.samplingRate;
		settings.frameSize = g_audio_settings.frameSize;
		if (iplSimulatorCreate(g_context, &settings, &sim) != IPL_STATUS_SUCCESS) throw std::runtime_error("could not create the Steam Audio simulator");
		IPLSceneSettings scene_settings{};
		scene_settings.type = IPL_SCENETYPE_DEFAULT;
		if (iplSceneCreate(g_context, &scene_settings, &scene) != IPL_STATUS_SUCCESS) {
			iplSimulatorRelease(&sim);
			throw std::runtime_error("could not create the Steam Audio scene");
		}
		// The legacy engine's materials: absorption low, mid, high, scattering, transmission low, mid, high.
		add_material("air", 0, 0, 0, 0, 1, 1, 1);
		add_material("generic", 0.10f, 0.20f, 0.30f, 0.05f, 0.100f, 0.050f, 0.030f);
		add_material("brick", 0.03f, 0.04f, 0.07f, 0.05f, 0.015f, 0.015f, 0.015f);
		add_material("concrete", 0.05f, 0.07f, 0.08f, 0.05f, 0.015f, 0.002f, 0.001f);
		add_material("ceramic", 0.01f, 0.02f, 0.02f, 0.05f, 0.060f, 0.044f, 0.011f);
		add_material("gravel", 0.60f, 0.70f, 0.80f, 0.05f, 0.031f, 0.012f, 0.008f);
		add_material("carpet", 0.24f, 0.69f, 0.73f, 0.05f, 0.020f, 0.005f, 0.003f);
		add_material("glass", 0.06f, 0.03f, 0.02f, 0.05f, 0.060f, 0.044f, 0.011f);
		add_material("plaster", 0.12f, 0.06f, 0.04f, 0.05f, 0.056f, 0.056f, 0.004f);
		add_material("wood", 0.11f, 0.07f, 0.06f, 0.05f, 0.070f, 0.014f, 0.005f);
		add_material("metal", 0.20f, 0.07f, 0.06f, 0.05f, 0.200f, 0.025f, 0.010f);
		add_material("rock", 0.13f, 0.20f, 0.24f, 0.05f, 0.015f, 0.002f, 0.001f);
		iplSimulatorSetScene(sim, scene);
		iplSimulatorCommit(sim);
		thread = std::thread(&sound_environment::thread_loop, this);
	}
	~sound_environment() {
		stopping = true;
		wake.notify_one();
		if (thread.joinable()) thread.join();
		for (IPLStaticMesh& m : meshes) {
			iplStaticMeshRemove(m, scene);
			iplStaticMeshRelease(&m);
		}
		iplSceneRelease(&scene);
		iplSimulatorRelease(&sim);
	}
	void add_ref() { ref_count++; }
	void release() {
		if (--ref_count == 0) delete this;
	}
	bool add_material(const std::string& name, float absorption_low, float absorption_mid, float absorption_high, float scattering, float transmission_low, float transmission_mid, float transmission_high, bool replace_if_existing = false) {
		if (!replace_if_existing && materials.find(name) != materials.end()) return false;
		materials[name] = IPLMaterial{{absorption_low, absorption_mid, absorption_high}, scattering, {transmission_low, transmission_mid, transmission_high}};
		return true;
	}
	bool add_box(const std::string& material, float minx, float maxx, float miny, float maxy, float minz, float maxz) {
		auto it = materials.find(material);
		if (it == materials.end()) return false;
		IPLVector3 vertices[8] = {{minx, miny, minz}, {maxx, miny, minz}, {maxx, maxy, minz}, {minx, maxy, minz}, {minx, miny, maxz}, {maxx, miny, maxz}, {maxx, maxy, maxz}, {minx, maxy, maxz}};
		// Floor, the four walls and the roof, two triangles each, as the legacy engine built them.
		IPLTriangle triangles[12] = {{0, 1, 2}, {0, 2, 3}, {0, 1, 5}, {0, 5, 4}, {1, 5, 6}, {1, 6, 2}, {2, 6, 7}, {2, 7, 3}, {3, 7, 0}, {3, 0, 4}, {4, 5, 6}, {4, 6, 7}};
		IPLint32 material_indexes[12] = {0};
		IPLMaterial mat = it->second;
		IPLStaticMeshSettings mesh_settings{8, 12, 1, vertices, triangles, material_indexes, &mat};
		std::lock_guard<std::mutex> sl(sim_mutex);
		IPLStaticMesh mesh = nullptr;
		if (iplStaticMeshCreate(scene, &mesh_settings, &mesh) != IPL_STATUS_SUCCESS) return false;
		iplStaticMeshAdd(mesh, scene);
		meshes.push_back(mesh);
		std::lock_guard<std::mutex> pl(pending_mutex);
		scene_dirty = true; // Committed by the thread, so building a level of many boxes rebuilds the scene once rather than once per box.
		return true;
	}
	void set_listener(float x, float y, float z, float rotation) {
		{
			std::lock_guard<spinlock> l(listener_lock);
			lx = x;
			ly = y;
			lz = z;
			lrot = rotation;
		}
		std::lock_guard<std::mutex> pl(pending_mutex);
		listener_dirty = true;
	}
	void get_listener(float& x, float& y, float& z, float& rotation) {
		std::lock_guard<spinlock> l(listener_lock);
		x = lx;
		y = ly;
		z = lz;
		rotation = lrot;
	}
	void update() { wake.notify_one(); } // The thread runs on its own; this only asks it to run now.
	void set_position(IPLSource source, float x, float y, float z) {
		std::lock_guard<std::mutex> pl(pending_mutex);
		pending_positions[source] = IPLVector3{x, y, z};
	}
	env_source* attach(float x, float y, float z) {
		env_source* es = new env_source();
		es->env = this;
		IPLDirectEffectSettings direct_settings{1};
		IPLReflectionEffectSettings reflection_settings{IPL_REFLECTIONEFFECTTYPE_CONVOLUTION, ir_size, g_ambisonics_channels};
		IPLAmbisonicsDecodeEffectSettings decode_settings{};
		decode_settings.speakerLayout = IPLSpeakerLayout{IPL_SPEAKERLAYOUTTYPE_STEREO};
		decode_settings.hrtf = g_hrtf;
		decode_settings.maxOrder = g_ambisonics_order;
		IPLSourceSettings source_settings{IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS)};
		bool ok = iplDirectEffectCreate(g_context, &g_audio_settings, &direct_settings, &es->direct) == IPL_STATUS_SUCCESS && iplReflectionEffectCreate(g_context, &g_audio_settings, &reflection_settings, &es->reflection) == IPL_STATUS_SUCCESS && iplAmbisonicsDecodeEffectCreate(g_context, &g_audio_settings, &decode_settings, &es->decode) == IPL_STATUS_SUCCESS;
		if (ok) {
			std::lock_guard<std::mutex> sl(sim_mutex);
			ok = iplSourceCreate(sim, &source_settings, &es->source) == IPL_STATUS_SUCCESS;
			if (ok) {
				set_source_inputs(es->source, IPLVector3{x, y, z});
				iplSourceAdd(es->source, sim);
				source_count++;
				std::lock_guard<std::mutex> pl(pending_mutex);
				sources_dirty = true;
			}
		}
		if (!ok) {
			free_effects(es);
			delete es;
			return nullptr;
		}
		add_ref(); // Every attached source keeps its environment alive.
		return es;
	}
	// The caller has already made sure the audio thread can no longer reach es.
	void detach(env_source* es) {
		{
			std::lock_guard<std::mutex> sl(sim_mutex);
			iplSourceRemove(es->source, sim);
			iplSimulatorCommit(sim);
			source_count--;
			std::lock_guard<std::mutex> pl(pending_mutex);
			pending_positions.erase(es->source);
		}
		iplSourceRelease(&es->source);
		free_effects(es);
		delete es;
		release();
	}
	static void free_effects(env_source* es) {
		if (es->decode) iplAmbisonicsDecodeEffectRelease(&es->decode);
		if (es->reflection) iplReflectionEffectRelease(&es->reflection);
		if (es->direct) iplDirectEffectRelease(&es->direct);
	}
	sound* new_sound();
	mixer* new_mixer();
};

phonon_state* create_phonon_state() {
	if (!phonon_init()) return nullptr;
	phonon_state* s = new phonon_state();
	IPLBinauralEffectSettings settings{};
	settings.hrtf = g_hrtf;
	if (iplBinauralEffectCreate(g_context, &g_audio_settings, &settings, &s->binaural) != IPL_STATUS_SUCCESS) {
		delete s;
		return nullptr;
	}
	return s;
}
void release_phonon_state(phonon_state* s) {
	if (!s) return;
	if (env_source* es = s->env) {
		{
			std::lock_guard<std::recursive_mutex> l(get_graph_mutex());
			s->env = nullptr;
		}
		es->env->detach(es);
	}
	if (s->binaural) iplBinauralEffectRelease(&s->binaural);
	delete s;
}
void reset_phonon_state(phonon_state* s) {
	if (s) s->reset = true;
}
bool phonon_attach_environment(phonon_state* s, sound_environment* env, float x, float y, float z) {
	if (!s || !env || s->env) return false;
	env_source* es = env->attach(x, y, z);
	if (!es) return false;
	std::lock_guard<std::recursive_mutex> l(get_graph_mutex());
	s->env = es;
	return true;
}
void phonon_set_position(phonon_state* s, float x, float y, float z) {
	if (s && s->env) s->env->env->set_position(s->env->source, x, y, z);
}

// Inside an environment: Steam Audio's direct sound (distance, air, occlusion, transmission) placed with the HRTF, plus its simulated reflections decoded binaurally. Steam Audio wants exactly one block here.
static void spatialize_in_environment(phonon_state* s, float* frames, const spatial_params& p) {
	env_source* es = s->env;
	float lx, ly, lz, lrot;
	es->env->get_listener(lx, ly, lz, lrot);
	IPLSimulationOutputs outputs{};
	iplSourceGetOutputs(es->source, IPLSimulationFlags(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS), &outputs);
	static thread_local float in_l[block_frames], in_r[block_frames], mono[block_frames], direct[block_frames], out_l[block_frames], out_r[block_frames], refl_l[block_frames], refl_r[block_frames], ambi[g_ambisonics_channels][block_frames];
	float* in_data[] = {in_l, in_r};
	float* mono_data[] = {mono};
	float* direct_data[] = {direct};
	float* out_data[] = {out_l, out_r};
	float* refl_data[] = {refl_l, refl_r};
	float* ambi_data[g_ambisonics_channels];
	for (int c = 0; c < g_ambisonics_channels; c++) ambi_data[c] = ambi[c];
	IPLAudioBuffer in{2, block_frames, in_data}, mono_buf{1, block_frames, mono_data}, direct_buf{1, block_frames, direct_data}, out{2, block_frames, out_data}, refl{2, block_frames, refl_data}, ambi_buf{g_ambisonics_channels, block_frames, ambi_data};
	iplAudioBufferDeinterleave(g_context, frames, &in);
	iplAudioBufferDownmix(g_context, &in, &mono_buf);
	IPLDirectEffectParams direct_params = outputs.direct;
	// The legacy engine simulated transmission but never applied it, so walls silenced sounds rather than muffling them.
	direct_params.flags = IPLDirectEffectFlags(IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION | IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION);
	direct_params.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
	iplDirectEffectApply(es->direct, &direct_params, &mono_buf, &direct_buf);
	float dx = p.x - lx, dy = p.y - ly, dz = p.z - lz;
	if (dx != 0 || dy != 0 || dz != 0) {
		IPLBinauralEffectParams params{};
		params.direction = iplCalculateRelativeDirection(g_context, IPLVector3{p.x, p.y, p.z}, IPLVector3{lx, ly, lz}, IPLVector3{sinf(lrot), cosf(lrot), 0}, IPLVector3{0, 0, 1});
		params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
		params.spatialBlend = std::clamp((fabsf(dx * p.pan_step) + fabsf(dy * p.pan_step) + fabsf(dz * p.pan_step)) / 3.0f, 0.0f, 1.0f);
		params.hrtf = g_hrtf;
		iplBinauralEffectApply(s->binaural, &params, &direct_buf, &out);
		s->active = true;
	} else {
		if (s->active) iplBinauralEffectReset(s->binaural);
		s->active = false;
		memcpy(out_l, direct, sizeof(direct));
		memcpy(out_r, direct, sizeof(direct));
	}
	// Reflections exist only once the simulation has traced them for the first time.
	if (outputs.reflections.ir) {
		IPLReflectionEffectParams reflection_params = outputs.reflections;
		reflection_params.numChannels = g_ambisonics_channels;
		reflection_params.irSize = es->env->ir_size;
		iplReflectionEffectApply(es->reflection, &reflection_params, &mono_buf, &ambi_buf, nullptr);
		IPLAmbisonicsDecodeEffectParams decode_params{g_ambisonics_order, g_hrtf, listener_space(lx, ly, lz, lrot), IPL_TRUE};
		iplAmbisonicsDecodeEffectApply(es->decode, &decode_params, &ambi_buf, &refl);
		iplAudioBufferMix(g_context, &refl, &out);
	}
	iplAudioBufferInterleave(g_context, &out, frames);
}

// Places stereo audio in direction with the HRTF, in place, and returns the distance attenuation that goes with it in vol_db. Steam Audio takes at most block_frames at a time, so longer requests (from a pitched mixer above) are done in pieces.
void phonon_spatialize(phonon_state* s, float* frames, int count, const spatial_params& p, float& vol_db) {
	vol_db = 0;
	if (s->reset.exchange(false)) iplBinauralEffectReset(s->binaural);
	// An environment's effects need whole blocks; the odd sized requests a pitched mixer makes fall back to plain HRTF.
	if (s->env && count == block_frames) {
		spatialize_in_environment(s, frames, p);
		return;
	}
	float dx = p.x - p.listener_x, dy = p.y - p.listener_y, dz = p.z - p.listener_z;
	if (dx == 0 && dy == 0 && dz == 0) {
		// At the listener, the legacy engine left the audio alone and cleared the effect so it starts clean next time.
		if (s->active) iplBinauralEffectReset(s->binaural);
		s->active = false;
		return;
	}
	s->active = true;
	float distance = sqrtf(dx * dx + dy * dy + dz * dz);
	if (p.rotation > 0) {
		float rx = cosf(p.rotation) * dx - sinf(p.rotation) * dy, ry = sinf(p.rotation) * dx + cosf(p.rotation) * dy;
		dx = rx;
		dy = ry;
	}
	float v = 1.0f - floorf(distance) / (125.0f / p.volume_step);
	vol_db = std::clamp(v, 0.0f, 1.0f) * 100.0f - 100.0f;
	IPLBinauralEffectParams params{};
	params.direction = iplCalculateRelativeDirection(g_context, IPLVector3{p.x, p.y, p.z}, IPLVector3{p.listener_x, p.listener_y, p.listener_z}, IPLVector3{sinf(p.rotation), cosf(p.rotation), 0}, IPLVector3{0, 0, 1});
	params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
	// Close sounds are only partly spatialized, so they don't snap from ear to ear as they pass through the listener.
	params.spatialBlend = std::clamp((fabsf(dx * p.pan_step) + fabsf(dy * p.pan_step) + fabsf(dz * p.pan_step)) / 3.0f, 0.0f, 1.0f);
	params.hrtf = g_hrtf;
	float in_l[block_frames], in_r[block_frames], out_l[block_frames], out_r[block_frames];
	float* in_data[] = {in_l, in_r};
	float* out_data[] = {out_l, out_r};
	for (int done = 0; done < count;) {
		int n = std::min(count - done, block_frames);
		IPLAudioBuffer in{2, n, in_data}, out{2, n, out_data};
		iplAudioBufferDeinterleave(g_context, frames + done * 2, &in);
		iplBinauralEffectApply(s->binaural, &params, &in, &out);
		iplAudioBufferInterleave(g_context, &out, frames + done * 2);
		done += n;
	}
}

// Sounds and mixers made by an environment have HRTF on, which the legacy engine left to the script even though nothing in an environment is heard without it.
sound* sound_environment::new_sound() {
	sound* s = make_sound();
	s->use_hrtf = true;
	s->attach_environment(this);
	return s;
}
mixer* sound_environment::new_mixer() {
	mixer* m = make_mixer();
	m->use_hrtf = true;
	m->attach_environment(this);
	return m;
}

static sound_environment* script_environment_factory() {
	if (!phonon_init()) {
		if (asIScriptContext* ctx = asGetActiveContext()) ctx->SetException("Steam Audio is not available");
		return nullptr;
	}
	set_global_hrtf(true); // As the legacy engine did on creating an environment.
	try {
		return new sound_environment();
	} catch (std::exception& e) {
		if (asIScriptContext* ctx = asGetActiveContext()) ctx->SetException(e.what());
		return nullptr;
	}
}

void RegisterNvsoundEnvironment(asIScriptEngine* engine) {
	engine->RegisterObjectType("sound_environment", 0, asOBJ_REF);
	engine->RegisterObjectBehaviour("sound_environment", asBEHAVE_FACTORY, "sound_environment@ e()", asFUNCTION(script_environment_factory), asCALL_CDECL);
	engine->RegisterObjectBehaviour("sound_environment", asBEHAVE_ADDREF, "void f()", asMETHOD(sound_environment, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("sound_environment", asBEHAVE_RELEASE, "void f()", asMETHOD(sound_environment, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound_environment", "bool add_material(const string&in name, float absorption_low, float absorption_mid, float absorption_high, float scattering, float transmission_low, float transmission_mid, float transmission_high, bool replace_if_existing = false)", asMETHOD(sound_environment, add_material), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound_environment", "bool add_box(const string&in material, float minx, float maxx, float miny, float maxy, float minz, float maxz)", asMETHOD(sound_environment, add_box), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound_environment", "mixer@ new_mixer()", asMETHOD(sound_environment, new_mixer), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound_environment", "sound@ new_sound()", asMETHOD(sound_environment, new_sound), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound_environment", "void update()", asMETHOD(sound_environment, update), asCALL_THISCALL);
	engine->RegisterObjectMethod("sound_environment", "void set_listener(float x, float y, float z, float rotation)", asMETHOD(sound_environment, set_listener), asCALL_THISCALL);
}

} // namespace nvsound
