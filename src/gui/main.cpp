// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: program entry point and command line.
#include "admin.hpp"
#include "app.hpp"
#include "log.hpp"
#include "mainwindow.hpp"
#include "settings.hpp"

#include <glib-unix.h>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <libintl.h>

namespace beluga {
int run_selftest(int argc, char **argv);
int run_portal(int argc, char **argv);
bool save_widget_png(GtkWidget *w, const std::string &path, int retries);
}

using namespace beluga;

// --detach, like kitty's: the terminal gets its prompt back at once and Beluga keeps running when the terminal
// closes (a new session, no terminal, output to /dev/null). Without a location it opens the current folder.
// Returns the arguments to continue with (in the child); the parent exits.
static std::vector<char *> detach(int argc, char **argv)
{
    std::vector<char *> args;
    bool detachAsked = false, location = false, endOfOptions = false;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && !endOfOptions && !strcmp(argv[i], "--detach")) {
            detachAsked = true;
            continue;
        }
        if (i > 0 && !strcmp(argv[i], "--")) endOfOptions = true;
        else if (i > 0 && (endOfOptions || argv[i][0] != '-')) location = true;
        args.push_back(argv[i]);
    }
    if (!detachAsked) return {argv, argv + argc};
    if (!location) {
        static std::string cwd;
        char *d = g_get_current_dir();
        cwd = d;
        g_free(d);
        args.push_back(cwd.data());
    }
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("beluga: --detach");
        std::exit(1);
    }
    if (pid > 0) std::exit(0);
    setsid();
    int null = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);
        if (null > STDERR_FILENO) close(null);
    }
    return args;
}

static int on_command_line(GApplication *gapp, GApplicationCommandLine *cmd, gpointer)
{
    GVariantDict *opts = g_application_command_line_get_options_dict(cmd);
    gboolean select = FALSE, split = FALSE, newWindow = FALSE, daemon = FALSE, version = FALSE;
    g_variant_dict_lookup(opts, "select", "b", &select);
    g_variant_dict_lookup(opts, "split", "b", &split);
    g_variant_dict_lookup(opts, "new-window", "b", &newWindow);
    g_variant_dict_lookup(opts, "daemon", "b", &daemon);
    g_variant_dict_lookup(opts, "version", "b", &version);
    if (version) {
        g_application_command_line_print(cmd, "beluga %s\n", BELUGA_VERSION);
        return 0;
    }
    std::vector<std::string> uris;
    const char **args = nullptr;
    if (g_variant_dict_lookup(opts, G_OPTION_REMAINING, "^a&ay", &args)) {
        for (int i = 0; args[i]; ++i) {
            GFile *f = g_application_command_line_create_file_for_arg(cmd, args[i]);
            uris.push_back(file_uri(f));
            g_object_unref(f);
        }
        g_free(args);
    }
    App &app = App::instance();
    if (daemon) {
        static bool held = false;
        if (!held) {
            g_application_hold(gapp);
            held = true;
        }
        if (uris.empty()) return 0;
    }
    if (uris.empty()) {
        if (app.windows().empty() && !newWindow && settings().rememberOpenedTabs && app.restoreSession()) return 0;
        app.newWindow({}, split);
        return 0;
    }
    app.openLocations(uris, select, split, newWindow || app.windows().empty());
    return 0;
}

// GTK warns about frame timings on some compositors (Hyprland: "Duplicate call to gdk_frame_timings_presented()",
// "called on discarded frame"). They say nothing about Beluga and nothing can be done about them; leave them out.
static GLogWriterOutput log_writer(GLogLevelFlags level, const GLogField *fields, gsize n, gpointer data)
{
    for (gsize i = 0; i < n; ++i)
        if (!strcmp(fields[i].key, "MESSAGE") && fields[i].length != 0 && fields[i].value
            && strstr(static_cast<const char *>(fields[i].value), "gdk_frame_timings"))
            return G_LOG_WRITER_HANDLED;
    // With --log-file, GLib's and GTK's own warnings go into the file too, next to Beluga's lines.
    if (log_level() > LogOff && (level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING | G_LOG_LEVEL_MESSAGE))) {
        char *line = g_log_writer_format_fields(level, fields, n, FALSE);
        log_write_raw_to_file(line);
        g_free(line);
    }
    return g_log_writer_default(level, fields, n, data);
}

