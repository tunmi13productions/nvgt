/* graph.h - the audio thread's side of the new nvsound core: nodes, what they publish, and the commands that change them. See DESIGN.md.
 * Only the audio thread touches a node once it's been added. Everything here is internal to the core.
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
#include "../nvsound_common.h"
#include "../nvsound_phonon.h" // spatial_params
#include "realtime.h"
#include <atomic>
#include <memory>

class WDL_Resampler;

namespace nvsound {

// What a node tells the script side, written by the audio thread. The seq numbers say which of the script's requests have been applied, so a getter knows whether to answer with this or with what was asked.
struct node_status {
	std::atomic<float> volume{0}, pan{0}, pitch{100};
	std::atomic<uint32_t> volume_seq{0}, pan_seq{0}, pitch_seq{0};
	std::atomic<bool> volume_sliding{false}, pan_sliding{false}, pitch_sliding{false};
	std::atomic<uint64_t> volume_end{0}, pan_end{0}, pitch_end{0};
	std::atomic<bool> playing{false}, ended{false};
	std::atomic<uint32_t> transport_seq{0};
};

// One sound or mixer, as the audio thread renders it. The fields and the order of every stage in render are today's nvsound.cpp, moved.
struct node {
	bool is_mixer = false;
	std::shared_ptr<node_status> st;
	// The tree. Children are a list linked through the nodes themselves, so adding and removing one allocates nothing, and they render in the order they were added, as today's vector did.
	node* parent = nullptr;
	node* first_child = nullptr;
	node* last_child = nullptr;
	node* prev = nullptr;
	node* next = nullptr;
	slider volume{0}, pan{0}, pitch{100};
	float last_gain_left = -1, last_gain_right = -1; // -1 until the first block, so a sound doesn't ramp in from silence.
	spatial_params sp{0, 0, 0, 0, 0, 0, 0, 1, 1};
	bool positioned = false;
	WDL_Resampler* resampler = nullptr; // Built on the script side. Replaced only by command, and the old one retired.
	float dry_volume = 0;
	// Sounds only.
	std::shared_ptr<source>* src = nullptr; // A holder, so the source's last reference is only ever dropped where the holder is deleted, off the audio thread.
	bool playing = false, ended = false, looping = false;
	~node();
};

enum class op : uint8_t {
	add,
	destroy,
	set_parent,
	set_volume,
	slide_volume,
	set_pan,
	slide_pan,
	set_pitch,
	slide_pitch,
	set_resampler,
	set_position,
	set_dry_volume,
	attach_source,
	detach_source,
	play,
	pause,
	stop,
	set_looping,
};

// Small and trivially destructible, so the queue can clear it on the audio thread. ptr hands over something the script side built, which the audio thread then owns.
struct command {
	op kind;
	bool flag = false;
	uint32_t seq = 0;
	node* target = nullptr;
	node* other = nullptr;
	void* ptr = nullptr;
	float value = 0;
	uint64_t frames = 0;
	spatial_params sp{};
};

// The audio thread's handoffs, defined in core.cpp.
void send(const command& c);
void retire(retired r);

} // namespace nvsound
