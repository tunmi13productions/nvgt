// Tests for realtime.h: the command queue, the lock-free ring and the garbage ring. A consumer thread plays the audio thread, and the test fails if it ever takes a lock or allocates.
#include "../realtime.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

// Every allocation in this program goes through here, so one made on the audio thread is counted.
void* operator new(size_t n) {
	if (nvsound::on_audio_thread()) nvsound::note_audio_thread_alloc();
	if (void* p = std::malloc(n ? n : 1)) return p;
	throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			std::printf("FAILED line %d: %s\n", __LINE__, #cond); \
			failures++; \
		} \
	} while (0)

struct command {
	int producer;
	int sequence;
};

static void queue_under_load() {
	nvsound::reset_audio_thread_counts();
	nvsound::command_queue<command> q;
	constexpr int producers = 4, per_producer = 50000;
	std::atomic<bool> done{false};
	std::vector<int> next(producers, 0);
	int out_of_order = 0, received = 0;
	std::thread consumer([&] {
		nvsound::audio_scope scope;
		auto on = [&](const command& c) {
			if (c.sequence != next[c.producer]) out_of_order++;
			next[c.producer] = c.sequence + 1;
			received++;
		};
		while (!done) {
			q.take(on);
			std::this_thread::yield();
		}
		// Every producer has finished, so nothing holds the lock and this take gets the rest.
		q.take(on);
	});
	std::vector<std::thread> ps;
	for (int p = 0; p < producers; p++)
		ps.emplace_back([&q, p] {
			for (int i = 0; i < per_producer; i++) {
				q.push({p, i});
				if (i % 1000 == 0) q.reserve_for_consumer();
			}
		});
	for (auto& t : ps) t.join();
	done = true;
	consumer.join();
	CHECK(received == producers * per_producer);
	CHECK(out_of_order == 0);
	CHECK(nvsound::audio_thread_lock_count() == 0);
}

static void consumer_never_locks() {
	nvsound::reset_audio_thread_counts();
	nvsound::command_queue<command> q;
	for (int i = 0; i < 100; i++) q.push({0, i});
	q.reserve_for_consumer();
	int got = 0;
	{
		nvsound::audio_scope scope;
		q.take([&](const command&) { got++; });
		q.take([&](const command&) { got++; });
	}
	CHECK(got == 100);
	CHECK(nvsound::audio_thread_lock_count() == 0);
	CHECK(nvsound::audio_thread_alloc_count() == 0);
}

struct tracked {
	static std::atomic<int> alive;
	tracked() { alive++; }
	~tracked() { alive--; }
};
std::atomic<int> tracked::alive{0};

static void garbage_is_freed_off_the_audio_thread() {
	nvsound::reset_audio_thread_counts();
	nvsound::garbage g;
	constexpr int n = 40000; // More than the ring holds, so the overflow list is used.
	std::vector<tracked*> made;
	made.reserve(n);
	for (int i = 0; i < n; i++) made.push_back(new tracked());
	CHECK(tracked::alive == n);
	std::atomic<bool> done{false};
	std::thread audio([&] {
		nvsound::audio_scope scope;
		for (int i = 0; i < n; i++) {
			g.retire(nvsound::retire_as(made[i]));
			if (i % 256 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
		}
		while (!done) {
			g.flush_overflow();
			std::this_thread::yield();
		}
	});
	int collected = 0;
	while (collected < n) {
		collected += int(g.collect());
		std::this_thread::yield();
	}
	done = true;
	audio.join();
	CHECK(collected == n);
	CHECK(tracked::alive == 0);
	CHECK(g.lost_count() == 0);
	CHECK(nvsound::audio_thread_lock_count() == 0);
	CHECK(nvsound::audio_thread_alloc_count() == 0);
}

static void counting_works() {
	nvsound::reset_audio_thread_counts();
	nvsound::mutex m;
	{
		nvsound::audio_scope scope;
		m.lock();
		m.unlock();
		int* p = new int(5);
		delete p;
	}
	CHECK(nvsound::audio_thread_lock_count() == 1);
	CHECK(nvsound::audio_thread_alloc_count() == 1);
	nvsound::reset_audio_thread_counts();
}

int main() {
	counting_works();
	consumer_never_locks();
	queue_under_load();
	garbage_is_freed_off_the_audio_thread();
	if (failures) {
		std::printf("%d failed\n", failures);
		return 1;
	}
	std::printf("realtime: all passed\n");
	return 0;
}