// --log[=LEVEL] and --log-file=FILE (or --log-file FILE) are taken out of argv before anything else, so they work
// for --portal and --selftest too. BELUGA_LOG=LEVEL and BELUGA_LOG_FILE=FILE do the same for copies D-Bus starts.
// Returns whether --log was on the command line.
static bool take_log_options(int &argc, char **argv)
{
    int level = LogOff;
    std::string file;
    if (const char *e = g_getenv("BELUGA_LOG")) level = std::max(0, log_parse_level(e));
    if (const char *e = g_getenv("BELUGA_LOG_FILE")) file = e;
    bool given = false;
    int out = 1;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") {
            for (; i < argc; ++i) argv[out++] = argv[i];
            break;
        }
        if (a == "--log" || a.rfind("--log=", 0) == 0) {
            int l = log_parse_level(a == "--log" ? std::string() : a.substr(6));
            if (l < 0) {
                std::fprintf(stderr, "beluga: unknown log level \"%s\" (use 1, 2 or 3)\n", a.substr(6).c_str());
                std::exit(2);
            }
            level = l;
            given = true;
            continue;
        }
        if (a.rfind("--log-file=", 0) == 0) {
            file = a.substr(11);
            continue;
        }
        if (a == "--log-file" && i + 1 < argc) {
            file = argv[++i];
            continue;
        }
        argv[out++] = argv[i];
    }
    argv[out] = nullptr;
    argc = out;
    // A log file alone means the default level.
    if (level == LogOff && !file.empty()) level = LogInfo;
    log_init(level, file);
    return given;
}

int main(int argc, char **argv)
{
    g_log_set_writer_func(log_writer, nullptr, nullptr);
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, BELUGA_LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
    bool logRequested = take_log_options(argc, argv);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--selftest")) return run_selftest(argc, argv);
        if (!strcmp(argv[i], "--portal")) return run_portal(argc, argv);
    }
    std::vector<char *> args = detach(argc, argv);
    argc = (int)args.size();
    args.push_back(nullptr);
    argv = args.data();
    prepare_root_environment(argc, argv);   // as root (Act as Administrator, sudo): a display, a bus and a runtime folder

    settings().load();
    // A Beluga that is already running would get this call and the log would stay empty: --log runs its own.
    GApplicationFlags flags = G_APPLICATION_HANDLES_COMMAND_LINE;
    if (logRequested) flags = GApplicationFlags(flags | G_APPLICATION_NON_UNIQUE);
    GtkApplication *app = gtk_application_new(BELUGA_APP_ID, flags);
    static const GOptionEntry entries[] = {
        {"select", 's', 0, G_OPTION_ARG_NONE, nullptr, N_("Select the given files in their folders"), nullptr},
        {"split", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Open the window in split view"), nullptr},
        {"new-window", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Always open a new window"), nullptr},
        {"daemon", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Keep running in the background for the FileManager1 D-Bus service"), nullptr},
        {"detach", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Detach from the terminal (without a location: open the current folder)"), nullptr},
        {"portal", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Run the file chooser for xdg-desktop-portal (started by D-Bus)"), nullptr},
        {"version", 'v', 0, G_OPTION_ARG_NONE, nullptr, N_("Show the version"), nullptr},
        // Read before GApplication parses (take_log_options); listed here for --help.
        {"log", 0, 0, G_OPTION_ARG_STRING, nullptr,
         N_("Log to the terminal: 1 errors, 2 also actions (the default), 3 also details; runs a Beluga of its own"), "1|2|3"},
        {"log-file", 0, 0, G_OPTION_ARG_FILENAME, nullptr, N_("Also write the log to this file"), N_("FILE")},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, nullptr, nullptr, N_("[FILE or FOLDER…]")},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr},
    };
    g_application_add_main_option_entries(G_APPLICATION(app), entries);
    g_application_set_option_context_summary(G_APPLICATION(app), "Beluga: a file manager (GTK 4)");
    g_signal_connect(app, "startup", G_CALLBACK(+[](GtkApplication *a) { App::instance().init(a); }), nullptr);
    g_signal_connect(app, "shutdown", G_CALLBACK(+[](GtkApplication *) { App::instance().shutdown(); }), nullptr);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), nullptr);
    // --version before GTK starts, so it works without a display (ssh, CI, scripts).
    g_signal_connect(app, "handle-local-options", G_CALLBACK(+[](GApplication *, GVariantDict *opts) -> int {
        if (!g_variant_dict_contains(opts, "version")) return -1;
        std::printf("beluga %s\n", BELUGA_VERSION);
        return 0;
    }), nullptr);
    // SIGTERM/SIGINT/SIGHUP (logout, `kill`, Ctrl+C): save the session and exit cleanly.
    for (int sig : {SIGTERM, SIGINT, SIGHUP})
        g_unix_signal_add(sig, [](gpointer a) -> gboolean {
            App::instance().quit();
            g_application_quit(G_APPLICATION(a));
            return G_SOURCE_REMOVE;
        }, app);
    // Debugging aid: BELUGA_SCREENSHOT=file.png saves the first window after 3 seconds.
    if (const char *shot = g_getenv("BELUGA_SCREENSHOT"))
        g_timeout_add_seconds(3, [](gpointer p) -> gboolean {
            const auto &wins = App::instance().windows();
            if (!wins.empty()) save_widget_png(wins.front()->widget(), static_cast<const char *>(p), 60);
            return G_SOURCE_REMOVE;
        }, (gpointer)shot);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
