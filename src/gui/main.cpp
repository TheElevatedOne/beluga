// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: program entry point and command line.
#include "app.hpp"
#include "mainwindow.hpp"
#include "settings.hpp"

#include <glib-unix.h>

#include <clocale>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <libintl.h>

namespace beluga {
int run_selftest(int argc, char **argv);
bool save_widget_png(GtkWidget *w, const std::string &path, int retries);
}

using namespace beluga;

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

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, BELUGA_LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--selftest")) return run_selftest(argc, argv);

    settings().load();
    GtkApplication *app = gtk_application_new(BELUGA_APP_ID, G_APPLICATION_HANDLES_COMMAND_LINE);
    static const GOptionEntry entries[] = {
        {"select", 's', 0, G_OPTION_ARG_NONE, nullptr, N_("Select the given files in their folders"), nullptr},
        {"split", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Open the window in split view"), nullptr},
        {"new-window", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Always open a new window"), nullptr},
        {"daemon", 0, 0, G_OPTION_ARG_NONE, nullptr, N_("Keep running in the background for the FileManager1 D-Bus service"), nullptr},
        {"version", 'v', 0, G_OPTION_ARG_NONE, nullptr, N_("Show the version"), nullptr},
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
