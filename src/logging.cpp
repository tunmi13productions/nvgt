/* logging.cpp - error and diagnostic logging
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

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <Poco/AutoPtr.h>
#include <Poco/Channel.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/Exception.h>
#include <Poco/File.h>
#include <Poco/FileChannel.h>
#include <Poco/DateTimeFormatter.h>
#include <Poco/FormattingChannel.h>
#include <Poco/LocalDateTime.h>
#include <Poco/NumberFormatter.h>
#include <Poco/Logger.h>
#include <Poco/Message.h>
#include <Poco/Mutex.h>
#include <Poco/Path.h>
#include <Poco/PatternFormatter.h>
#include <Poco/SplitterChannel.h>
#include <Poco/String.h>
#include <Poco/StringTokenizer.h>
#include <Poco/UnicodeConverter.h>
#include <Poco/Util/Application.h>
#include <angelscript.h>
#include "logging.h"
#include "nvgt.h" // g_scriptpath
#include "nvgt_plugin.h" // subsystem access masks
#include "version.h"
#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
#else
	#include <fcntl.h>
	#include <unistd.h>
#endif

#ifdef _WIN32
	#define NEWLINE "\r\n"
#else
	#define NEWLINE "\n"
#endif

using namespace Poco;

std::string get_preferences_path(const std::string& org, const std::string& app); // filesystem.cpp

namespace nvgt_log {

std::atomic<int> g_ceiling(NVGT_LOG_OFF);

namespace {
	FastMutex g_mutex;
	// Guards nothing but the raw handle. A crash handler needs to reach that handle without waiting on whatever the rest of the logger happens to be doing.
	FastMutex g_raw_mutex;
	bool g_running = false;
	int g_level = NVGT_LOG_WARN;
	std::string g_path;
	std::string g_requested_file;
	AutoPtr<FileChannel> g_file_channel;
	AutoPtr<Channel> g_channel;
	std::map<std::string, int> g_channel_levels;
	#ifdef _WIN32
	HANDLE g_raw = INVALID_HANDLE_VALUE;
	#else
	int g_raw = -1;
	#endif

	// The ceiling is what the fast path at each call site tests, so it has to sit at or above every per channel override.
	void recalculate_ceiling() {
		int ceiling = g_running ? g_level : NVGT_LOG_OFF;
		if (g_running) {
			for (const auto& entry : g_channel_levels) {
				if (entry.second > ceiling) ceiling = entry.second;
			}
		}
		g_ceiling.store(ceiling, std::memory_order_relaxed);
	}

	Util::LayeredConfiguration* configuration() {
		try {
			return &Util::Application::instance().config();
		} catch (...) {
			return nullptr; // No application object exists yet, so every property falls back to its default.
		}
	}

	std::string config_string(const std::string& key, const std::string& def) {
		Util::LayeredConfiguration* config = configuration();
		if (!config) return def;
		try {
			return config->getString(key, def);
		} catch (...) {
			return def;
		}
	}

	bool config_flag(const std::string& key, bool def) {
		Util::LayeredConfiguration* config = configuration();
		if (!config) return def;
		try {
			if (!config->hasOption(key)) return def;
			std::string value = toLower(trim(config->getString(key, "")));
			// A property with no value at all, which is what a bare #pragma config or command line switch produces, reads as an enable.
			return value.empty() || value == "1" || value == "true" || value == "yes" || value == "on";
		} catch (...) {
			return def;
		}
	}

	// A relative log name belongs beside the script or game it describes, not beside whichever copy of nvgt happened to run it.
	std::string application_directory() {
		if (!g_scriptpath.empty()) return g_scriptpath;
		std::string dir = config_string("application.dir", "");
		if (!dir.empty()) return dir;
		return Path::current();
	}

	std::string resolve_path(const std::string& filename) {
		Path file(filename);
		if (file.isAbsolute()) return file.toString();
		std::string dir = config_string("logging.directory", "");
		if (dir.empty()) dir = application_directory();
		return Path(dir).resolve(file).toString();
	}

	// Used when the directory holding the program cannot be written to, which is normal for an installed game or a macOS bundle.
	std::string fallback_path(const std::string& filename) {
		std::string base = config_string("application.baseName", "nvgt");
		std::string dir = get_preferences_path("nvgt", base);
		if (dir.empty()) return "";
		return Path(dir).resolve(Path(filename).getFileName()).toString();
	}

	void open_raw(const std::string& target) {
		FastMutex::ScopedLock lock(g_raw_mutex);
		#ifdef _WIN32
		std::wstring wide;
		try {
			UnicodeConverter::toUTF16(target, wide);
		} catch (...) {
			return;
		}
		g_raw = CreateFileW(wide.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		#else
		g_raw = open(target.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
		#endif
	}

	void close_raw() {
		FastMutex::ScopedLock lock(g_raw_mutex);
		#ifdef _WIN32
		if (g_raw != INVALID_HANDLE_VALUE) CloseHandle(g_raw);
		g_raw = INVALID_HANDLE_VALUE;
		#else
		if (g_raw >= 0) close(g_raw);
		g_raw = -1;
		#endif
	}

	// A timestamp on every line is a wall of digits to listen through. The date and the time are announced once and then only when they change, so a burst of related messages reads as a group.
	class grouped_formatter : public Formatter {
		FastMutex mutex;
		std::string last_date;
		std::string last_time;
		bool with_source;
	public:
		grouped_formatter(bool source) : with_source(source) {}
		void format(const Message& msg, std::string& text) override {
			LocalDateTime when(msg.getTime());
			std::string date = DateTimeFormatter::format(when, "%Y-%m-%d");
			std::string time = DateTimeFormatter::format(when, "%H:%M:%S");
			text.clear();
			{
				FastMutex::ScopedLock lock(mutex);
				if (date != last_date) {
					if (!last_date.empty()) text += NEWLINE;
					text += date + NEWLINE + time + NEWLINE;
					last_date = date;
					last_time = time;
				} else if (time != last_time) {
					text += NEWLINE + time + NEWLINE;
					last_time = time;
				}
			}
			text += level_name(msg.getPriority());
			text += " ";
			text += msg.getSource();
			if (with_source && msg.getSourceFile()) {
				text += " (";
				text += msg.getSourceFile();
				text += ":";
				text += NumberFormatter::format(msg.getSourceLine());
				text += ")";
			}
			text += ": ";
			text += msg.getText();
		}
	};

	// A game can produce the same failure every frame. Left alone that writes megabytes of one sentence and pushes everything worth reading out of the file, so consecutive duplicates are counted instead of written.
	class repeat_filter_channel : public Channel {
		AutoPtr<Channel> inner;
		FastMutex mutex;
		std::string last_text;
		std::string last_source;
		int last_priority;
		unsigned long long repeats;
		unsigned long long reported;
		unsigned long long next_notice;
		void say(const std::string& text) { // Call with the lock held.
			inner->log(Message(last_source, text, Message::Priority(last_priority)));
		}
		// Interim notices go at ten, a hundred, a thousand and so on, so a run of fifty thousand costs five lines rather than fifty.
		void note_progress() {
			say("the line above has repeated " + NumberFormatter::format(repeats) + " times so far");
			reported = repeats;
			next_notice *= 10;
		}
		void finish_run() {
			if (repeats > reported) say("the line above repeated " + NumberFormatter::format(repeats) + (repeats == 1 ? " time in total" : " times in total"));
			repeats = reported = 0;
			next_notice = 10;
		}
	public:
		repeat_filter_channel(AutoPtr<Channel> target) : inner(target), last_priority(0), repeats(0), reported(0), next_notice(10) {}
		void log(const Message& msg) override {
			FastMutex::ScopedLock lock(mutex);
			if (msg.getPriority() == last_priority && msg.getSource() == last_source && msg.getText() == last_text) {
				if (++repeats >= next_notice) note_progress();
				return;
			}
			finish_run();
			last_priority = msg.getPriority();
			last_source = msg.getSource();
			last_text = msg.getText();
			inner->log(msg);
		}
		void open() override { inner->open(); }
		void close() override {
			FastMutex::ScopedLock lock(mutex);
			finish_run();
			inner->close();
		}
		void flush() {
			FastMutex::ScopedLock lock(mutex);
			finish_run();
		}
	};

	AutoPtr<repeat_filter_channel> g_repeat_filter;

	// Accepts a plain byte count or one with a K, M or G suffix, the same shapes Poco's own size properties take.
	Poco::UInt64 parse_size(const std::string& value, Poco::UInt64 def) {
		std::string text = trim(value);
		if (text.empty()) return def;
		char* end = nullptr;
		double number = strtod(text.c_str(), &end);
		if (!end || end == text.c_str() || number < 0) return def;
		while (*end == ' ') end++;
		switch (toupper(*end)) {
			case 'K': number *= 1024; break;
			case 'M': number *= 1024 * 1024; break;
			case 'G': number *= 1024 * 1024 * 1024; break;
			default: break;
		}
		return Poco::UInt64(number);
	}

	// One log, not a drift of dated siblings. Rather than archiving a file that grew too big, the old one is started over, which is the only way to keep the name stable and the size bounded at once.
	void enforce_size_cap(const std::string& target) {
		UInt64 cap = parse_size(config_string("logging.max_size", "5 M"), 5 * 1024 * 1024);
		if (!cap) return;
		try {
			File existing(target);
			if (!existing.exists() || UInt64(existing.getSize()) <= cap) return;
			existing.remove();
		} catch (Exception&) {} // An unwritable log is handled by the caller, not here.
	}

	AutoPtr<Channel> build_channel(const std::string& target) {
		enforce_size_cap(target);
		AutoPtr<FileChannel> file(new FileChannel(target));
		// Rotation is off by default so that only one file with one name ever exists. A scripter who wants dated archives can ask for them.
		std::string rotation = config_string("logging.rotation", "never");
		file->setProperty(FileChannel::PROP_ROTATION, rotation);
		if (rotation != "never") {
			file->setProperty(FileChannel::PROP_ARCHIVE, config_string("logging.archive", "timestamp"));
			file->setProperty(FileChannel::PROP_PURGECOUNT, config_string("logging.purge_count", "3"));
		}
		file->setProperty(FileChannel::PROP_FLUSH, "true");
		file->open(); // Fails here rather than on the first message if the path cannot be written.
		std::string pattern = config_string("logging.pattern", "");
		AutoPtr<Formatter> formatter;
		if (pattern.empty()) formatter = new grouped_formatter(config_flag("logging.source", false));
		else {
			AutoPtr<PatternFormatter> pf(new PatternFormatter(pattern));
			pf->setProperty(PatternFormatter::PROP_TIMES, "local");
			formatter = pf.cast<Formatter>();
		}
		AutoPtr<Channel> sink = file.cast<Channel>();
		if (config_flag("logging.console", false)) {
			AutoPtr<SplitterChannel> splitter(new SplitterChannel());
			splitter->addChannel(sink);
			splitter->addChannel(AutoPtr<Channel>(new ConsoleChannel(std::cerr)));
			sink = splitter.cast<Channel>();
		}
		g_file_channel = file;
		g_repeat_filter = new repeat_filter_channel(AutoPtr<Channel>(new FormattingChannel(formatter, sink)));
		return g_repeat_filter.cast<Channel>();
	}

	void stop_locked() {
		if (!g_running) return;
		Logger::setChannel("", AutoPtr<Channel>()); // Detaches every logger from our channel before it goes away.
		g_channel = nullptr;
		if (g_repeat_filter) g_repeat_filter->close();
		g_repeat_filter = nullptr;
		g_file_channel = nullptr;
		close_raw();
		g_running = false;
		recalculate_ceiling();
	}

	std::string banner() {
		std::string text = "NVGT " + NVGT_VERSION;
		if (!NVGT_VERSION_COMMIT_HASH.empty()) text += ", commit " + NVGT_VERSION_COMMIT_HASH.substr(0, 8);
		std::string command = config_string("application.path", "");
		if (!command.empty()) text += ", " + command;
		return text;
	}
}

bool running() {
	FastMutex::ScopedLock lock(g_mutex);
	return g_running;
}

std::string path() {
	FastMutex::ScopedLock lock(g_mutex);
	return g_path;
}

std::string planned_path() {
	FastMutex::ScopedLock lock(g_mutex);
	if (!g_path.empty()) return g_path;
	std::string requested = g_requested_file.empty() ? config_string("logging.file", "errors.log") : g_requested_file;
	try {
		return resolve_path(requested);
	} catch (...) {
		return requested;
	}
}

int level() { return g_level; }

const char* level_name(int value) {
	switch (value) {
		case NVGT_LOG_OFF: return "off";
		case 1: return "critical"; // Poco reserves this for fatal, which nothing in NVGT distinguishes from critical.
		case NVGT_LOG_CRITICAL: return "critical";
		case 5: return "info"; // Poco's notice, likewise.
		case NVGT_LOG_ERROR: return "error";
		case NVGT_LOG_WARN: return "warning";
		case NVGT_LOG_INFO: return "info";
		case NVGT_LOG_DEBUG: return "debug";
		case NVGT_LOG_TRACE: return "trace";
		default: return "unknown";
	}
}

int parse_level(const std::string& name, int fallback) {
	std::string value = toLower(trim(name));
	if (value.empty()) return fallback;
	if (value == "off" || value == "none" || value == "silent") return NVGT_LOG_OFF;
	if (value == "critical" || value == "fatal") return NVGT_LOG_CRITICAL;
	if (value == "error") return NVGT_LOG_ERROR;
	if (value == "warn" || value == "warning") return NVGT_LOG_WARN;
	if (value == "info" || value == "information") return NVGT_LOG_INFO;
	if (value == "debug") return NVGT_LOG_DEBUG;
	if (value == "trace" || value == "all") return NVGT_LOG_TRACE;
	char* end = nullptr;
	long numeric = strtol(value.c_str(), &end, 10);
	if (end && *end == 0 && numeric >= 0 && numeric <= NVGT_LOG_TRACE) return int(numeric);
	return fallback;
}

void set_level(int value) {
	FastMutex::ScopedLock lock(g_mutex);
	g_level = value;
	if (g_running) {
		Logger::setLevel("", value);
		for (const auto& entry : g_channel_levels) Logger::setLevel(entry.first, entry.second);
	}
	recalculate_ceiling();
}

int channel_level(const std::string& channel) {
	FastMutex::ScopedLock lock(g_mutex);
	std::map<std::string, int>::const_iterator it = g_channel_levels.find(channel);
	return it != g_channel_levels.end() ? it->second : g_level;
}

void set_channel_level(const std::string& channel, int value) {
	FastMutex::ScopedLock lock(g_mutex);
	if (channel.empty()) {
		g_level = value;
		if (g_running) Logger::setLevel("", value);
	} else {
		g_channel_levels[channel] = value;
		if (g_running) Logger::setLevel(channel, value);
	}
	recalculate_ceiling();
}

bool start(const std::string& filename, int value) {
	FastMutex::ScopedLock lock(g_mutex);
	stop_locked();
	std::string requested = filename.empty() ? config_string("logging.file", "errors.log") : filename;
	g_requested_file = requested;
	std::string target = resolve_path(requested);
	try {
		g_channel = build_channel(target);
	} catch (Exception&) {
		std::string alternate = fallback_path(requested);
		if (alternate.empty()) return false;
		try {
			g_channel = build_channel(alternate);
		} catch (Exception&) {
			return false;
		}
		target = alternate;
	}
	g_path = target;
	g_running = true;
	g_level = value;
	Logger::setChannel("", g_channel);
	Logger::setLevel("", g_level);
	for (const auto& entry : g_channel_levels) Logger::setLevel(entry.first, entry.second);
	recalculate_ceiling();
	open_raw(target);
	if (enabled(NVGT_LOG_INFO)) Logger::get("nvgt").information(banner());
	return true;
}

void stop() {
	FastMutex::ScopedLock lock(g_mutex);
	stop_locked();
}

void flush() {
	FastMutex::ScopedLock lock(g_mutex);
	// A pending run of duplicates is part of the log, so it has to land before anything reads the file.
	if (g_repeat_filter) g_repeat_filter->flush();
}

void write(const char* channel, int priority, const std::string& text, const char* file, int line) {
	if (!enabled(priority)) return;
	try {
		Logger& logger = Logger::get(channel && *channel ? channel : "nvgt");
		if (!logger.is(priority)) return;
		logger.log(Message(logger.name(), text, Message::Priority(priority), file, line));
	} catch (...) {} // A failing log must never take the program down with it.
}

// Deliberately does not take the main mutex. This runs from a crash handler, where waiting on whatever a dying thread was holding is worse than the small race on the file name.
bool open_emergency() {
	#ifdef _WIN32
	if (g_raw != INVALID_HANDLE_VALUE) return true;
	#else
	if (g_raw >= 0) return true;
	#endif
	std::string requested = g_requested_file.empty() ? config_string("logging.file", "errors.log") : g_requested_file;
	std::string target;
	try {
		target = resolve_path(requested);
		open_raw(target);
	} catch (...) {}
	#ifdef _WIN32
	if (g_raw == INVALID_HANDLE_VALUE) {
	#else
	if (g_raw < 0) {
	#endif
		try {
			target = fallback_path(requested);
			if (!target.empty()) open_raw(target);
		} catch (...) {}
	}
	#ifdef _WIN32
	if (g_raw == INVALID_HANDLE_VALUE) return false;
	#else
	if (g_raw < 0) return false;
	#endif
	if (g_path.empty()) g_path = target;
	return true;
}

void raw(const char* text, std::size_t length) {
	if (!text || !length) return;
	#ifdef _WIN32
	if (g_raw == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	WriteFile(g_raw, text, DWORD(length), &written, nullptr);
	#else
	if (g_raw < 0) return;
	ssize_t ignored = ::write(g_raw, text, length);
	(void)ignored;
	#endif
}

void raw(const char* text) {
	if (text) raw(text, strlen(text));
}

int raw_descriptor() {
	#ifdef _WIN32
	return -1;
	#else
	return g_raw;
	#endif
}

bool option(const std::string& key, bool def) { return config_flag(key, def); }

void configure() {
	// logging.channels takes a comma separated list of name=level pairs, using the same dotted names Poco gives its loggers.
	std::string channels = config_string("logging.channels", "");
	if (!channels.empty()) {
		StringTokenizer parts(channels, ",", StringTokenizer::TOK_TRIM | StringTokenizer::TOK_IGNORE_EMPTY);
		for (const std::string& part : parts) {
			std::string::size_type sep = part.find('=');
			if (sep == std::string::npos) continue;
			g_channel_levels[trim(part.substr(0, sep))] = parse_level(part.substr(sep + 1), NVGT_LOG_WARN);
		}
	}
	int requested = parse_level(config_string("logging.level", "warning"), NVGT_LOG_WARN);
	if (!config_flag("logging.enabled", false)) return;
	if (running()) {
		set_level(requested);
		return;
	}
	start(config_string("logging.file", "errors.log"), requested);
}

void shutdown() {
	stop();
	try {
		Logger::shutdown();
	} catch (...) {}
}

} // namespace nvgt_log

std::string get_call_stack_ctx(asIScriptContext* ctx); // scriptstuff.cpp

namespace {
	// Scripts get the section and line they logged from for free, which is what makes a log line traceable back to game code.
	void script_write(const char* channel, int priority, const std::string& message) {
		if (!nvgt_log::enabled(priority)) return;
		const char* file = nullptr;
		int line = 0;
		if (asIScriptContext* ctx = asGetActiveContext()) {
			const char* section = nullptr;
			line = ctx->GetLineNumber(0, nullptr, &section);
			file = section;
		}
		nvgt_log::write(channel, priority, message, file, line);
	}

	std::string with_call_stack(const std::string& message) {
		asIScriptContext* ctx = asGetActiveContext();
		if (!ctx) return message;
		return message + "\r\n" + get_call_stack_ctx(ctx);
	}
}

class script_logger {
	int ref_count;
	std::string channel;
public:
	script_logger(const std::string& name) : ref_count(1), channel(name.empty() ? "app" : name) {}
	void add_ref() { asAtomicInc(ref_count); }
	void release() { if (asAtomicDec(ref_count) < 1) delete this; }
	const std::string& get_name() const { return channel; }
	int get_level() { return nvgt_log::channel_level(channel); }
	void set_level(int level) { nvgt_log::set_channel_level(channel, level); }
	bool is_enabled_for(int level) { return nvgt_log::enabled(level) && level <= nvgt_log::channel_level(channel); }
	void log(int level, const std::string& message) { script_write(channel.c_str(), level, message); }
	void critical(const std::string& message) { script_write(channel.c_str(), NVGT_LOG_CRITICAL, message); }
	void error(const std::string& message) { script_write(channel.c_str(), NVGT_LOG_ERROR, message); }
	void warn(const std::string& message) { script_write(channel.c_str(), NVGT_LOG_WARN, message); }
	void info(const std::string& message) { script_write(channel.c_str(), NVGT_LOG_INFO, message); }
	void debug(const std::string& message) { script_write(channel.c_str(), NVGT_LOG_DEBUG, message); }
	void trace(const std::string& message) { script_write(channel.c_str(), NVGT_LOG_TRACE, message); }
	void exception(const std::string& message) {
		if (!nvgt_log::enabled(NVGT_LOG_ERROR)) return;
		script_write(channel.c_str(), NVGT_LOG_ERROR, with_call_stack(message));
	}
};

static script_logger* script_logger_factory(const std::string& name) { return new script_logger(name); }
static bool script_log_start(const std::string& filename, int level) { return nvgt_log::start(filename, level); }
static void script_log_critical(const std::string& message) { script_write("app", NVGT_LOG_CRITICAL, message); }
static void script_log_error(const std::string& message) { script_write("app", NVGT_LOG_ERROR, message); }
static void script_log_warn(const std::string& message) { script_write("app", NVGT_LOG_WARN, message); }
static void script_log_info(const std::string& message) { script_write("app", NVGT_LOG_INFO, message); }
static void script_log_debug(const std::string& message) { script_write("app", NVGT_LOG_DEBUG, message); }
static void script_log_trace(const std::string& message) { script_write("app", NVGT_LOG_TRACE, message); }
static void script_log_exception(const std::string& message) {
	if (!nvgt_log::enabled(NVGT_LOG_ERROR)) return;
	script_write("app", NVGT_LOG_ERROR, with_call_stack(message));
}
static std::string script_log_path() { return nvgt_log::path(); }
static bool script_log_running() { return nvgt_log::running(); }
static int script_log_level() { return nvgt_log::level(); }
static void script_set_log_level(int level) { nvgt_log::set_level(level); }
static void script_set_channel_level(const std::string& channel, int level) { nvgt_log::set_channel_level(channel, level); }

void RegisterLogging(asIScriptEngine* engine) {
	engine->RegisterEnum("log_level");
	engine->RegisterEnumValue("log_level", "LOG_OFF", NVGT_LOG_OFF);
	engine->RegisterEnumValue("log_level", "LOG_CRITICAL", NVGT_LOG_CRITICAL);
	engine->RegisterEnumValue("log_level", "LOG_ERROR", NVGT_LOG_ERROR);
	engine->RegisterEnumValue("log_level", "LOG_WARN", NVGT_LOG_WARN);
	engine->RegisterEnumValue("log_level", "LOG_INFO", NVGT_LOG_INFO);
	engine->RegisterEnumValue("log_level", "LOG_DEBUG", NVGT_LOG_DEBUG);
	engine->RegisterEnumValue("log_level", "LOG_TRACE", NVGT_LOG_TRACE);
	engine->RegisterObjectType("logger", 0, asOBJ_REF);
	engine->RegisterObjectBehaviour("logger", asBEHAVE_FACTORY, "logger@ l(const string&in name = \"app\")", asFUNCTION(script_logger_factory), asCALL_CDECL);
	engine->RegisterObjectBehaviour("logger", asBEHAVE_ADDREF, "void f()", asMETHOD(script_logger, add_ref), asCALL_THISCALL);
	engine->RegisterObjectBehaviour("logger", asBEHAVE_RELEASE, "void f()", asMETHOD(script_logger, release), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "const string& get_name() const property", asMETHOD(script_logger, get_name), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "log_level get_level() property", asMETHOD(script_logger, get_level), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void set_level(log_level level) property", asMETHOD(script_logger, set_level), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "bool is_enabled_for(log_level level)", asMETHOD(script_logger, is_enabled_for), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void log(log_level level, const string&in message)", asMETHOD(script_logger, log), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void critical(const string&in message)", asMETHOD(script_logger, critical), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void error(const string&in message)", asMETHOD(script_logger, error), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void warn(const string&in message)", asMETHOD(script_logger, warn), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void warning(const string&in message)", asMETHOD(script_logger, warn), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void info(const string&in message)", asMETHOD(script_logger, info), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void debug(const string&in message)", asMETHOD(script_logger, debug), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void trace(const string&in message)", asMETHOD(script_logger, trace), asCALL_THISCALL);
	engine->RegisterObjectMethod("logger", "void exception(const string&in message)", asMETHOD(script_logger, exception), asCALL_THISCALL);
	engine->RegisterGlobalFunction("void log_critical(const string&in message)", asFUNCTION(script_log_critical), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_error(const string&in message)", asFUNCTION(script_log_error), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_warn(const string&in message)", asFUNCTION(script_log_warn), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_warning(const string&in message)", asFUNCTION(script_log_warn), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_info(const string&in message)", asFUNCTION(script_log_info), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_debug(const string&in message)", asFUNCTION(script_log_debug), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_trace(const string&in message)", asFUNCTION(script_log_trace), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_exception(const string&in message)", asFUNCTION(script_log_exception), asCALL_CDECL);
	engine->RegisterGlobalFunction("bool get_log_running() property", asFUNCTION(script_log_running), asCALL_CDECL);
	engine->RegisterGlobalFunction("string get_log_path() property", asFUNCTION(script_log_path), asCALL_CDECL);
	engine->RegisterGlobalFunction("log_level get_logging_level() property", asFUNCTION(script_log_level), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_logging_level(log_level level) property", asFUNCTION(script_set_log_level), asCALL_CDECL);
	engine->RegisterGlobalFunction("void set_log_channel_level(const string&in channel, log_level level)", asFUNCTION(script_set_channel_level), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_flush()", asFUNCTION(nvgt_log::flush), asCALL_CDECL);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_FS);
	engine->RegisterGlobalFunction("bool log_start(const string&in filename = \"errors.log\", log_level level = LOG_WARN)", asFUNCTION(script_log_start), asCALL_CDECL);
	engine->RegisterGlobalFunction("void log_stop()", asFUNCTION(nvgt_log::stop), asCALL_CDECL);
	engine->SetDefaultAccessMask(NVGT_SUBSYSTEM_GENERAL);
}
