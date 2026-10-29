/* nvsound_decode.cpp - decoders, playable sources and the background stream feeder for NVGT's sound engine.
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

#define NOMINMAX // Some of the codec headers pull in windows.h.
#include "nvsound.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <thread>
#include <dr_wav.h>
#include <dr_mp3.h>
#include <dr_flac.h>
#include <opus/opusfile.h>
#include <vorbis/vorbisfile.h>
#include <resample.h>

namespace nvsound {

void slider::slide(float target, uint64_t now, uint64_t duration) {
	if (duration == 0) {
		set(target);
		return;
	}
	from = value;
	to = target;
	start = now;
	end = now + duration;
	sliding = true;
}
float slider::at(uint64_t now) {
	if (!sliding) return value;
	if (now >= end) {
		value = to;
		sliding = false;
	} else value = from + (to - from) * float(double(now - start) / double(end - start));
	return value;
}

// An istream over a shared, immutable byte buffer, so that any number of sounds can decode the same preloaded file at once.
class shared_membuf : public std::streambuf {
	std::shared_ptr<const std::string> data;
public:
	shared_membuf(std::shared_ptr<const std::string> d) : data(std::move(d)) {
		char* b = const_cast<char*>(data->data());
		setg(b, b, b + data->size());
	}
protected:
	pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
		off_type base = dir == std::ios_base::beg ? 0 : dir == std::ios_base::cur ? gptr() - eback() : egptr() - eback();
		off_type p = base + off;
		if (p < 0 || p > egptr() - eback()) return pos_type(off_type(-1));
		setg(eback(), eback() + p, egptr());
		return pos_type(p);
	}
	pos_type seekpos(pos_type pos, std::ios_base::openmode which) override { return seekoff(off_type(pos), std::ios_base::beg, which); }
};
class shared_memstream : public std::istream {
	shared_membuf buf;
public:
	shared_memstream(std::shared_ptr<const std::string> d) : std::istream(nullptr), buf(std::move(d)) { rdbuf(&buf); }
};
std::unique_ptr<std::istream> open_memory_stream(std::shared_ptr<const std::string> data) { return std::make_unique<shared_memstream>(std::move(data)); }

// istream helpers shared by every decoder's callbacks. Reading to the end of a stream sets eofbit, which would make every later seek fail, so it is cleared on the spot.
static size_t stream_read(std::istream* s, void* out, size_t bytes) {
	s->read((char*)out, bytes);
	size_t got = s->gcount();
	if (!*s) s->clear();
	return got;
}
static bool stream_seek(std::istream* s, int64_t offset, int whence) {
	s->clear();
	s->seekg(offset, whence == SEEK_SET ? std::ios_base::beg : whence == SEEK_CUR ? std::ios_base::cur : std::ios_base::end);
	if (s->fail()) {
		s->clear();
		return false;
	}
	return true;
}
static int64_t stream_tell(std::istream* s) {
	std::streampos p = s->tellg();
	if (p == std::streampos(-1)) {
		s->clear();
		return -1;
	}
	return int64_t(p);
}

// dr_libs share a callback shape: read, seek with SET/CUR/END origins in that order, and tell.
template <typename origin_t> static int dr_whence(origin_t o) { return int(o) == 0 ? SEEK_SET : int(o) == 1 ? SEEK_CUR : SEEK_END; }
static size_t dr_read(void* ud, void* out, size_t bytes) { return stream_read((std::istream*)ud, out, bytes); }

class wav_decoder : public decoder {
	drwav wav;
	bool ok = false;
	static drwav_bool32 seek_cb(void* ud, int offset, drwav_seek_origin o) { return stream_seek((std::istream*)ud, offset, dr_whence(o)); }
	static drwav_bool32 tell_cb(void* ud, drwav_int64* cursor) { return (*cursor = stream_tell((std::istream*)ud)) >= 0; }
public:
	std::istream* in;
	wav_decoder(std::istream* in) : in(in) {}
	~wav_decoder() { if (ok) drwav_uninit(&wav); }
	bool open() override {
		ok = drwav_init(&wav, dr_read, seek_cb, tell_cb, in, nullptr);
		if (ok) {
			channels = wav.channels;
			sample_rate = wav.sampleRate;
		}
		return ok;
	}
	uint64_t read(float* out, uint64_t frames) override { return drwav_read_pcm_frames_f32(&wav, frames, out); }
	bool seek(uint64_t frame) override { return drwav_seek_to_pcm_frame(&wav, frame); }
	uint64_t length() override { return wav.totalPCMFrameCount; }
};

class mp3_decoder : public decoder {
	drmp3 mp3;
	bool ok = false;
	uint64_t len = 0;
	bool len_known = false;
	static drmp3_bool32 seek_cb(void* ud, int offset, drmp3_seek_origin o) { return stream_seek((std::istream*)ud, offset, dr_whence(o)); }
	static drmp3_bool32 tell_cb(void* ud, drmp3_int64* cursor) { return (*cursor = stream_tell((std::istream*)ud)) >= 0; }
public:
	std::istream* in;
	mp3_decoder(std::istream* in) : in(in) {}
	~mp3_decoder() { if (ok) drmp3_uninit(&mp3); }
	bool open() override {
		ok = drmp3_init(&mp3, dr_read, seek_cb, tell_cb, nullptr, in, nullptr);
		if (ok) {
			channels = mp3.channels;
			sample_rate = mp3.sampleRate;
		}
		return ok;
	}
	uint64_t read(float* out, uint64_t frames) override { return drmp3_read_pcm_frames_f32(&mp3, frames, out); }
	bool seek(uint64_t frame) override { return drmp3_seek_to_pcm_frame(&mp3, frame); }
	uint64_t length() override {
		// Counting an mp3's frames means scanning the whole file, so do it once and only when someone asks.
		if (!len_known) {
			len = drmp3_get_pcm_frame_count(&mp3);
			len_known = true;
		}
		return len;
	}
};

class flac_decoder : public decoder {
	drflac* flac = nullptr;
	static drflac_bool32 seek_cb(void* ud, int offset, drflac_seek_origin o) { return stream_seek((std::istream*)ud, offset, dr_whence(o)); }
	static drflac_bool32 tell_cb(void* ud, drflac_int64* cursor) { return (*cursor = stream_tell((std::istream*)ud)) >= 0; }
public:
	std::istream* in;
	flac_decoder(std::istream* in) : in(in) {}
	~flac_decoder() { if (flac) drflac_close(flac); }
	bool open() override {
		flac = drflac_open(dr_read, seek_cb, tell_cb, in, nullptr);
		if (flac) {
			channels = flac->channels;
			sample_rate = flac->sampleRate;
		}
		return flac != nullptr;
	}
	uint64_t read(float* out, uint64_t frames) override { return drflac_read_pcm_frames_f32(flac, frames, out); }
	bool seek(uint64_t frame) override { return drflac_seek_to_pcm_frame(flac, frame); }
	uint64_t length() override { return flac->totalPCMFrameCount; }
};

class vorbis_decoder : public decoder {
	OggVorbis_File vf;
	bool ok = false;
	static size_t read_cb(void* ptr, size_t size, size_t nmemb, void* ud) { return size ? stream_read((std::istream*)ud, ptr, size * nmemb) / size : 0; }
	static int seek_cb(void* ud, ogg_int64_t offset, int whence) { return stream_seek((std::istream*)ud, offset, whence) ? 0 : -1; }
	static long tell_cb(void* ud) { return long(stream_tell((std::istream*)ud)); }
public:
	std::istream* in;
	vorbis_decoder(std::istream* in) : in(in) {}
	~vorbis_decoder() { if (ok) ov_clear(&vf); }
	bool open() override {
		ov_callbacks cb{read_cb, seek_cb, nullptr, tell_cb};
		ok = ov_open_callbacks(in, &vf, nullptr, 0, cb) == 0;
		if (!ok) return false;
		vorbis_info* info = ov_info(&vf, -1);
		channels = info->channels;
		sample_rate = info->rate;
		return true;
	}
	uint64_t read(float* out, uint64_t frames) override {
		uint64_t done = 0;
		while (done < frames) {
			float** pcm;
			int bitstream;
			long n = ov_read_float(&vf, &pcm, int(std::min<uint64_t>(frames - done, 4096)), &bitstream);
			if (n == OV_HOLE) continue;
			if (n <= 0) break;
			// A chained stream can change its channel count between links; keep the layout we announced and fill what is missing with silence.
			int link_channels = ov_info(&vf, bitstream)->channels;
			for (long f = 0; f < n; f++) {
				for (unsigned int c = 0; c < channels; c++) out[(done + f) * channels + c] = int(c) < link_channels ? pcm[c][f] : 0.0f;
			}
			done += n;
		}
		return done;
	}
	bool seek(uint64_t frame) override { return ov_pcm_seek(&vf, ogg_int64_t(frame)) == 0; }
	uint64_t length() override {
		ogg_int64_t l = ov_pcm_total(&vf, -1);
		return l > 0 ? uint64_t(l) : 0;
	}
};

class opus_decoder_impl : public decoder {
	OggOpusFile* of = nullptr;
	static int read_cb(void* ud, unsigned char* ptr, int bytes) { return int(stream_read((std::istream*)ud, ptr, bytes)); }
	static int seek_cb(void* ud, opus_int64 offset, int whence) { return stream_seek((std::istream*)ud, offset, whence) ? 0 : -1; }
	static opus_int64 tell_cb(void* ud) { return stream_tell((std::istream*)ud); }
public:
	std::istream* in;
	opus_decoder_impl(std::istream* in) : in(in) {}
	~opus_decoder_impl() { if (of) op_free(of); }
	bool open() override {
		OpusFileCallbacks cb{read_cb, seek_cb, tell_cb, nullptr};
		int err = 0;
		of = op_open_callbacks(in, &cb, nullptr, 0, &err);
		if (!of) return false;
		// Opus always decodes at 48 kHz; asking for stereo keeps every link of a chained stream the same shape.
		channels = 2;
		sample_rate = 48000;
		return true;
	}
	uint64_t read(float* out, uint64_t frames) override {
		uint64_t done = 0;
		while (done < frames) {
			int n = op_read_float_stereo(of, out + done * 2, int(std::min<uint64_t>(frames - done, 8192) * 2));
			if (n == OP_HOLE) continue;
			if (n <= 0) break;
			done += n;
		}
		return done;
	}
	bool seek(uint64_t frame) override { return op_pcm_seek(of, ogg_int64_t(frame)) == 0; }
	uint64_t length() override {
		ogg_int64_t l = op_pcm_total(of, -1);
		return l > 0 ? uint64_t(l) : 0;
	}
};

// Picks a decoder by looking at the first bytes of the stream, then falls back to trying each one in turn. Mp3 always goes last, because its frame sync can be found by chance in data that isn't mp3 at all.
std::unique_ptr<decoder> open_decoder(std::unique_ptr<std::istream> stream) {
	if (!stream) return nullptr;
	char head[64] = {0};
	size_t got = stream_read(stream.get(), head, sizeof(head));
	std::string h(head, got);
	enum kind { wav, flac, vorbis, opus, mp3 };
	std::vector<kind> order;
	auto has = [&](const char* s) { return h.find(s) != std::string::npos; };
	if (h.compare(0, 4, "RIFF") == 0 || h.compare(0, 4, "RIFX") == 0 || h.compare(0, 4, "RF64") == 0 || h.compare(0, 4, "FORM") == 0 || h.compare(0, 4, "riff") == 0) order.push_back(wav);
	else if (h.compare(0, 4, "fLaC") == 0) order.push_back(flac);
	else if (h.compare(0, 4, "OggS") == 0) {
		if (has("OpusHead")) order.push_back(opus);
		else if (has("\x01vorbis")) order.push_back(vorbis);
		else if (has("FLAC")) order.push_back(flac);
	}
	for (kind k : {wav, flac, vorbis, opus, mp3}) {
		if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
	}
	// Trying each decoder means rewinding between attempts. A stream that can't rewind (some filters can't) is read into memory first, putting back the bytes already taken.
	if (!stream_seek(stream.get(), 0, SEEK_SET)) {
		auto data = std::make_shared<std::string>(h);
		char buf[65536];
		while (size_t n = stream_read(stream.get(), buf, sizeof(buf))) data->append(buf, n);
		stream = open_memory_stream(data);
	}
	for (kind k : order) {
		if (!stream_seek(stream.get(), 0, SEEK_SET)) return nullptr;
		std::unique_ptr<decoder> d;
		switch (k) {
			case wav: d = std::make_unique<wav_decoder>(stream.get()); break;
			case flac: d = std::make_unique<flac_decoder>(stream.get()); break;
			case vorbis: d = std::make_unique<vorbis_decoder>(stream.get()); break;
			case opus: d = std::make_unique<opus_decoder_impl>(stream.get()); break;
			case mp3: d = std::make_unique<mp3_decoder>(stream.get()); break;
		}
		if (d->open() && d->channels > 0 && d->channels <= max_source_channels && d->sample_rate > 0) {
			d->stream = std::move(stream);
			return d;
		}
	}
	return nullptr;
}

// Lets a stream that can't seek, such as a live network response, be rewound over its first bytes while its format is worked out. Everything read is recorded until the window is full; from then on reads pass straight through and only the current position can be asked for. The source is read on the caller's thread and isn't owned.
class window_streambuf : public std::streambuf {
	std::istream* src;
	std::string seen;
	size_t cap, pos = 0; // pos is how far the consumer has been handed data, counting what sits unread in buf.
	bool closed = false;
	char buf[4096];
	std::streampos current() const { return std::streampos(std::streamoff(pos - (egptr() - gptr()))); }
protected:
	int_type underflow() override {
		size_t n;
		if (pos < seen.size()) {
			n = std::min(sizeof(buf), seen.size() - pos);
			memcpy(buf, seen.data() + pos, n);
		} else {
			src->read(buf, sizeof(buf));
			n = size_t(src->gcount());
			if (!n) return traits_type::eof();
			if (!closed && seen.size() + n <= cap) seen.append(buf, n);
			else closed = true;
		}
		pos += n;
		setg(buf, buf, buf + n);
		return traits_type::to_int_type(buf[0]);
	}
	std::streampos seekoff(std::streamoff off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
		std::streamoff here = std::streamoff(current());
		if (dir == std::ios_base::cur && off == 0) return here;
		if (closed || dir == std::ios_base::end) return std::streampos(std::streamoff(-1));
		std::streamoff target = dir == std::ios_base::beg ? off : here + off;
		if (target < 0) return std::streampos(std::streamoff(-1));
		// Going forward past what has been recorded means reading up to it.
		while (size_t(target) > seen.size() && !closed) {
			pos = seen.size();
			setg(buf, buf, buf);
			if (underflow() == traits_type::eof()) return std::streampos(std::streamoff(-1));
		}
		if (closed) return std::streampos(std::streamoff(-1));
		pos = size_t(target);
		setg(buf, buf, buf);
		return std::streampos(target);
	}
	std::streampos seekpos(std::streampos p, std::ios_base::openmode which) override { return seekoff(std::streamoff(p), std::ios_base::beg, which); }
public:
	window_streambuf(std::istream* source, size_t window) : src(source), cap(window) {}
};
class window_istream : public std::istream {
	window_streambuf sb;
public:
	window_istream(std::istream* source, size_t window) : std::istream(nullptr), sb(source, window) { rdbuf(&sb); }
};
std::unique_ptr<std::istream> open_window_stream(std::istream* source, size_t window) { return std::make_unique<window_istream>(source, window); }

std::vector<float> resample_interleaved(const float* in, uint64_t frames, unsigned int channels, unsigned int rate_in, unsigned int rate_out, uint64_t& frames_out) {
	frames_out = uint64_t((double(frames) * rate_out) / rate_in + 0.5);
	std::vector<float> out(frames_out * channels);
	WDL_Resampler r;
	r.SetMode(false, 0, true, 64, 32);
	r.SetRates(rate_in, rate_out);
	uint64_t consumed = 0, produced = 0;
	while (produced < frames_out) {
		int want = int(std::min<uint64_t>(frames_out - produced, 4096));
		WDL_ResampleSample* inbuf;
		int need = r.ResamplePrepare(want, channels, &inbuf);
		// Past the end of the input we keep feeding silence, which flushes the filter's tail out.
		uint64_t avail = consumed < frames ? std::min<uint64_t>(need, frames - consumed) : 0;
		if (avail) memcpy(inbuf, in + consumed * channels, avail * channels * sizeof(float));
		if (avail < uint64_t(need)) memset(inbuf + avail * channels, 0, (need - avail) * channels * sizeof(float));
		consumed += avail;
		int n = r.ResampleOut(out.data() + produced * channels, need, want, channels);
		if (n <= 0) break;
		produced += n;
	}
	if (produced < frames_out) memset(out.data() + produced * channels, 0, (frames_out - produced) * channels * sizeof(float));
	return out;
}

class pcm_source : public source {
	std::shared_ptr<const pcm_data> data;
	std::atomic<uint64_t> pos{0};
	std::atomic<bool> loop{false};
public:
	pcm_source(std::shared_ptr<const pcm_data> d) : data(std::move(d)) {}
	unsigned int channels() const override { return data->channels; }
	unsigned int sample_rate() const override { return data->sample_rate; }
	uint64_t read(float* out, uint64_t frames, bool& ended) override {
		unsigned int ch = data->channels;
		uint64_t start = pos.load(), p = start, done = 0;
		while (done < frames) {
			if (p >= data->frames) {
				if (!loop || data->frames == 0) break;
				p = 0;
			}
			uint64_t n = std::min(frames - done, data->frames - p);
			memcpy(out + done * ch, data->samples.data() + p * ch, n * ch * sizeof(float));
			done += n;
			p += n;
		}
		if (done < frames) {
			memset(out + done * ch, 0, (frames - done) * ch * sizeof(float));
			ended = true;
		}
		// If the script seeked while this block was being read, its seek wins.
		pos.compare_exchange_strong(start, p);
		return frames;
	}
	bool seek(uint64_t frame) override {
		pos = std::min(frame, data->frames);
		return true;
	}
	uint64_t position() override { return pos; }
	uint64_t length() override { return data->frames; }
	void set_loop(bool l) override { loop = l; }
};

// Streams decode on a background thread into a ring buffer, so the audio thread never waits on a file read or a slow decode.
class stream_source : public source {
	std::unique_ptr<decoder> dec;
	std::mutex dec_mutex; // Owned by whoever is touching the decoder: a feeding thread, or a seek from the script thread.
	std::mutex ring_mutex;
	std::vector<float> ring;
	uint64_t capacity, rpos = 0, wpos = 0, fill = 0; // In frames.
	int64_t written_pos = 0; // The decoder position at the end of the newest frame in the ring.
	std::atomic<bool> eof{false}, loop{false};
	uint64_t len;
	unsigned int ch, rate;
	// Only for streams with a thread of their own.
	bool dedicated;
	std::function<void()> abort;
	std::thread thread;
	std::atomic<bool> stopping{false};
	std::mutex wake_mutex;
	std::condition_variable wake;
	void write_ring(const float* in, uint64_t frames) {
		std::lock_guard<std::mutex> l(ring_mutex);
		for (uint64_t done = 0; done < frames;) {
			uint64_t n = std::min(frames - done, capacity - wpos);
			memcpy(ring.data() + wpos * ch, in + done * ch, n * ch * sizeof(float));
			wpos = (wpos + n) % capacity;
			done += n;
		}
		fill += frames;
		written_pos += frames;
	}
	uint64_t free_space() {
		std::lock_guard<std::mutex> l(ring_mutex);
		return capacity - fill;
	}
	void thread_loop() {
		while (!stopping) {
			if (feed(true)) continue;
			std::unique_lock<std::mutex> l(wake_mutex);
			wake.wait_for(l, std::chrono::milliseconds(10), [this] { return stopping.load(); });
		}
	}
public:
	stream_source(std::unique_ptr<decoder> d, bool dedicated, std::function<void()> abort) : dec(std::move(d)), dedicated(dedicated), abort(std::move(abort)) {
		ch = dec->channels;
		rate = dec->sample_rate;
		len = dedicated ? 0 : dec->length(); // Asking a network or pushed stream for its length could mean reading all of it.
		capacity = std::max<uint64_t>(rate, 16384); // About a second of audio.
		ring.resize(capacity * ch);
	}
	~stream_source() {
		if (!thread.joinable()) return;
		stopping = true;
		if (abort) abort(); // Frees the thread if it is waiting inside a read.
		wake.notify_one();
		thread.join();
	}
	void start_thread() { thread = std::thread(&stream_source::thread_loop, this); }
	unsigned int channels() const override { return ch; }
	unsigned int sample_rate() const override { return rate; }
	// Decodes one chunk if there is room. Returns true if it did anything, so a feeding thread knows whether to sleep.
	bool feed(bool blocking = false) {
		std::unique_lock<std::mutex> dl(dec_mutex, std::defer_lock);
		if (blocking) dl.lock();
		else if (!dl.try_lock()) return false;
		if (eof || stopping) return false;
		uint64_t space = free_space();
		if (space < 1024) return false;
		uint64_t want = std::min<uint64_t>(space, 4096);
		thread_local std::vector<float> tmp;
		tmp.resize(want * ch);
		uint64_t got = dec->read(tmp.data(), want);
		if (got == 0) {
			std::unique_lock<std::mutex> l(ring_mutex);
			// written_pos of 0 means nothing was decoded since the last wrap, so this stream is empty and looping it would spin forever.
			if (loop && !dedicated && written_pos > 0 && dec->seek(0)) {
				written_pos = 0;
				return true;
			}
			l.unlock();
			eof = true;
			return false;
		}
		write_ring(tmp.data(), got);
		return true;
	}
	void prefill(uint64_t frames) {
		uint64_t start = 0;
		while (start < frames && feed(true)) {
			std::lock_guard<std::mutex> l(ring_mutex);
			start = fill;
		}
	}
	uint64_t read(float* out, uint64_t frames, bool& ended) override {
		std::lock_guard<std::mutex> l(ring_mutex);
		uint64_t n = std::min(frames, fill);
		for (uint64_t done = 0; done < n;) {
			uint64_t c = std::min(n - done, capacity - rpos);
			memcpy(out + done * ch, ring.data() + rpos * ch, c * ch * sizeof(float));
			rpos = (rpos + c) % capacity;
			done += c;
		}
		fill -= n;
		if (n < frames) {
			memset(out + n * ch, 0, (frames - n) * ch * sizeof(float));
			// Running dry before the end is an underrun, which plays as silence. Running dry after the end is the end.
			if (eof) ended = true;
		}
		return frames;
	}
	bool seek(uint64_t frame) override {
		if (dedicated) return false; // Its thread may be waiting inside the decoder, and a socket can't rewind anyway.
		{
			std::lock_guard<std::mutex> dl(dec_mutex);
			if (!dec->seek(frame)) return false;
			std::lock_guard<std::mutex> l(ring_mutex);
			rpos = wpos = fill = 0;
			written_pos = int64_t(frame);
			eof = false;
		}
		prefill(rate / 10); // Without this the sound would play silence after a seek until the feeder next came around.
		return true;
	}
	uint64_t position() override {
		std::lock_guard<std::mutex> l(ring_mutex);
		int64_t p = written_pos - int64_t(fill);
		if (p < 0 && len > 0) p += len; // The ring still holds audio from before a loop wrapped around.
		return p < 0 ? 0 : uint64_t(p);
	}
	uint64_t length() override { return len; }
	void set_loop(bool l) override {
		loop = l;
		// A stream that hit the end can start feeding again once it is asked to loop.
		if (l && eof && !dedicated) {
			std::lock_guard<std::mutex> dl(dec_mutex);
			if (dec->seek(0)) {
				std::lock_guard<std::mutex> rl(ring_mutex);
				written_pos = 0;
				eof = false;
			}
		}
	}
};

// One thread services every ordinary stream, topping each ring up in turn.
static std::mutex g_feeder_mutex;
static std::condition_variable g_feeder_cv;
static std::vector<std::weak_ptr<stream_source>> g_feeder_streams;
static std::thread g_feeder_thread;
static bool g_feeder_stop = false;

static void feeder_loop() {
	std::vector<std::shared_ptr<stream_source>> live;
	while (true) {
		{
			std::lock_guard<std::mutex> l(g_feeder_mutex);
			if (g_feeder_stop) return;
			live.clear();
			auto it = g_feeder_streams.begin();
			while (it != g_feeder_streams.end()) {
				if (auto s = it->lock()) {
					live.push_back(s);
					++it;
				} else it = g_feeder_streams.erase(it);
			}
		}
		bool worked = false;
		for (auto& s : live) worked |= s->feed();
		live.clear(); // Don't keep a stream alive while sleeping.
		if (!worked) {
			std::unique_lock<std::mutex> l(g_feeder_mutex);
			g_feeder_cv.wait_for(l, std::chrono::milliseconds(10), [] { return g_feeder_stop; });
		}
	}
}

std::shared_ptr<source> make_stream_source(std::unique_ptr<decoder> dec, bool dedicated_thread, std::function<void()> abort, uint64_t prefill_frames) {
	auto s = std::make_shared<stream_source>(std::move(dec), dedicated_thread, std::move(abort));
	// A quarter second up front by default, so playback can start the moment it is asked to.
	s->prefill(prefill_frames == ~uint64_t(0) ? s->sample_rate() / 4 : prefill_frames);
	if (dedicated_thread) {
		s->start_thread();
		return s;
	}
	std::lock_guard<std::mutex> l(g_feeder_mutex);
	g_feeder_streams.push_back(s);
	if (!g_feeder_thread.joinable()) {
		g_feeder_stop = false;
		g_feeder_thread = std::thread(feeder_loop);
	}
	g_feeder_cv.notify_one();
	return s;
}

void stop_stream_feeder() {
	{
		std::lock_guard<std::mutex> l(g_feeder_mutex);
		g_feeder_stop = true;
	}
	g_feeder_cv.notify_one();
	if (g_feeder_thread.joinable()) g_feeder_thread.join();
}

// Pushed PCM arrives in chunks of whatever size the script pushes. Each push is copied before taking the lock, so the audio thread only ever waits for a vector to be moved.
class pcm_push_source : public source {
	std::mutex m;
	std::deque<std::vector<float>> chunks;
	size_t front_pos = 0; // Samples already played from the front chunk.
	uint64_t buffered = 0, consumed = 0; // In frames.
	bool ending = false;
	unsigned int ch, rate;
public:
	pcm_push_source(unsigned int rate, unsigned int channels) : ch(channels), rate(rate) {}
	void push(const float* samples, size_t count) {
		count -= count % ch; // A partial frame can't be played.
		if (!count) return;
		std::vector<float> chunk(samples, samples + count);
		std::lock_guard<std::mutex> l(m);
		chunks.push_back(std::move(chunk));
		buffered += count / ch;
	}
	void end() {
		std::lock_guard<std::mutex> l(m);
		ending = true;
	}
	// Keeps only the next fade_frames of what is waiting, faded out to silence, so stopping mid-word doesn't click.
	void clear(uint64_t fade_frames) {
		std::lock_guard<std::mutex> l(m);
		std::vector<float> tail;
		tail.reserve(std::min<uint64_t>(fade_frames, buffered) * ch);
		for (size_t i = 0; i < chunks.size() && tail.size() < fade_frames * ch; i++) {
			const std::vector<float>& c = chunks[i];
			size_t from = i == 0 ? front_pos : 0, n = std::min<size_t>(c.size() - from, fade_frames * ch - tail.size());
			tail.insert(tail.end(), c.begin() + from, c.begin() + from + n);
		}
		size_t frames = tail.size() / ch;
		for (size_t f = 0; f < frames; f++) {
			float g = 1.0f - float(f + 1) / float(frames);
			for (unsigned int c = 0; c < ch; c++) tail[f * ch + c] *= g;
		}
		chunks.clear();
		front_pos = 0;
		buffered = frames;
		if (frames) chunks.push_back(std::move(tail));
	}
	uint64_t queued() {
		std::lock_guard<std::mutex> l(m);
		return buffered;
	}
	unsigned int channels() const override { return ch; }
	unsigned int sample_rate() const override { return rate; }
	uint64_t read(float* out, uint64_t frames, bool& ended) override {
		std::lock_guard<std::mutex> l(m);
		uint64_t done = 0;
		while (done < frames && !chunks.empty()) {
			std::vector<float>& c = chunks.front();
			size_t n = std::min<size_t>((frames - done) * ch, c.size() - front_pos);
			memcpy(out + done * ch, c.data() + front_pos, n * sizeof(float));
			front_pos += n;
			done += n / ch;
			if (front_pos >= c.size()) {
				chunks.pop_front();
				front_pos = 0;
			}
		}
		buffered -= done;
		consumed += done;
		if (done < frames) {
			memset(out + done * ch, 0, (frames - done) * ch * sizeof(float));
			if (ending) ended = true; // Otherwise the script just hasn't pushed more yet, and we play silence until it does.
		}
		return frames;
	}
	bool seek(uint64_t) override { return false; }
	uint64_t position() override {
		std::lock_guard<std::mutex> l(m);
		return consumed;
	}
	// Under BASS a push stream's length was however much audio was waiting to be played.
	uint64_t length() override {
		std::lock_guard<std::mutex> l(m);
		return buffered;
	}
	void set_loop(bool) override {}
};
std::shared_ptr<pcm_push_source> make_pcm_push_source(unsigned int sample_rate, unsigned int channels) { return std::make_shared<pcm_push_source>(sample_rate, channels); }
void pcm_push(pcm_push_source* s, const float* samples, size_t count) { s->push(samples, count); }
void pcm_push_end(pcm_push_source* s) { s->end(); }
void pcm_push_clear(pcm_push_source* s, uint64_t fade_frames) { s->clear(fade_frames); }
uint64_t pcm_push_queued(pcm_push_source* s) { return s->queued(); }
std::shared_ptr<source> as_source(std::shared_ptr<pcm_push_source> s) { return s; }

// The bytes of an encoded file pushed in pieces. Everything pushed is kept, as BASS kept it, which is what lets the decoder probe rewind to the start.
struct push_bytes {
	std::mutex m;
	std::condition_variable more;
	std::string data;
	bool ended = false, aborted = false, blocking = false;
};
class push_streambuf : public std::streambuf {
	std::shared_ptr<push_bytes> p;
	size_t pos = 0; // Offset in p->data of the end of what is in buf.
	char buf[4096];
protected:
	int_type underflow() override {
		std::unique_lock<std::mutex> l(p->m);
		if (p->blocking) p->more.wait(l, [this] { return pos < p->data.size() || p->ended || p->aborted; });
		if (p->aborted || pos >= p->data.size()) return traits_type::eof();
		size_t n = std::min(sizeof(buf), p->data.size() - pos);
		memcpy(buf, p->data.data() + pos, n);
		pos += n;
		setg(buf, buf, buf + n);
		return traits_type::to_int_type(buf[0]);
	}
	pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode) override {
		std::lock_guard<std::mutex> l(p->m);
		off_type cur = off_type(pos) - (egptr() - gptr());
		off_type target = dir == std::ios_base::beg ? off : dir == std::ios_base::cur ? cur + off : off_type(p->data.size()) + off;
		if (target < 0 || target > off_type(p->data.size())) return pos_type(off_type(-1));
		pos = size_t(target);
		setg(buf, buf, buf);
		return pos_type(target);
	}
	pos_type seekpos(pos_type sp, std::ios_base::openmode which) override { return seekoff(off_type(sp), std::ios_base::beg, which); }
public:
	push_streambuf(std::shared_ptr<push_bytes> p) : p(std::move(p)) { setg(buf, buf, buf); }
};
class push_istream : public std::istream {
	push_streambuf sb;
public:
	push_istream(std::shared_ptr<push_bytes> p) : std::istream(nullptr), sb(std::move(p)) { rdbuf(&sb); }
};
std::shared_ptr<push_bytes> make_push_bytes() { return std::make_shared<push_bytes>(); }
void push_bytes_append(push_bytes* p, const char* data, size_t size, bool end) {
	{
		std::lock_guard<std::mutex> l(p->m);
		p->data.append(data, size);
		if (end) p->ended = true;
	}
	p->more.notify_all();
}
size_t push_bytes_size(push_bytes* p) {
	std::lock_guard<std::mutex> l(p->m);
	return p->data.size();
}
std::unique_ptr<std::istream> open_push_stream(std::shared_ptr<push_bytes> p) { return std::make_unique<push_istream>(std::move(p)); }
void set_push_blocking(push_bytes* p) {
	std::lock_guard<std::mutex> l(p->m);
	p->blocking = true;
}
void abort_push_bytes(push_bytes* p) {
	{
		std::lock_guard<std::mutex> l(p->m);
		p->aborted = true;
	}
	p->more.notify_all();
}

std::shared_ptr<source> make_pcm_source(std::shared_ptr<const pcm_data> data) { return std::make_shared<pcm_source>(std::move(data)); }

} // namespace nvsound
