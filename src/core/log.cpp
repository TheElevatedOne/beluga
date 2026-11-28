// SPDX-License-Identifier: GPL-2.0-or-later
#include "log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace beluga {

namespace {
std::atomic<int> g_level{LogOff};
std::mutex g_mutex;
FILE *g_file = nullptr;
} // namespace

void log_init(int level, const std::string &file)
{
    g_level = level < LogOff ? LogOff : level > LogDebug ? LogDebug : level;
    if (!file.empty() && level > LogOff) {
        g_file = fopen(file.c_str(), "a");
        if (!g_file) std::fprintf(stderr, "beluga: can't write the log to %s\n", file.c_str());
    }
    if (g_level > LogOff)
        log_write(LogOff, "beluga", "Beluga %s, log level %d%s%s", BELUGA_VERSION, (int)g_level, g_file ? ", file " : "",
                  g_file ? file.c_str() : "");
}

int log_parse_level(const std::string &s)
{
    if (s == "0" || s == "off") return LogOff;
    if (s == "1" || s == "errors" || s == "error") return LogError;
    if (s.empty() || s == "2" || s == "info" || s == "actions") return LogInfo;
    if (s == "3" || s == "debug" || s == "details") return LogDebug;
    return -1;
}

int log_level() { return g_level.load(std::memory_order_relaxed); }

void log_write(int level, const char *area, const char *fmt, ...)
{
    if (log_level() < level) return;
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GDateTime *now = g_date_time_new_now_local();
    char *t = g_date_time_format(now, "%H:%M:%S");
    static const char tags[] = {'*', 'E', 'I', 'D'};   // * the start line, shown at every level
    char *line = g_strdup_printf("%s.%03d [%c] %s: %s\n", t, g_date_time_get_microsecond(now) / 1000,
                                 tags[level < 0 ? 0 : level > 3 ? 3 : level], area, msg);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::fputs(line, stderr);
        if (g_file) {
            std::fputs(line, g_file);
            std::fflush(g_file);
        }
    }
    g_free(line);
    g_free(t);
    g_date_time_unref(now);
    g_free(msg);
}

void log_write_raw_to_file(const std::string &line)
{
    size_t b = line.find_first_not_of('\n'), e = line.find_last_not_of('\n');
    if (b == std::string::npos) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    std::fputs(line.substr(b, e - b + 1).c_str(), g_file);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

} // namespace beluga
