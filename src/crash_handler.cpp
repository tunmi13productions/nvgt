/* crash_handler.cpp - writes a report to the log when the process is about to die
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

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <new>
#include <angelscript.h>
#include "crash_handler.h"
#include "logging.h"
#include "version.h"
#ifdef _WIN32
	#define WIN32_LEAN_AND_MEAN
	#include <windows.h>
	#include <dbghelp.h>
	#include <crtdbg.h>
#else
	#include <unistd.h>
	#if !defined(__ANDROID__)
		#include <execinfo.h>
	#endif
#endif

#ifdef _WIN32
	#define NL "\r\n"
#else
	#define NL "\n"
#endif

namespace {

const int MAX_NATIVE_FRAMES = 48;
const int MAX_SCRIPT_FRAMES = 64;

std::atomic<bool> g_installed(false);
// Set once a report is under way. A fault raised while writing one must not start the whole dance again.
std::atomic<bool> g_reporting(false);
char g_buffer[16384];
std::size_t g_used = 0;
char g_alert_path[1024];
bool g_alert = true;
// A fault caught at first chance is held here rather than written straight out, because something further up the stack may still swallow it and carry on.
char g_pending[16384];
std::size_t g_pending_used = 0;
std::atomic<bool> g_deferring(false);
std::atomic<bool> g_has_pending(false);
int g_reports_written = 0;
const int MAX_REPORTS = 4;
const char* g_pending_name = nullptr;
unsigned long long g_pending_address = 0;

void put(const char* text, std::size_t length) {
	if (!text) return;
	if (g_used + length >= sizeof(g_buffer)) length = sizeof(g_buffer) - g_used - 1;
	if (!length) return;
	memcpy(g_buffer + g_used, text, length);
	g_used += length;
}

void put(const char* text) {
	if (text) put(text, strlen(text));
}

void put_signed(long long value) {
	char digits[24];
	int at = 0;
	bool negative = value < 0;
	unsigned long long magnitude = negative ? (unsigned long long)(-(value + 1)) + 1 : (unsigned long long)value;
	do {
		digits[at++] = char('0' + magnitude % 10);
		magnitude /= 10;
	} while (magnitude && at < int(sizeof(digits)));
	if (negative) put("-", 1);
	while (at) put(&digits[--at], 1);
}

void put_padded(long long value, int width) {
	long long scale = 1;
	for (int i = 1; i < width; i++) scale *= 10;
	while (scale > 1 && value < scale) {
		put("0", 1);
		scale /= 10;
	}
	put_signed(value);
}

void put_hex(unsigned long long value, int width) {
	static const char alphabet[] = "0123456789abcdef";
	char digits[20];
	int at = 0;
	do {
		digits[at++] = alphabet[value & 0xf];
		value >>= 4;
	} while (value && at < int(sizeof(digits)));
	put("0x", 2);
	for (int pad = at; pad < width; pad++) put("0", 1);
	while (at) put(&digits[--at], 1);
}

void flush() {
	if (g_deferring.load()) {
		std::size_t room = sizeof(g_pending) - g_pending_used - 1;
		std::size_t take = g_used < room ? g_used : room;
		if (take) {
			memcpy(g_pending + g_pending_used, g_buffer, take);
			g_pending_used += take;
		}
		g_used = 0;
		return;
	}
	nvgt_log::raw(g_buffer, g_used);
	g_used = 0;
}

void put_timestamp() {
	#ifdef _WIN32
	SYSTEMTIME now;
	GetLocalTime(&now);
	put_padded(now.wYear, 4);
	put("-", 1);
	put_padded(now.wMonth, 2);
	put("-", 1);
	put_padded(now.wDay, 2);
	put(" ", 1);
	put_padded(now.wHour, 2);
	put(":", 1);
	put_padded(now.wMinute, 2);
	put(":", 1);
	put_padded(now.wSecond, 2);
	#else
	time_t seconds = time(nullptr);
	struct tm parts;
	if (localtime_r(&seconds, &parts)) {
		put_padded(parts.tm_year + 1900, 4);
		put("-", 1);
		put_padded(parts.tm_mon + 1, 2);
		put("-", 1);
		put_padded(parts.tm_mday, 2);
		put(" ", 1);
		put_padded(parts.tm_hour, 2);
		put(":", 1);
		put_padded(parts.tm_min, 2);
		put(":", 1);
		put_padded(parts.tm_sec, 2);
	} else put_signed((long long)seconds);
	#endif
}

// Only the file name is repeated per frame. A full path read out on every line is a great deal to listen through.
const char* base_name(const char* path) {
	if (!path || !*path) return "unknown file";
	const char* name = path;
	for (const char* at = path; *at; at++) {
		if (*at == '/' || *at == 92) name = at + 1;
	}
	return *name ? name : path;
}

// AngelScript is not built to be walked from a signal handler, but this stack is the only part of the report a game developer can act on, so it is worth attempting.
void put_script_stack() {
	asIScriptContext* ctx = asGetActiveContext();
	if (!ctx) {
		put("Script call stack: none. The thread that faulted was not running script code." NL);
		return;
	}
	asUINT size = ctx->GetCallstackSize();
	put("Script call stack, ");
	put_signed(size);
	put(size == 1 ? " frame, innermost first." NL : " frames, innermost first." NL);
	if (size > asUINT(MAX_SCRIPT_FRAMES)) size = asUINT(MAX_SCRIPT_FRAMES);
	for (asUINT i = 0; i < size; i++) {
		const char* section = nullptr;
		int column = 0;
		int line = ctx->GetLineNumber(i, &column, &section);
		asIScriptFunction* func = ctx->GetFunction(i);
		const char* declaration = func ? func->GetDeclaration() : nullptr;
		put("Frame ");
		put_signed(i);
		put(": ");
		put(declaration ? declaration : "unknown function");
		put(", file ");
		put(base_name(section));
		put(", line ");
		put_signed(line);
		put("." NL);
	}
	if (ctx->GetState() == asEXECUTION_EXCEPTION) {
		put("Script exception: ");
		const char* text = ctx->GetExceptionString();
		put(text ? text : "(none)");
		put(NL);
	}
}

#ifdef _WIN32

const char* exception_name(DWORD code) {
	switch (code) {
		case EXCEPTION_ACCESS_VIOLATION: return "access violation, the program read or wrote memory it does not own (EXCEPTION_ACCESS_VIOLATION)";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array index out of bounds (EXCEPTION_ARRAY_BOUNDS_EXCEEDED)";
		case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned memory access (EXCEPTION_DATATYPE_MISALIGNMENT)";
		case EXCEPTION_FLT_DENORMAL_OPERAND: return "floating point denormal operand (EXCEPTION_FLT_DENORMAL_OPERAND)";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating point division by zero (EXCEPTION_FLT_DIVIDE_BY_ZERO)";
		case EXCEPTION_FLT_INEXACT_RESULT: return "floating point inexact result (EXCEPTION_FLT_INEXACT_RESULT)";
		case EXCEPTION_FLT_INVALID_OPERATION: return "invalid floating point operation (EXCEPTION_FLT_INVALID_OPERATION)";
		case EXCEPTION_FLT_OVERFLOW: return "floating point overflow (EXCEPTION_FLT_OVERFLOW)";
		case EXCEPTION_FLT_STACK_CHECK: return "floating point stack check (EXCEPTION_FLT_STACK_CHECK)";
		case EXCEPTION_FLT_UNDERFLOW: return "floating point underflow (EXCEPTION_FLT_UNDERFLOW)";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction (EXCEPTION_ILLEGAL_INSTRUCTION)";
		case EXCEPTION_IN_PAGE_ERROR: return "a memory page could not be read, often a failing disk or a network drive going away (EXCEPTION_IN_PAGE_ERROR)";
		case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero (EXCEPTION_INT_DIVIDE_BY_ZERO)";
		case EXCEPTION_INT_OVERFLOW: return "integer overflow (EXCEPTION_INT_OVERFLOW)";
		case EXCEPTION_INVALID_DISPOSITION: return "invalid exception disposition (EXCEPTION_INVALID_DISPOSITION)";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "a fault the program cannot continue past (EXCEPTION_NONCONTINUABLE_EXCEPTION)";
		case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction (EXCEPTION_PRIV_INSTRUCTION)";
		case EXCEPTION_STACK_OVERFLOW: return "stack overflow, usually endless recursion (EXCEPTION_STACK_OVERFLOW)";
		case EXCEPTION_BREAKPOINT: return "breakpoint (EXCEPTION_BREAKPOINT)";
		case EXCEPTION_SINGLE_STEP: return "single step (EXCEPTION_SINGLE_STEP)";
		case 0xe06d7363: return "an unhandled C++ exception";
		default: return "an unrecognised fault";
	}
}

void put_module_for(DWORD64 address) {
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module) || !module) {
		put_hex(address, 16);
		return;
	}
	char path[MAX_PATH];
	if (!GetModuleFileNameA(module, path, MAX_PATH)) {
		put_hex(address, 16);
		return;
	}
	put(base_name(path));
	put(" plus ");
	put_hex(address - (DWORD64)module, 0);
}

void put_native_stack(CONTEXT* context) {
	put("Native stack. This part is for whoever maintains NVGT rather than for you." NL);
	CONTEXT walking = *context; // StackWalk64 modifies the context it is handed.
	STACKFRAME64 frame;
	memset(&frame, 0, sizeof(frame));
	DWORD machine;
	#if defined(_M_X64) || defined(__x86_64__)
	machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = walking.Rip;
	frame.AddrFrame.Offset = walking.Rbp;
	frame.AddrStack.Offset = walking.Rsp;
	#elif defined(_M_ARM64) || defined(__aarch64__)
	machine = IMAGE_FILE_MACHINE_ARM64;
	frame.AddrPC.Offset = walking.Pc;
	frame.AddrFrame.Offset = walking.Fp;
	frame.AddrStack.Offset = walking.Sp;
	#else
	machine = IMAGE_FILE_MACHINE_I386;
	frame.AddrPC.Offset = walking.Eip;
	frame.AddrFrame.Offset = walking.Ebp;
	frame.AddrStack.Offset = walking.Esp;
	#endif
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Mode = AddrModeFlat;
	HANDLE process = GetCurrentProcess();
	HANDLE thread = GetCurrentThread();
	static char symbol_storage[sizeof(SYMBOL_INFO) + 512];
	for (int depth = 0; depth < MAX_NATIVE_FRAMES; depth++) {
		if (!StackWalk64(machine, process, thread, &frame, &walking, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
		if (!frame.AddrPC.Offset) break;
		put("Frame ");
		put_signed(depth);
		put(": ");
		put_module_for(frame.AddrPC.Offset);
		SYMBOL_INFO* symbol = (SYMBOL_INFO*)symbol_storage;
		memset(symbol_storage, 0, sizeof(symbol_storage));
		symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
		symbol->MaxNameLen = 511;
		DWORD64 displacement = 0;
		if (SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol)) {
			put(", ");
			put(symbol->Name);
			if (displacement) {
				put(" plus ");
				put_hex(displacement, 0);
			}
		}
		IMAGEHLP_LINE64 line;
		memset(&line, 0, sizeof(line));
		line.SizeOfStruct = sizeof(line);
		DWORD line_displacement = 0;
		if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line) && line.FileName) {
			put(", file ");
			put(base_name(line.FileName));
			put(", line ");
			put_signed(line.LineNumber);
		}
		put("." NL);
	}
}

void show_alert() {
	if (!g_alert || !g_alert_path[0]) return;
	static wchar_t text[1400];
	static wchar_t wide_path[1024];
	int converted = MultiByteToWideChar(CP_UTF8, 0, g_alert_path, -1, wide_path, 1024);
	if (converted <= 0) return;
	const wchar_t* prefix = L"This program has crashed. A report describing what went wrong was written to the file named below. Please send it to whoever maintains this program.\n\n";
	const wchar_t* suffix = L"";
	text[0] = 0;
	wcsncat_s(text, prefix, _TRUNCATE);
	wcsncat_s(text, wide_path, _TRUNCATE);
	wcsncat_s(text, suffix, _TRUNCATE);
	MessageBoxW(nullptr, text, L"Crash", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
}

void write_report(const char* reason, DWORD code, void* address, void* extra, CONTEXT* context);

// Codes that mean the program has genuinely gone wrong, as opposed to the ones a runtime raises and handles as part of working normally.
bool is_fatal_code(DWORD code) {
	switch (code) {
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		case EXCEPTION_DATATYPE_MISALIGNMENT:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_IN_PAGE_ERROR:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_STACK_OVERFLOW:
		case EXCEPTION_NONCONTINUABLE_EXCEPTION:
			return true;
		default:
			return false;
	}
}

void report_record(EXCEPTION_RECORD* record, CONTEXT* context) {
	void* extra = nullptr;
	if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) extra = (void*)record->ExceptionInformation[1];
	write_report(exception_name(record->ExceptionCode), record->ExceptionCode, record->ExceptionAddress, extra, context);
}

// Runs before any frame based handler, which is the only way to see a fault that a catch(...) further up is about to swallow. The report is held rather than written, so a fault the program really does recover from leaves nothing behind.
LONG WINAPI first_chance_handler(EXCEPTION_POINTERS* info) {
	if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
	if (!is_fatal_code(info->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
	if (g_has_pending.load() || g_reports_written >= MAX_REPORTS) return EXCEPTION_CONTINUE_SEARCH;
	g_pending_used = 0;
	g_deferring.store(true);
	report_record(info->ExceptionRecord, info->ContextRecord);
	g_deferring.store(false);
	g_pending_name = exception_name(info->ExceptionRecord->ExceptionCode);
	g_pending_address = (unsigned long long)info->ExceptionRecord->ExceptionAddress;
	if (g_pending_used) g_has_pending.store(true);
	return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
	if (g_has_pending.load()) crash_handler_commit_pending(); // The first chance pass already described this fault and had the better context record.
	else if (info && info->ExceptionRecord) report_record(info->ExceptionRecord, info->ContextRecord);
	else write_report("unknown fault", 0, nullptr, nullptr, nullptr);
	show_alert();
	return EXCEPTION_CONTINUE_SEARCH; // Let Windows do whatever it would normally do, including handing the process to an attached debugger.
}

void purecall_handler() {
	write_report("pure virtual function call", 0, nullptr, nullptr, nullptr);
}

void invalid_parameter_handler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
	write_report("invalid parameter passed to a C runtime function", 0, nullptr, nullptr, nullptr);
}

#else

const char* signal_name(int number) {
	switch (number) {
		case SIGSEGV: return "invalid memory access, the program read or wrote memory it does not own (SIGSEGV)";
		case SIGBUS: return "bus error, often a misaligned access or a file that shrank while it was mapped (SIGBUS)";
		case SIGFPE: return "arithmetic error, usually division by zero (SIGFPE)";
		case SIGILL: return "illegal instruction (SIGILL)";
		case SIGABRT: return "the program aborted itself, usually a failed assertion or an unhandled C++ exception (SIGABRT)";
		case SIGSYS: return "bad system call (SIGSYS)";
		default: return "an unrecognised signal";
	}
}

// There is no dialog to show here, so the terminal is where a player or a developer is told the report exists.
void show_alert() {
	if (!g_alert || !g_alert_path[0]) return;
	const char* prefix = "This program has crashed. A report describing what went wrong was written to ";
	ssize_t ignored = ::write(2, prefix, strlen(prefix));
	ignored = ::write(2, g_alert_path, strlen(g_alert_path));
	ignored = ::write(2, ".\n", 2);
	(void)ignored;
}

void write_report(const char* reason, int number, void* address);

char g_alt_stack[65536]; // SIGSTKSZ stopped being a constant expression in newer glibc, and a stack overflow report needs the room anyway.

void signal_handler(int number, siginfo_t* info, void*) {
	write_report(signal_name(number), number, info ? info->si_addr : nullptr);
	// Restore the default disposition and re raise so the operating system still produces whatever it normally would, a core file or a macOS crash report.
	signal(number, SIG_DFL);
	raise(number);
}

#endif

void put_header(const char* reason) {
	put(NL "NVGT crash report." NL);
	put("Time: ");
	put_timestamp();
	put(NL "Version: NVGT ");
	put(NVGT_VERSION.c_str());
	if (!NVGT_VERSION_COMMIT_HASH.empty()) {
		put(", commit ");
		put(NVGT_VERSION_COMMIT_HASH.c_str(), NVGT_VERSION_COMMIT_HASH.size() < 8 ? NVGT_VERSION_COMMIT_HASH.size() : 8); // The short hash identifies a build and is far less to listen through.
	}
	put(NL "Reason: ");
	put(reason ? reason : "unknown");
}

#ifdef _WIN32

void write_report(const char* reason, DWORD code, void* address, void* extra, CONTEXT* context) {
	bool expected = false;
	if (!g_reporting.compare_exchange_strong(expected, true)) return;
	if (!g_deferring.load()) nvgt_log::open_emergency(); // A deferred report may never be written, and creating the file for one would be misleading.
	g_used = 0;
	put_header(reason);
	if (code) {
		put(", code ");
		put_hex(code, 8);
	}
	if (address) {
		put(NL "Fault address: ");
		put_hex((unsigned long long)address, 16);
	}
	if (extra) {
		put(NL "Accessed address: ");
		put_hex((unsigned long long)extra, 16);
	}
	put(NL "Thread: ");
	put_signed(GetCurrentThreadId());
	put(NL NL);
	put_script_stack();
	put(NL);
	flush(); // Everything gathered so far is on disk before the risky part starts.
	if (context) {
		put_native_stack(context);
		flush();
	}
	put("End of crash report." NL);
	flush();
	if (!g_deferring.load()) {
		nvgt_log::flush();
		g_reports_written++;
	}
	g_reporting.store(false);
}

#else

void write_report(const char* reason, int number, void* address) {
	bool expected = false;
	if (!g_reporting.compare_exchange_strong(expected, true)) return;
	nvgt_log::open_emergency();
	g_used = 0;
	put_header(reason);
	if (number) {
		put(", signal ");
		put_signed(number);
	}
	if (address) {
		put(NL "Fault address: ");
		put_hex((unsigned long long)address, 16);
	}
	put(NL NL);
	put_script_stack();
	put(NL);
	flush();
	#if !defined(__ANDROID__)
	put("Native stack. This part is for whoever maintains NVGT rather than for you." NL);
	flush();
	void* frames[MAX_NATIVE_FRAMES];
	int count = backtrace(frames, MAX_NATIVE_FRAMES);
	// backtrace_symbols allocates, its _fd counterpart does not, so the log descriptor is handed straight to it.
	int descriptor = nvgt_log::raw_descriptor();
	if (descriptor >= 0) backtrace_symbols_fd(frames, count, descriptor);
	#endif
	put("End of crash report." NL);
	flush();
	nvgt_log::flush();
	g_reports_written++;
	show_alert();
	g_reporting.store(false);
}

#endif

void on_terminate() {
	const char* what = "std::terminate called";
	static char detail[512];
	try {
		std::exception_ptr current = std::current_exception();
		if (current) std::rethrow_exception(current);
	} catch (const std::exception& e) {
		detail[0] = 0;
		strncat(detail, "unhandled C++ exception: ", sizeof(detail) - 1);
		strncat(detail, e.what(), sizeof(detail) - strlen(detail) - 1);
		what = detail;
	} catch (...) {
		what = "unhandled C++ exception of unknown type";
	}
	crash_handler_commit_pending();
	crash_handler_uninstall(); // Otherwise the abort below is trapped as a signal and a second report is written for the same fault.
	#ifdef _WIN32
	write_report(what, 0, nullptr, nullptr, nullptr);
	#else
	write_report(what, 0, nullptr);
	#endif
	abort();
}

} // namespace

namespace {
	void remember_log_path() {
		std::string log_path = nvgt_log::planned_path();
		if (log_path.empty()) return;
		#ifdef _WIN32
		strncpy_s(g_alert_path, log_path.c_str(), _TRUNCATE);
		#else
		strncpy(g_alert_path, log_path.c_str(), sizeof(g_alert_path) - 1);
		g_alert_path[sizeof(g_alert_path) - 1] = 0;
		#endif
	}
}

void crash_handler_refresh() {
	remember_log_path();
}

void crash_handler_install(bool alert) {
	bool expected = false;
	if (!g_installed.compare_exchange_strong(expected, true)) return;
	g_alert = alert;
	remember_log_path();
	#ifdef _WIN32
	SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
	SymInitialize(GetCurrentProcess(), nullptr, TRUE); // Done now rather than at fault time, when allocating is a much worse idea.
	AddVectoredExceptionHandler(1, first_chance_handler);
	SetUnhandledExceptionFilter(unhandled_exception_filter);
	_set_purecall_handler(purecall_handler);
	_set_invalid_parameter_handler(invalid_parameter_handler);
	#else
	stack_t alt;
	alt.ss_sp = g_alt_stack;
	alt.ss_size = sizeof(g_alt_stack);
	alt.ss_flags = 0;
	sigaltstack(&alt, nullptr); // Without this a stack overflow cannot be reported, there would be no stack left to run the handler on.
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = signal_handler;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, nullptr);
	sigaction(SIGBUS, &action, nullptr);
	sigaction(SIGFPE, &action, nullptr);
	sigaction(SIGILL, &action, nullptr);
	sigaction(SIGABRT, &action, nullptr);
	sigaction(SIGSYS, &action, nullptr);
	#endif
	std::set_terminate(on_terminate);
}

void crash_handler_uninstall() {
	bool expected = true;
	if (!g_installed.compare_exchange_strong(expected, false)) return;
	#ifdef _WIN32
	SetUnhandledExceptionFilter(nullptr);
	SymCleanup(GetCurrentProcess());
	#else
	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGILL, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
	signal(SIGSYS, SIG_DFL);
	#endif
}

bool crash_handler_pending() { return g_has_pending.load(); }

std::string crash_handler_pending_description() {
	std::string text = "native error inside the engine";
	if (g_pending_name) {
		text += ": ";
		text += g_pending_name;
		char at[40];
		snprintf(at, sizeof(at), " at 0x%llx", g_pending_address);
		text += at;
	}
	std::string where = nvgt_log::path();
	if (where.empty()) where = nvgt_log::planned_path();
	if (!where.empty()) text += ", details written to " + where;
	return text;
}

void crash_handler_commit_pending() {
	if (!g_has_pending.exchange(false)) return;
	// This runs from ordinary code rather than from a handler, so when a log is open the report goes through it. Writing behind the channel's back would leave the next message it writes on top of the report.
	if (nvgt_log::running()) nvgt_log::write("nvgt.crash", NVGT_LOG_CRITICAL, std::string(g_pending, g_pending_used));
	else {
		nvgt_log::open_emergency();
		nvgt_log::raw(g_pending, g_pending_used);
	}
	nvgt_log::flush();
	g_pending_used = 0;
	g_reports_written++;
}

void crash_handler_report(const std::string& reason) {
	#ifdef _WIN32
	write_report(reason.c_str(), 0, nullptr, nullptr, nullptr);
	#else
	write_report(reason.c_str(), 0, nullptr);
	#endif
}
