/* nvsound_effects.h - the effects that sound.set_fx and mixer.set_fx apply.
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
#include <string>
#include <vector>

namespace nvsound {

// One stage of an effect chain, processing interleaved stereo in place at the engine's sample rate.
class effect {
public:
	std::string id; // The $name it was given, if any.
	virtual ~effect() {}
	// Effects that BASS treated as one type can replace each other under the same id, as highpass and lowpass could.
	virtual const char* type() const = 0;
	// Takes the colon separated parts of the effect string with its name first, so args[1] is the first parameter as in the legacy engine. Returns false if they can't be used.
	virtual bool configure(const std::vector<std::string>& args) = 0;
	virtual void process(float* frames, int count) = 0;
};

} // namespace nvsound
