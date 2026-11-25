/* realtime.h - what lets the audio thread hand work back and forth without ever waiting or allocating. See DESIGN.md.
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

#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace nvsound {

// Whether the calling thread is rendering audio right now. Set by audio_scope.
bool on_audio_thread();
// How many times a lock was taken, or memory allocated, while on_audio_thread(). Tests check these stay 0. Allocations are only counted where the test build hooks operator new.
size_t audio_thread_lock_count();
size_t audio_thread_alloc_count();
void note_audio_thread_alloc();
void reset_audio_thread_counts();

// Marks the current thread as the audio thread for its lifetime, as the device callback and offline rendering do.
class audio_scope {
	bool was;
public:
	audio_scope();
	~audio_scope();
};

// The core's only mutex type. Taking one on the audio thread is a bug, and it's counted.
class mutex {
	std::mutex m;
public:
	void lock();
	bool try_lock(); // The one lock the audio thread may take, because it never waits.
	void unlock() { m.unlock(); }
};

// Many producers, one consumer that never waits. Producers append under the mutex. The consumer try_locks it once per block and swaps the pending items for its own empty vector, whose capacity stays, so neither side frees memory there. T must be trivially destructible, so clearing a vector frees nothing.
template <class T>
class command_queue {
	static_assert(std::is_trivially_destructible_v<T>, "commands are cleared on the audio thread, so they can't own anything");
	mutex m;
	std::vector<T> pending;
	std::vector<T> taking;
public:
	explicit command_queue(size_t reserve = 4096) {
		pending.reserve(reserve);
		taking.reserve(reserve);
	}
	void push(const T& item) {
		std::lock_guard<mutex> l(m);
		pending.push_back(item);
	}
	// Consumer only. Calls f on every command pushed since the last take, in order. If a producer holds the lock right now, nothing is taken this time, and it all comes next time.
	template <class F>
	bool take(F&& f) {
		if (!m.try_lock()) return false;
		std::swap(pending, taking);
		m.unlock();
		for (const T& item : taking) f(item);
		taking.clear();
		return true;
	}
	// Producers only. Makes sure the consumer's vector can take what's pending without growing, which would allocate on the audio thread.
	void reserve_for_consumer() {
		std::lock_guard<mutex> l(m);
		if (taking.capacity() < pending.capacity()) taking.reserve(pending.capacity());
	}
	size_t pending_count() {
		std::lock_guard<mutex> l(m);
		return pending.size();
	}
};

// One producer, one consumer, fixed size, no locks. The audio thread uses it to hand things back.
template <class T, size_t N>
class spsc_ring {
	static_assert((N & (N - 1)) == 0, "N must be a power of two");
	std::array<T, N> items{};
	alignas(64) std::atomic<size_t> head{0}; // Next slot to write.
	alignas(64) std::atomic<size_t> tail{0}; // Next slot to read.
public:
	bool push(const T& item) {
		size_t h = head.load(std::memory_order_relaxed);
		if (h - tail.load(std::memory_order_acquire) == N) return false;
		items[h & (N - 1)] = item;
		head.store(h + 1, std::memory_order_release);
		return true;
	}
	bool pop(T& out) {
		size_t t = tail.load(std::memory_order_relaxed);
		if (t == head.load(std::memory_order_acquire)) return false;
		out = items[t & (N - 1)];
		tail.store(t + 1, std::memory_order_release);
		return true;
	}
	size_t size() const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
};

// Something the audio thread is finished with, deleted later on a script thread. A plain function pointer, so retiring needs no allocation.
struct retired {
	void* object = nullptr;
	void (*destroy)(void*) = nullptr;
};
template <class T>
retired retire_as(T* object) {
	return retired{object, [](void* p) { delete static_cast<T*>(p); }};
}

// The audio thread retires into this, and script threads collect. If the ring is ever full, retirements wait in a preallocated overflow list until there's room, and only if that is full too would anything be dropped, which is counted rather than freed on the audio thread.
class garbage {
	static constexpr size_t ring_size = 16384;
	spsc_ring<retired, ring_size> ring;
	std::array<retired, 4096> overflow{};
	size_t overflow_count = 0; // Audio thread only.
	std::atomic<size_t> lost{0};
	mutex collect_lock; // Only script threads take this, so two of them never pop the ring at once.
public:
	void retire(retired r); // Audio thread.
	void flush_overflow(); // Audio thread, once per block.
	size_t collect(); // Script threads. Deletes everything retired so far and returns how many.
	size_t lost_count() const { return lost; }
};

} // namespace nvsound
