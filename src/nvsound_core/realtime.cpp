/* realtime.cpp - the audio thread's no-wait handoffs. See realtime.h.
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

#include "realtime.h"

namespace nvsound {

static thread_local bool t_on_audio_thread = false;
static std::atomic<size_t> g_audio_locks{0}, g_audio_allocs{0};

bool on_audio_thread() { return t_on_audio_thread; }
size_t audio_thread_lock_count() { return g_audio_locks; }
size_t audio_thread_alloc_count() { return g_audio_allocs; }
void note_audio_thread_alloc() { g_audio_allocs++; }
void reset_audio_thread_counts() {
	g_audio_locks = 0;
	g_audio_allocs = 0;
}

audio_scope::audio_scope() : was(t_on_audio_thread) { t_on_audio_thread = true; }
audio_scope::~audio_scope() { t_on_audio_thread = was; }

void mutex::lock() {
	if (t_on_audio_thread) g_audio_locks++;
	m.lock();
}
bool mutex::try_lock() { return m.try_lock(); }

void garbage::retire(retired r) {
	if (!r.object) return;
	flush_overflow();
	if (overflow_count == 0 && ring.push(r)) return;
	if (overflow_count < overflow.size()) overflow[overflow_count++] = r;
	else lost++;
}
void garbage::flush_overflow() {
	size_t moved = 0;
	while (moved < overflow_count && ring.push(overflow[moved])) moved++;
	if (!moved) return;
	for (size_t i = moved; i < overflow_count; i++) overflow[i - moved] = overflow[i];
	overflow_count -= moved;
}
size_t garbage::collect() {
	std::lock_guard<mutex> l(collect_lock);
	size_t n = 0;
	retired r;
	while (ring.pop(r)) {
		r.destroy(r.object);
		n++;
	}
	return n;
}

} // namespace nvsound
