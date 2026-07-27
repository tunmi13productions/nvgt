/* opus_codec.cpp - raw Opus packet encoder/decoder for AngelScript
 *
 * FORK ADDITION -- this file does not exist upstream (samtupy/nvgt). See opus_codec.h for why.
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

#include <angelscript.h>
#include <opus/opus.h>
#include <cassert>
#include <cstring>
#include <string>
#include <vector>
#include "opus_codec.h"

using namespace std;

// Comfortably above any packet 48kHz voice produces; opus_encode is told this is the ceiling and
// returns the real length, so an over-estimate only costs a stack-free vector that is trimmed.
static const opus_int32 OPUS_CODEC_MAX_PACKET = 4000;

// PCM crosses the script boundary as a byte string of signed 16-bit little-endian samples, which
// is the shape the script side already builds and reads (see float_to_int16_string /
// decode_to_float in the calling scripts). Every platform nvgt targets is little-endian, so the
// bytes are copied rather than swapped, but the copy is explicit rather than a reinterpret_cast
// over the string's buffer, which would assume an alignment std::string does not promise.

class nvgt_opus_encoder {
	mutable int refcount;
	OpusEncoder* enc;
	int channels;
public:
	nvgt_opus_encoder(int sample_rate, int chans, int application) : refcount(1), enc(nullptr), channels(chans) {
		if (chans < 1 || chans > 2) return;
		int err = OPUS_OK;
		enc = opus_encoder_create(sample_rate, chans, application, &err);
		if (err != OPUS_OK) {
			if (enc) opus_encoder_destroy(enc);
			enc = nullptr;
		}
	}
	~nvgt_opus_encoder() { if (enc) opus_encoder_destroy(enc); }
	void duplicate() const { asAtomicInc(refcount); }
	void release() const { if (asAtomicDec(refcount) < 1) delete this; }
	// A failed opus_encoder_create leaves a live but unusable object rather than throwing, so
	// callers are expected to check this before use.
	bool get_valid() const { return enc != nullptr; }
	void set_bitrate(int v) { if (enc) opus_encoder_ctl(enc, OPUS_SET_BITRATE(v)); }
	void set_complexity(int v) { if (enc) opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(v)); }
	void set_dtx(bool v) { if (enc) opus_encoder_ctl(enc, OPUS_SET_DTX(v ? 1 : 0)); }
	void set_signal(int v) { if (enc) opus_encoder_ctl(enc, OPUS_SET_SIGNAL(v)); }
	void set_inband_fec(bool v) { if (enc) opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(v ? 1 : 0)); }
	// One frame of interleaved int16 PCM in, one self-contained Opus packet out. The frame size is
	// derived from the string's length because that is what the caller already knows; opus only
	// accepts its own valid frame durations and rejects anything else, which surfaces as "".
	string encode(const string& pcm) {
		if (!enc || channels < 1) return "";
		const size_t samples = pcm.size() / sizeof(opus_int16);
		const int frame_size = int(samples / size_t(channels));
		if (frame_size <= 0) return "";
		vector<opus_int16> in(samples);
		memcpy(in.data(), pcm.data(), samples * sizeof(opus_int16));
		vector<unsigned char> out(size_t(OPUS_CODEC_MAX_PACKET));
		const opus_int32 n = opus_encode(enc, in.data(), frame_size, out.data(), OPUS_CODEC_MAX_PACKET);
		if (n < 0) return "";
		return string(reinterpret_cast<const char*>(out.data()), size_t(n));
	}
};

class nvgt_opus_decoder {
	mutable int refcount;
	OpusDecoder* dec;
	int channels;
	// A null packet is how libopus is asked to conceal a lost frame, so decode and conceal are the
	// same call with and without data behind them.
	string decode_internal(const unsigned char* data, opus_int32 len, int frame_size) {
		if (!dec || channels < 1 || frame_size <= 0) return "";
		vector<opus_int16> pcm(size_t(frame_size) * size_t(channels));
		const int frames = opus_decode(dec, data, len, pcm.data(), frame_size, 0);
		if (frames <= 0) return "";
		const size_t bytes = size_t(frames) * size_t(channels) * sizeof(opus_int16);
		string out;
		out.resize(bytes);
		memcpy(&out[0], pcm.data(), bytes);
		return out;
	}
public:
	nvgt_opus_decoder(int sample_rate, int chans) : refcount(1), dec(nullptr), channels(chans) {
		if (chans < 1 || chans > 2) return;
		int err = OPUS_OK;
		dec = opus_decoder_create(sample_rate, chans, &err);
		if (err != OPUS_OK) {
			if (dec) opus_decoder_destroy(dec);
			dec = nullptr;
		}
	}
	~nvgt_opus_decoder() { if (dec) opus_decoder_destroy(dec); }
	void duplicate() const { asAtomicInc(refcount); }
	void release() const { if (asAtomicDec(refcount) < 1) delete this; }
	bool get_valid() const { return dec != nullptr; }
	string decode(const string& packet, int frame_size) {
		if (packet.empty()) return "";
		return decode_internal(reinterpret_cast<const unsigned char*>(packet.data()), opus_int32(packet.size()), frame_size);
	}
	// Synthesises one frame to cover a packet that never arrived. Keeping the decoder fed this way
	// matters: it carries state between frames, so silently skipping a gap makes everything after
	// it sound worse than concealing it does.
	string conceal(int frame_size) { return decode_internal(nullptr, 0, frame_size); }
	// Throws away inter-frame state. For a gap too large to be worth concealing, resynchronising is
	// better than synthesising seconds of invented audio.
	void reset_state() { if (dec) opus_decoder_ctl(dec, OPUS_RESET_STATE); }
};

static nvgt_opus_encoder* create_opus_encoder(int sample_rate, int channels, int application) { return new nvgt_opus_encoder(sample_rate, channels, application); }
static nvgt_opus_decoder* create_opus_decoder(int sample_rate, int channels) { return new nvgt_opus_decoder(sample_rate, channels); }

void RegisterOpusCodec(asIScriptEngine* engine) {
	int ot = engine->RegisterObjectType("opus_encoder", 0, asOBJ_REF); assert(ot >= 0);
	engine->RegisterObjectBehaviour("opus_encoder", asBEHAVE_FACTORY, "opus_encoder@ e(int sample_rate, int channels, int application)", asFUNCTION(create_opus_encoder), asCALL_CDECL);
	engine->RegisterObjectBehaviour("opus_encoder", asBEHAVE_ADDREF, "void f()", asMETHOD(nvgt_opus_encoder, duplicate), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("opus_encoder", asBEHAVE_RELEASE, "void f()", asMETHOD(nvgt_opus_encoder, release), asCALL_THISCALL);
	// Registered as plain methods, not virtual properties: the surface being replaced was called as
	// enc.get_valid() and enc.set_bitrate(n), and a property registration would only answer to
	// enc.valid / enc.bitrate and break every existing caller.
	engine->RegisterObjectMethod("opus_encoder", "bool get_valid() const", asMETHOD(nvgt_opus_encoder, get_valid), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_encoder", "void set_bitrate(int bitrate)", asMETHOD(nvgt_opus_encoder, set_bitrate), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_encoder", "void set_complexity(int complexity)", asMETHOD(nvgt_opus_encoder, set_complexity), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_encoder", "void set_dtx(bool enabled)", asMETHOD(nvgt_opus_encoder, set_dtx), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_encoder", "void set_signal(int signal)", asMETHOD(nvgt_opus_encoder, set_signal), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_encoder", "void set_inband_fec(bool enabled)", asMETHOD(nvgt_opus_encoder, set_inband_fec), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_encoder", "string encode(const string&in pcm)", asMETHOD(nvgt_opus_encoder, encode), asCALL_THISCALL);

	ot = engine->RegisterObjectType("opus_decoder", 0, asOBJ_REF); assert(ot >= 0);
	engine->RegisterObjectBehaviour("opus_decoder", asBEHAVE_FACTORY, "opus_decoder@ d(int sample_rate, int channels)", asFUNCTION(create_opus_decoder), asCALL_CDECL);
	engine->RegisterObjectBehaviour("opus_decoder", asBEHAVE_ADDREF, "void f()", asMETHOD(nvgt_opus_decoder, duplicate), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("opus_decoder", asBEHAVE_RELEASE, "void f()", asMETHOD(nvgt_opus_decoder, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_decoder", "bool get_valid() const", asMETHOD(nvgt_opus_decoder, get_valid), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_decoder", "string decode(const string&in packet, int frame_size)", asMETHOD(nvgt_opus_decoder, decode), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_decoder", "string conceal(int frame_size)", asMETHOD(nvgt_opus_decoder, conceal), asCALL_THISCALL);
	engine->RegisterObjectMethod("opus_decoder", "void reset_state()", asMETHOD(nvgt_opus_decoder, reset_state), asCALL_THISCALL);
}
