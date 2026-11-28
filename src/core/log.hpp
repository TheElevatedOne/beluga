// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Logging for bug reports: off unless `beluga --log[=1|2|3]` or BELUGA_LOG=1|2|3.
//   1 errors   what failed (listing, mounting, file operations, programs that didn't start)
//   2 actions  plus what Beluga did (folders opened, file operations, searches, mounts, portal requests)
//   3 details  plus timings, counts and every file of a file operation
// Lines go to stderr and, with --log-file=FILE, to that file too:
//   12:04:31.207 [I] view: open sftp://host/srv

#include <glib.h>

#include <string>

namespace beluga {

enum LogLevel { LogOff = 0, LogError = 1, LogInfo = 2, LogDebug = 3 };

// level: 0–3; file: also append to it (empty: stderr only). Safe to call once, before threads start.
void log_init(int level, const std::string &file);
// Parses "1", "2", "3", "errors", "info", "actions", "debug", "details"; -1 when it's none of them.
int log_parse_level(const std::string &s);
int log_level();
void log_write(int level, const char *area, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
// A line written as is (GLib's own warnings), only to the log file.
void log_write_raw_to_file(const std::string &line);

} // namespace beluga

#define BLOG(level, area, ...)                                                                                         \
    do {                                                                                                               \
        if (::beluga::log_level() >= (level)) ::beluga::log_write((level), (area), __VA_ARGS__);                     \
    } while (0)
#define BLOG_ERROR(area, ...) BLOG(::beluga::LogError, area, __VA_ARGS__)
#define BLOG_INFO(area, ...) BLOG(::beluga::LogInfo, area, __VA_ARGS__)
#define BLOG_DEBUG(area, ...) BLOG(::beluga::LogDebug, area, __VA_ARGS__)
