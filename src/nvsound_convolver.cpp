/* nvsound_convolver.cpp - uniformly partitioned FFT convolution, for mixer.set_impulse_response.
 * The algorithm is the one in dep/ma_convolution_node.c (see doc/convolution_reverb_rationale.md), moved out of its MiniAudio node unchanged: overlap-save with a frequency domain delay line, an optional reduced rate wet path, and an idle mode once the tail has died away.
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

#include "nvsound_convolver.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#define NVSOUND_CONVOLVER_SSE 1
#endif

namespace nvsound {

static const double conv_pi = 3.14159265358979323846;
static const uint64_t conv_max_bytes = uint64_t(1024) * 1024 * 1024; // Past this the response is almost certainly a mistake.

// Transposed direct form II biquad.
struct conv_biquad {
	float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
	void lowpass(double fs, double fc, double q) {
		double w0 = 2.0 * conv_pi * fc / fs, cw = cos(w0), alpha = sin(w0) / (2.0 * q), a0 = 1.0 + alpha;
		b0 = float((1.0 - cw) * 0.5 / a0);
		b1 = float((1.0 - cw) / a0);
		b2 = b0;
		a1 = float(-2.0 * cw / a0);
		a2 = float((1.0 - alpha) / a0);
		z1 = z2 = 0;
	}
	float run(float x) {
		float y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
#ifndef NVSOUND_CONVOLVER_SSE
		// A dying tail would otherwise sink into denormal range. SSE builds flush denormals for the whole process call instead.
		if (z1 > -1e-15f && z1 < 1e-15f) z1 = 0;
		if (z2 > -1e-15f && z2 < 1e-15f) z2 = 0;
#endif
		return y;
	}
};
// Sixth order Butterworth low-pass: three biquads at the Qs of its pole pairs.
struct conv_lowpass6 {
	conv_biquad s[3];
	void set(double fs, double fc) {
		static const double q[3] = {0.51763809, 0.70710678, 1.93185165};
		for (int i = 0; i < 3; i++) s[i].lowpass(fs, fc, q[i]);
	}
	float run(float x) { return s[2].run(s[1].run(s[0].run(x))); }
	void reset() {
		for (conv_biquad& b : s) b.z1 = b.z2 = 0;
	}
};

// Everything one loaded impulse response needs, built off the audio thread and swapped in with one pointer write. Per channel arrays are laid out channel after channel.
struct convolver::state {
	unsigned int channels, div, B, N, P, span; // Rate divisor, block length at the reduced rate, FFT size (2B), partitions, device frames per block (B * div).
	unsigned int pos = 0, bpos = 0, phase = 0; // Newest slot in the delay line, reduced rate position in the block being filled, which of the div input frames this is.
	uint64_t tail_frames, idle_frames = 0; // Device frames of silent input it takes the delay line to hold only zeros.
	bool asleep = false;
	std::vector<unsigned int> rev;
	std::vector<float> cos_t, sin_t, h_re, h_im, x_re, x_im, prev, in_blk, wet, y_re, y_im;
	std::vector<conv_lowpass6> aa, ai; // Anti-alias before decimating, anti-image after zero-stuffing.
	// Radix-2 FFT over separate real and imaginary arrays.
	void fft(float* re, float* im, bool inverse) const {
		float sign = inverse ? 1.0f : -1.0f;
		for (unsigned int i = 1; i < N; i++) {
			unsigned int j = rev[i];
			if (i < j) {
				std::swap(re[i], re[j]);
				std::swap(im[i], im[j]);
			}
		}
		for (unsigned int len = 2; len <= N; len <<= 1) {
			unsigned int half = len >> 1, step = N / len;
			for (unsigned int k = 0; k < half; k++) {
				float wr = cos_t[k * step], wi = sign * sin_t[k * step];
				for (unsigned int a = k; a < N; a += len) {
					unsigned int b = a + half;
					float vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
					re[b] = re[a] - vr;
					im[b] = im[a] - vi;
					re[a] += vr;
					im[a] += vi;
				}
			}
		}
		if (inverse) {
			float inv = 1.0f / float(N);
			for (unsigned int i = 0; i < N; i++) {
				re[i] *= inv;
				im[i] *= inv;
			}
		}
	}
	// y += x * h, bin by bin. This is where nearly all of a convolver's time goes.
	static void accumulate(const float* xr, const float* xi, const float* hr, const float* hi, float* yr, float* yi, unsigned int n) {
		unsigned int i = 0;
#ifdef NVSOUND_CONVOLVER_SSE
		for (; i + 4 <= n; i += 4) {
			__m128 a = _mm_loadu_ps(xr + i), b = _mm_loadu_ps(xi + i), c = _mm_loadu_ps(hr + i), d = _mm_loadu_ps(hi + i);
			_mm_storeu_ps(yr + i, _mm_add_ps(_mm_loadu_ps(yr + i), _mm_sub_ps(_mm_mul_ps(a, c), _mm_mul_ps(b, d))));
			_mm_storeu_ps(yi + i, _mm_add_ps(_mm_loadu_ps(yi + i), _mm_add_ps(_mm_mul_ps(a, d), _mm_mul_ps(b, c))));
		}
#endif
		for (; i < n; i++) {
			yr[i] += xr[i] * hr[i] - xi[i] * hi[i];
			yi[i] += xr[i] * hi[i] + xi[i] * hr[i];
		}
	}
	// Convolves the completed block of every channel and replaces wet with a span of wet per channel.
	void render() {
		uint64_t history = uint64_t(P) * N;
		for (unsigned int c = 0; c < channels; c++) {
			float* xre = x_re.data() + c * history;
			float* xim = x_im.data() + c * history;
			const float* hre = h_re.data() + c * history;
			const float* him = h_im.data() + c * history;
			float* p_prev = prev.data() + uint64_t(c) * B;
			float* p_in = in_blk.data() + uint64_t(c) * B;
			float* p_wet = wet.data() + uint64_t(c) * span;
			float* xr = xre + uint64_t(pos) * N;
			float* xi = xim + uint64_t(pos) * N;
			// The newest spectrum is built straight in its slot of the history: the previous block, then this one.
			memcpy(xr, p_prev, sizeof(float) * B);
			memcpy(xr + B, p_in, sizeof(float) * B);
			memset(xi, 0, sizeof(float) * N);
			memcpy(p_prev, p_in, sizeof(float) * B);
			fft(xr, xi, false);
			std::fill(y_re.begin(), y_re.end(), 0.0f);
			std::fill(y_im.begin(), y_im.end(), 0.0f);
			for (unsigned int p = 0; p < P; p++) {
				uint64_t slot = (pos + P - p) % P;
				accumulate(xre + slot * N, xim + slot * N, hre + uint64_t(p) * N, him + uint64_t(p) * N, y_re.data(), y_im.data(), N);
			}
			fft(y_re.data(), y_im.data(), true);
			const float* out = y_re.data() + (N - B); // Overlap-save: only the last B are valid.
			if (div == 1) memcpy(p_wet, out, sizeof(float) * B);
			else {
				// Zero-stuff back up to the device rate, scaled by div to make up for the zeros, and filter out the images.
				float g = float(div);
				unsigned int w = 0;
				for (unsigned int k = 0; k < B; k++) {
					p_wet[w++] = ai[c].run(out[k] * g);
					for (unsigned int z = 1; z < div; z++) p_wet[w++] = ai[c].run(0.0f);
				}
			}
		}
		pos = (pos + 1) % P;
	}
	// Called once the input has been silent for longer than the tail. Clearing everything is exact: a history of silent blocks is all zeros anyway.
	void go_idle() {
		std::fill(x_re.begin(), x_re.end(), 0.0f);
		std::fill(x_im.begin(), x_im.end(), 0.0f);
		std::fill(prev.begin(), prev.end(), 0.0f);
		std::fill(in_blk.begin(), in_blk.end(), 0.0f);
		std::fill(wet.begin(), wet.end(), 0.0f);
		for (unsigned int c = 0; c < channels; c++) {
			aa[c].reset();
			ai[c].reset();
		}
		asleep = true;
	}
};

convolver::convolver(unsigned int channels, unsigned int sample_rate, unsigned int partition_size) : channels(channels), sample_rate(sample_rate), partition_size(partition_size) {}
convolver::~convolver() {}

bool convolver::set_ir(const float* frames, uint64_t frame_count, unsigned int ir_channels, float tail_seconds, unsigned int rate_divisor) {
	if (!frames || frame_count == 0 || ir_channels == 0) return false;
	double fs = sample_rate;
	unsigned int div = rate_divisor < 1 ? 1 : rate_divisor;
	if ((div & (div - 1)) != 0 || partition_size / div < 8) return false;
	unsigned int B = partition_size / div, N = B * 2;
	double fc = 0.8 * fs / (2.0 * div);
	uint64_t L = frame_count, fade = 0;
	bool cut = false;
	if (tail_seconds > 0) {
		uint64_t max_len = std::max<uint64_t>(uint64_t(fs * tail_seconds), 1);
		if (L > max_len) {
			L = max_len;
			cut = true;
			fade = std::min<uint64_t>(std::max<uint64_t>(uint64_t(fs * 0.01), 1), L); // About 10 ms, so the cut does not click.
		}
	}
	uint64_t Ld = (L + div - 1) / div;
	if ((Ld + B - 1) / B > 0xFFFFFFu) return false;
	unsigned int P = std::max<unsigned int>(unsigned((Ld + B - 1) / B), 1);
	uint64_t history = uint64_t(P) * N * channels;
	if (history * sizeof(float) * 4 > conv_max_bytes) return false;
	auto s = std::make_unique<state>();
	s->channels = channels;
	s->div = div;
	s->B = B;
	s->N = N;
	s->P = P;
	s->span = B * div;
	s->tail_frames = (uint64_t(P) + 2) * s->span;
	s->rev.assign(N, 0);
	s->cos_t.resize(N / 2);
	s->sin_t.resize(N / 2);
	for (auto* v : {&s->h_re, &s->h_im, &s->x_re, &s->x_im}) v->assign(size_t(history), 0.0f);
	s->prev.assign(size_t(B) * channels, 0.0f);
	s->in_blk.assign(size_t(B) * channels, 0.0f);
	s->wet.assign(size_t(B) * div * channels, 0.0f);
	s->y_re.assign(N, 0.0f);
	s->y_im.assign(N, 0.0f);
	s->aa.resize(channels);
	s->ai.resize(channels);
	for (unsigned int k = 1, j = 0; k < N; k++) {
		unsigned int bit = N >> 1;
		for (; j & bit; bit >>= 1) j ^= bit;
		j ^= bit;
		s->rev[k] = j;
	}
	for (unsigned int k = 0; k < N / 2; k++) {
		double a = 2.0 * conv_pi * double(k) / double(N);
		s->cos_t[k] = float(cos(a));
		s->sin_t[k] = float(sin(a));
	}
	std::vector<float> tmp(static_cast<size_t>(L));
	unsigned int prev_src = 0xFFFFFFFF;
	for (unsigned int c = 0; c < channels; c++) {
		// A mono response is broadcast to every channel; otherwise the last available response channel covers any channel past its end.
		unsigned int src = ir_channels == 1 ? 0 : (c < ir_channels ? c : ir_channels - 1);
		float* hre = s->h_re.data() + uint64_t(c) * P * N;
		float* him = s->h_im.data() + uint64_t(c) * P * N;
		if (src == prev_src) {
			memcpy(hre, hre - uint64_t(P) * N, sizeof(float) * size_t(P) * N);
			memcpy(him, him - uint64_t(P) * N, sizeof(float) * size_t(P) * N);
			continue;
		}
		prev_src = src;
		for (uint64_t i = 0; i < L; i++) tmp[i] = frames[i * ir_channels + src];
		if (cut) {
			for (uint64_t i = 0; i < fade; i++) tmp[L - fade + i] *= 1.0f - float(i) / float(fade);
		}
		if (div > 1) {
			// Low-passed forwards and back so the response keeps its timing, then every div-th sample, scaled by div so the sum over the sparser response comes out the same size.
			conv_lowpass6 lp;
			lp.set(fs, fc);
			for (uint64_t i = 0; i < L; i++) tmp[i] = lp.run(tmp[i]);
			lp.reset();
			for (uint64_t i = L; i-- > 0;) tmp[i] = lp.run(tmp[i]);
			for (uint64_t i = 0; i < Ld; i++) tmp[i] = tmp[i * div] * float(div);
		}
		for (unsigned int p = 0; p < P; p++) {
			float* hr = hre + uint64_t(p) * N;
			float* hi = him + uint64_t(p) * N;
			uint64_t base = uint64_t(p) * B;
			for (unsigned int k = 0; k < B && base + k < Ld; k++) hr[k] = tmp[base + k];
			s->fft(hr, hi, false);
		}
	}
	if (div > 1) {
		for (unsigned int c = 0; c < channels; c++) {
			s->aa[c].set(fs, fc);
			s->ai[c].set(fs, fc);
		}
	}
	// The expensive part is done. The audio thread only waits for the swap, and the old state is freed after the lock is let go.
	std::unique_ptr<state> old;
	{
		std::lock_guard<std::mutex> l(m);
		old = std::move(st);
		st = std::move(s);
	}
	return true;
}
void convolver::clear_ir() {
	std::unique_ptr<state> old;
	std::lock_guard<std::mutex> l(m);
	old = std::move(st);
}
bool convolver::has_ir() {
	std::lock_guard<std::mutex> l(m);
	return st != nullptr;
}

void convolver::process(const float* conv_in, const float* dry_in, float* out, unsigned int frames, float dry, float wet) {
	std::lock_guard<std::mutex> l(m);
	uint64_t total = uint64_t(frames) * channels;
	if (!st) {
		for (uint64_t i = 0; i < total; i++) out[i] = dry_in[i] * dry;
		return;
	}
	state& s = *st;
	bool silent = true;
	for (uint64_t i = 0; i < total; i++) {
		if (conv_in[i] > 1e-7f || conv_in[i] < -1e-7f) {
			silent = false;
			break;
		}
	}
	if (silent) {
		// Once the input has been quiet for longer than the tail there is nothing left to compute.
		if (!s.asleep) {
			s.idle_frames += frames;
			if (s.idle_frames > s.tail_frames) s.go_idle();
		}
		if (s.asleep) {
			for (uint64_t i = 0; i < total; i++) out[i] = dry_in[i] * dry;
			return;
		}
	} else {
		s.idle_frames = 0;
		s.asleep = false;
	}
#ifdef NVSOUND_CONVOLVER_SSE
	// A decaying tail would otherwise sink into denormal range, where every operation on it is many times slower.
	unsigned int csr = _mm_getcsr();
	_mm_setcsr(csr | 0x8040); // Flush to zero, and treat denormal inputs as zero.
#endif
	for (unsigned int f = 0; f < frames; f++) {
		unsigned int at = s.bpos * s.div + s.phase;
		for (unsigned int c = 0; c < channels; c++) {
			float x = conv_in[f * channels + c], w = s.wet[uint64_t(c) * s.span + at];
			out[f * channels + c] = dry_in[f * channels + c] * dry + w * wet;
			if (s.div > 1) x = s.aa[c].run(x);
			if (s.phase == 0) s.in_blk[uint64_t(c) * s.B + s.bpos] = x;
		}
		if (++s.phase == s.div) {
			s.phase = 0;
			if (++s.bpos == s.B) {
				s.bpos = 0;
				s.render();
			}
		}
	}
#ifdef NVSOUND_CONVOLVER_SSE
	_mm_setcsr(csr);
#endif
}

} // namespace nvsound
