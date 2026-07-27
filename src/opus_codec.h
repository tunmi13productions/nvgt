/* opus_codec.h - raw Opus packet encoder/decoder for AngelScript
 *
 * FORK ADDITION -- this file does not exist upstream (samtupy/nvgt).
 *
 * NVGT core already has audio_opus_encoder, but that is an OggOpusEnc from libopusenc: it writes
 * a complete Ogg Opus *stream*, which is what you want for recording to a file and not what you
 * want for real-time voice. Voice chat needs to turn one PCM frame into one self-contained packet
 * and back again, which is the raw libopus API (opus_encode / opus_decode). That used to come
 * from a separate opus plugin whose binary is not shipped for every platform, which made any
 * script depending on it impossible to build cross-platform. libopus is already statically linked
 * into nvgt -- it is in SConstruct's common_libs and sound.cpp already includes <opus/opus.h> --
 * so exposing the packet API from core costs no new dependency and removes the plugin entirely.
 *
 * The registered script types are deliberately named opus_encoder and opus_decoder with the same
 * method surface the plugin had, so scripts only need to drop their `#pragma plugin opus`.
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

class asIScriptEngine;

// Registers the opus_encoder and opus_decoder script classes. Called from RegisterSoundsystem so
// it lands after the opus_application_type and opus_signal_type enums core already registers --
// those supply OPUS_APPLICATION_VOIP and OPUS_SIGNAL_VOICE, so they are not redeclared here.
void RegisterOpusCodec(asIScriptEngine* engine);
