/* nvsound_audio_decoder.cpp - audio_decoder, which reads raw float samples out of a file, a pack or a datastream.
 * It plays nothing. It is for scripts that want the samples themselves: to analyse them, to send them over a network, or to push them into a sound of their own.
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
#include <atomic>
#include <cstring>
#include <angelscript.h>
#include <scriptarray.h>
#include <resample.h>
#include "datastreams.h"
#include "nvgt.h" // g_ScriptEngine

namespace nvsound {

class audio_decoder {
	std::atomic<int> ref_count{1};
	std::unique_ptr<decoder> dec;
	datastream* ds = nullptr; // Kept alive while the decoder reads from it.
	unsigned int out_rate = 0, out_channels = 0;
	std::unique_ptr<WDL_Resampler> resampler;
	std::vector<float> in_frames;
	uint64_t cursor = 0; // Frames handed out, at the output rate.
	bool looping = false, at_end = false;
	// Reads frames at the decoder's own rate and channel count, starting over at the end when looping.
	uint64_t read_source(float* out, uint64_t frames) {
		uint64_t done = 0;
		while (done < frames && !at_end) {
			uint64_t got = dec->read(out + done * dec->channels, frames - done);
			done += got;
			if (got) continue;
			if (!looping || !dec->seek(0)) at_end = true;
		}
		return done;
	}
	// Mono is copied to every channel; anything going down to mono is averaged; otherwise channels are matched up in order and any extra ones are silent.
	void convert_channels(const float* in, unsigned int in_ch, float* out, uint64_t frames) {
		for (uint64_t f = 0; f < frames; f++) {
			const float* i = in + f * in_ch;
			float* o = out + f * out_channels;
			if (in_ch == out_channels) memcpy(o, i, in_ch * sizeof(float));
			else if (in_ch == 1) std::fill(o, o + out_channels, i[0]);
			else if (out_channels == 1) {
				float sum = 0;
				for (unsigned int c = 0; c < in_ch; c++) sum += i[c];
				o[0] = sum / in_ch;
			} else {
				for (unsigned int c = 0; c < out_channels; c++) o[c] = c < in_ch ? i[c] : 0;
			}
		}
	}
	bool start(std::unique_ptr<decoder> d, unsigned int sample_rate, unsigned int channels) {
		if (!d) return false;
		dec = std::move(d);
		out_rate = sample_rate ? sample_rate : dec->sample_rate;
		out_channels = channels ? std::min<unsigned int>(channels, max_source_channels) : dec->channels;
		if (out_rate != dec->sample_rate) {
			resampler = std::make_unique<WDL_Resampler>();
			resampler->SetMode(false, 0, true, 64, 32);
			resampler->SetRates(dec->sample_rate, out_rate);
		}
		cursor = 0;
		at_end = false;
		return true;
	}
public:
	~audio_decoder() { close(); }
	void add_ref() { ref_count++; }
	void release() {
		if (--ref_count == 0) delete this;
	}
	bool open(const std::string& filename, const pack_interface* pack, unsigned int sample_rate, unsigned int channels) {
		close();
		return start(open_decoder(open_sound_file(filename, pack)), sample_rate, channels);
	}
	// Takes the handle's reference. A stream that can't seek is read as it arrives rather than all at once, so a live network stream works.
	bool open_stream(datastream* stream, unsigned int sample_rate, unsigned int channels) {
		close();
		std::istream* in = stream ? stream->get_istr() : nullptr;
		if (!in) {
			if (stream) stream->release();
			return false;
		}
		std::unique_ptr<std::istream> wrapped;
		if (in->tellg() == std::streampos(-1)) wrapped = open_window_stream(in);
		else wrapped = std::make_unique<std::istream>(in->rdbuf()); // Shares the datastream's buffer without owning it.
		if (!start(open_decoder(std::move(wrapped)), sample_rate, channels)) {
			stream->release();
			return false;
		}
		ds = stream;
		return true;
	}
	bool close() {
		bool was_open = bool(dec);
		dec.reset(); // Before the datastream it reads from goes.
		resampler.reset();
		if (ds) ds->release();
		ds = nullptr;
		out_rate = out_channels = 0;
		return was_open;
	}
	// Up to frames frames at the output rate and channel count; fewer at the end, and none once it's over.
	uint64_t read(float* out, uint64_t frames) {
		if (!dec) return 0;
		uint64_t done = 0;
		unsigned int in_ch = dec->channels;
		while (done < frames) {
			uint64_t want = std::min<uint64_t>(frames - done, 4096), got;
			if (!resampler) {
				in_frames.resize(want * in_ch);
				got = read_source(in_frames.data(), want);
				convert_channels(in_frames.data(), in_ch, out + done * out_channels, got);
			} else {
				WDL_ResampleSample* rin;
				int need = resampler->ResamplePrepare(int(want), int(in_ch), &rin);
				uint64_t have = read_source(rin, uint64_t(need));
				if (have < uint64_t(need)) memset(rin + have * in_ch, 0, (need - have) * in_ch * sizeof(float));
				in_frames.resize(want * in_ch);
				got = uint64_t(resampler->ResampleOut(in_frames.data(), need, int(want), int(in_ch)));
				// At the end the resampler pads with silence; stop at the source's own last frame instead.
				if (have < uint64_t(need)) got = std::min<uint64_t>(got, uint64_t(double(have) * out_rate / dec->sample_rate + 0.5));
				convert_channels(in_frames.data(), in_ch, out + done * out_channels, got);
			}
			done += got;
			if (got < want) break;
		}
		cursor += done;
		return done;
	}
	CScriptArray* read_script(uint64_t frames) {
		CScriptArray* a = CScriptArray::Create(g_ScriptEngine->GetTypeInfoByDecl("array<float>"));
		if (!dec) return a;
		if (!frames) frames = get_advised_read_frame_count();
		a->Resize(asUINT(frames * out_channels));
		uint64_t got = read((float*)a->At(0), frames);
		a->Resize(asUINT(got * out_channels));
		return a;
	}
	bool seek_frames(uint64_t frame) {
		if (!dec) return false;
		uint64_t source_frame = uint64_t(double(frame) * dec->sample_rate / out_rate);
		if (!dec->seek(source_frame)) return false;
		if (resampler) resampler->Reset();
		cursor = frame;
		at_end = false;
		return true;
	}
	uint64_t skip_frames(uint64_t frames) {
		if (!dec) return 0;
		if (seek_frames(cursor + frames)) return frames;
		// A stream that can't seek is read through instead.
		std::vector<float> scratch(4096 * out_channels);
		uint64_t skipped = 0;
		while (skipped < frames) {
			uint64_t got = read(scratch.data(), std::min<uint64_t>(4096, frames - skipped));
			if (!got) break;
			skipped += got;
		}
		return skipped;
	}
	float skip_milliseconds(float ms) { return out_rate ? float(skip_frames(uint64_t(ms * out_rate / 1000.0)) * 1000.0 / out_rate) : 0; }
	bool seek_milliseconds(float ms) { return out_rate && ms >= 0 && seek_frames(uint64_t(ms * out_rate / 1000.0)); }
	uint64_t get_cursor_frames() const { return cursor; }
	float get_cursor_milliseconds() const { return out_rate ? float(cursor * 1000.0 / out_rate) : 0; }
	uint64_t get_length_frames() const { return dec ? uint64_t(double(dec->length()) * out_rate / dec->sample_rate) : 0; } // 0 when the length isn't known, as for a live stream.
	float get_length_milliseconds() const { return dec ? float(dec->length() * 1000.0 / dec->sample_rate) : 0; }
	bool set_looping(bool loop) {
		looping = loop;
		return bool(dec);
	}
	bool get_looping() const { return looping; }
	unsigned int get_channels() const { return out_channels; }
	unsigned int get_sample_rate() const { return out_rate; }
	bool get_active() const { return dec && !at_end; }
	unsigned int get_advised_read_frame_count() const { return block_frames; }
};
static audio_decoder* make_audio_decoder() { return new audio_decoder(); }

void RegisterNvsoundDecoder(asIScriptEngine* engine) {
	std::string ns = engine->GetDefaultNamespace();
	std::string default_pack = ns.empty() ? "sound_default_pack" : ns + "::sound_default_pack";
	engine->RegisterObjectType("audio_decoder", 0, asOBJ_REF);
	engine->RegisterObjectBehaviour("audio_decoder", asBEHAVE_FACTORY, "audio_decoder@ d()", asFUNCTION(make_audio_decoder), asCALL_CDECL);
	engine->RegisterObjectBehaviour("audio_decoder", asBEHAVE_ADDREF, "void f()", asMETHOD(audio_decoder, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("audio_decoder", asBEHAVE_RELEASE, "void f()", asMETHOD(audio_decoder, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", ("bool open(const string&in filename, const pack_interface@+ pack_file = " + default_pack + ", uint sample_rate = 0, uint channels = 0)").c_str(), asMETHOD(audio_decoder, open), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "bool open(datastream@ stream, uint sample_rate = 0, uint channels = 0)", asMETHOD(audio_decoder, open_stream), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "bool close()", asMETHOD(audio_decoder, close), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "float[]@ read(uint64 frame_count = 0)", asMETHOD(audio_decoder, read_script), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "uint64 skip_frames(uint64 frame_count)", asMETHOD(audio_decoder, skip_frames), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "float skip_milliseconds(float ms)", asMETHOD(audio_decoder, skip_milliseconds), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "bool seek_frames(uint64 frame_index)", asMETHOD(audio_decoder, seek_frames), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "bool seek_milliseconds(float ms)", asMETHOD(audio_decoder, seek_milliseconds), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "uint64 get_cursor_frames() const property", asMETHOD(audio_decoder, get_cursor_frames), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "float get_cursor_milliseconds() const property", asMETHOD(audio_decoder, get_cursor_milliseconds), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "uint64 get_length_frames() const property", asMETHOD(audio_decoder, get_length_frames), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "float get_length_milliseconds() const property", asMETHOD(audio_decoder, get_length_milliseconds), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "bool set_looping(bool looping)", asMETHOD(audio_decoder, set_looping), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "bool get_looping() const property", asMETHOD(audio_decoder, get_looping), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "uint get_channels() const property", asMETHOD(audio_decoder, get_channels), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "uint get_sample_rate() const property", asMETHOD(audio_decoder, get_sample_rate), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "bool get_active() const property", asMETHOD(audio_decoder, get_active), asCALL_THISCALL);
	engine->RegisterObjectMethod("audio_decoder", "uint get_advised_read_frame_count() const property", asMETHOD(audio_decoder, get_advised_read_frame_count), asCALL_THISCALL);
}

} // namespace nvsound
