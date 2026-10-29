/* nvsound_phonon.h - Steam Audio spatialization for NVGT's sound engine.
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

namespace nvsound {

// Where a sound or mixer is relative to its listener, as set_position gave it.
struct spatial_params {
	float listener_x, listener_y, listener_z, x, y, z, rotation, pan_step, volume_step;
};

bool phonon_init(); // Creates the Steam Audio context and HRTF on first use. False if Steam Audio can't be loaded.
bool phonon_available();
struct phonon_state; // One sound's or mixer's Steam Audio effects.
class sound_environment;
bool phonon_attach_environment(phonon_state* s, sound_environment* env, float x, float y, float z);
void phonon_set_position(phonon_state* s, float x, float y, float z); // Passes a new position on to the environment, if the state is in one.
phonon_state* create_phonon_state();
void release_phonon_state(phonon_state* s);
void reset_phonon_state(phonon_state* s); // Clears the effects' history on the next block, as after a pause or seek.
void phonon_spatialize(phonon_state* s, float* frames, int count, const spatial_params& p, float& vol_db);

} // namespace nvsound
