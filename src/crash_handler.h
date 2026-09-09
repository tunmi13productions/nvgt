/* crash_handler.h - writes a report to the log when the process is about to die
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

// alert asks for a message box naming the log file when the process dies, which is how a player learns there is a file worth sending back.
void crash_handler_install(bool alert = true);
void crash_handler_uninstall();
// Picks up the log location again after later configuration has moved it.
void crash_handler_refresh();
// Writes a report for a condition the process detected itself rather than one the OS trapped.
void crash_handler_report(const std::string& reason);
// True once a native fault has been captured but not yet written out.
bool crash_handler_pending();
// Writes out a captured fault. A fault that something swallowed leaves no other trace, so this is called wherever the program discovers that it went wrong.
void crash_handler_commit_pending();
// A one line description of the most recent captured fault, suitable for handing to a script as an exception message.
std::string crash_handler_pending_description();
