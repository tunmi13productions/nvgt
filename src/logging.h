/* logging.h - error and diagnostic logging
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
#include <atomic>
#include <cstddef>
#include <string>

class asIScriptEngine;

// These match Poco::Message::Priority so that a level can be handed straight to Poco, with 0 meaning that nothing is logged at all.
enum nvgt_log_level {
	NVGT_LOG_OFF = 0,
	NVGT_LOG_CRITICAL = 2,
	NVGT_LOG_ERROR = 3,
	NVGT_LOG_WARN = 4,
	NVGT_LOG_INFO = 6,
	NVGT_LOG_DEBUG = 7,
	NVGT_LOG_TRACE = 8
};

namespace nvgt_log {
	// The highest priority any channel currently accepts, so that a disabled log costs one relaxed load at the call site.
	extern std::atomic<int> g_ceiling;
	inline bool enabled(int priority) { return priority <= g_ceiling.load(std::memory_order_relaxed); }
	bool start(const std::string& filename = "", int level = NVGT_LOG_WARN);
	void stop();
	void flush();
	// Applies the logging.* configuration properties. Safe to call again once more configuration has arrived, which is what a compiled game does after its embedded settings are read.
	void configure();
	// Reads a configuration property as a flag, treating a property present but empty as an enable the way a bare #pragma config leaves it.
	bool option(const std::string& key, bool def);
	void shutdown();
	bool running();
	std::string path();
	// Where a log would be written, whether or not one is currently open. Used to tell a player where to find a crash report.
	std::string planned_path();
	int level();
	void set_level(int level);
	int channel_level(const std::string& channel);
	void set_channel_level(const std::string& channel, int level);
	int parse_level(const std::string& name, int fallback);
	const char* level_name(int level);
	void write(const char* channel, int priority, const std::string& message, const char* file = nullptr, int line = 0);
	// Opens the log without any of Poco's machinery so that a crashing process still has somewhere to write, returns false if no path could be opened.
	bool open_emergency();
	// Appends text using nothing but a raw OS write, safe to call from a signal handler or an unhandled exception filter.
	void raw(const char* text, std::size_t length);
	void raw(const char* text);
	// The descriptor behind raw(), for the few POSIX routines such as backtrace_symbols_fd that insist on writing themselves. Returns -1 on Windows or when nothing is open.
	int raw_descriptor();
}

#define NVGT_LOG_AT(channel, priority, message) do { if (nvgt_log::enabled(priority)) nvgt_log::write(channel, priority, message, __FILE__, __LINE__); } while (0)
#define NVGT_CRITICAL(channel, message) NVGT_LOG_AT(channel, NVGT_LOG_CRITICAL, message)
#define NVGT_ERROR(channel, message) NVGT_LOG_AT(channel, NVGT_LOG_ERROR, message)
#define NVGT_WARN(channel, message) NVGT_LOG_AT(channel, NVGT_LOG_WARN, message)
#define NVGT_INFO(channel, message) NVGT_LOG_AT(channel, NVGT_LOG_INFO, message)
#define NVGT_DEBUG(channel, message) NVGT_LOG_AT(channel, NVGT_LOG_DEBUG, message)
#define NVGT_TRACE(channel, message) NVGT_LOG_AT(channel, NVGT_LOG_TRACE, message)

void RegisterLogging(asIScriptEngine* engine);
