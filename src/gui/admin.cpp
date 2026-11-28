// SPDX-License-Identifier: GPL-2.0-or-later
#include "admin.hpp"
#include "log.hpp"

#include "dialogs.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <glib/gstdio.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace beluga {

namespace {

// What Act as Administrator hands to the root Beluga: pkexec clears the environment.
const char *const kPassedEnv[] = {"WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY", "LANG", "LANGUAGE", "LC_ALL", "LC_MESSAGES",
                                  "LC_CTYPE", "LC_TIME", "LC_NUMERIC", "GDK_SCALE", "GDK_DPI_SCALE", "XCURSOR_THEME",
                                  "XCURSOR_SIZE", "GTK_THEME", nullptr};

bool passed(const char *key)
{
    for (int i = 0; kPassedEnv[i]; ++i)
        if (!strcmp(kPassedEnv[i], key)) return true;
    return false;
}

// The user who started this root Beluga: pkexec sets PKEXEC_UID, sudo SUDO_UID.
uid_t invoking_uid()
{
    for (const char *var : {"PKEXEC_UID", "SUDO_UID"})
        if (const char *v = g_getenv(var))
            if (*v) return (uid_t)g_ascii_strtoull(v, nullptr, 10);
    return 0;
}

std::string home_of(uid_t uid)
{
    if (uid == 0) return {};
    struct passwd *pw = getpwuid(uid);
    return pw && pw->pw_dir ? pw->pw_dir : "";
}

// The first Wayland socket in a user's runtime folder (sudo drops WAYLAND_DISPLAY).
std::string find_wayland_socket(uid_t uid)
{
    std::string dir = printf_str("/run/user/%u", (unsigned)uid);
    std::vector<std::string> found;
    if (GDir *d = g_dir_open(dir.c_str(), 0, nullptr)) {
        while (const char *n = g_dir_read_name(d)) {
            struct stat st;
            std::string p = dir + "/" + n;
            if (starts_with(n, "wayland-") && !ends_with(n, ".lock") && stat(p.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) found.push_back(p);
        }
        g_dir_close(d);
    }
    std::sort(found.begin(), found.end());
    return found.empty() ? std::string() : found[0];
}

} // namespace

bool running_as_root() { return geteuid() == 0; }

void prepare_root_environment(int &argc, char **&argv)
{
    // --root-env KEY=VALUE pairs from Act as Administrator (only these keys, and only as root).
    std::vector<char *> keep;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && !strcmp(argv[i], "--root-env") && i + 1 < argc) {
            const char *kv = argv[++i];
            const char *eq = strchr(kv, '=');
            if (geteuid() == 0 && eq) {
                std::string key(kv, eq - kv);
                if (passed(key.c_str())) g_setenv(key.c_str(), eq + 1, TRUE);
            }
            continue;
        }
        keep.push_back(argv[i]);
    }
    static std::vector<char *> args;
    args = keep;
    argc = (int)args.size();
    args.push_back(nullptr);
    argv = args.data();
    if (geteuid() != 0) return;

    uid_t user = invoking_uid();
    // The display: a relative WAYLAND_DISPLAY lives in the user's runtime folder, which isn't root's.
    const char *wd = g_getenv("WAYLAND_DISPLAY");
    const char *runtime = g_getenv("XDG_RUNTIME_DIR");
    if (wd && *wd && wd[0] != '/' && runtime && *runtime) g_setenv("WAYLAND_DISPLAY", (std::string(runtime) + "/" + wd).c_str(), TRUE);
    if ((!wd || !*wd) && !g_getenv("DISPLAY") && user != 0) {
        std::string sock = find_wayland_socket(user);
        if (!sock.empty()) g_setenv("WAYLAND_DISPLAY", sock.c_str(), TRUE);
    }
    // A runtime folder of root's own, so nothing (dconf, D-Bus) writes root-owned files into the user's.
    struct stat st;
    if (!runtime || stat(runtime, &st) != 0 || st.st_uid != 0) {
        std::string own = "/run/user/0";
        if (stat(own.c_str(), &st) != 0 || st.st_uid != 0 || !S_ISDIR(st.st_mode)) {
            char tmpl[] = "/tmp/beluga-root-XXXXXX";
            own = g_mkdtemp_full(tmpl, 0700) ? tmpl : "";
        }
        if (!own.empty()) g_setenv("XDG_RUNTIME_DIR", own.c_str(), TRUE);
        else g_unsetenv("XDG_RUNTIME_DIR");
    }
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);   // no dconf writes as root
    g_setenv("NO_AT_BRIDGE", "1", TRUE);              // the user's accessibility bus isn't root's
    g_setenv("GTK_A11Y", "none", TRUE);
    g_setenv("GTK_USE_PORTAL", "0", TRUE);            // portals answer the user's session, not root
    const char *dbg = g_getenv("GDK_DEBUG");
    g_setenv("GDK_DEBUG", dbg && *dbg ? (std::string(dbg) + ",no-portals").c_str() : "no-portals", TRUE);
    // Themes and icons installed in the user's home.
    std::string home = home_of(user);
    if (!home.empty()) {
        const char *dirs = g_getenv("XDG_DATA_DIRS");
        std::string d = home + "/.local/share:" + (dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
        g_setenv("XDG_DATA_DIRS", d.c_str(), TRUE);
    }
    // A D-Bus session of root's own: the user's bus refuses root, and without one GLib would start a bus that
    // outlives Beluga. dbus-run-session ends it with Beluga.
    const char *bus = g_getenv("DBUS_SESSION_BUS_ADDRESS");
    bool ownBus = bus && *bus && (!strstr(bus, "/run/user/") || strstr(bus, "/run/user/0/"));
    if (!ownBus && !g_getenv("BELUGA_ROOT_BUS")) {
        char *drs = g_find_program_in_path("dbus-run-session");
        if (drs) {
            g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
            g_setenv("BELUGA_ROOT_BUS", "1", TRUE);
            std::vector<char *> exec{drs, (char *)"--"};
            char *self = g_file_read_link("/proc/self/exe", nullptr);
            exec.push_back(self ? self : argv[0]);
            for (int i = 1; i < argc; ++i) exec.push_back(argv[i]);
            exec.push_back(nullptr);
            execv(drs, exec.data());
            g_free(self);   // only when exec failed: carry on without a bus
            g_free(drs);
        }
        g_unsetenv("DBUS_SESSION_BUS_ADDRESS");
    }
}

void use_invoking_users_theme()
{
    if (geteuid() != 0) return;
    std::string home = home_of(invoking_uid());
    if (home.empty()) return;
    std::string dir = home + "/.config/gtk-4.0";
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, (dir + "/settings.ini").c_str(), G_KEY_FILE_NONE, nullptr)) {
        GtkSettings *gs = gtk_settings_get_default();
        gsize n = 0;
        char **keys = g_key_file_get_keys(kf, "Settings", &n, nullptr);
        for (gsize i = 0; keys && i < n; ++i) {
            GParamSpec *ps = g_object_class_find_property(G_OBJECT_GET_CLASS(gs), keys[i]);
            if (!ps || (ps->flags & G_PARAM_DEPRECATED) || !(ps->flags & G_PARAM_WRITABLE)) continue;
            if (G_PARAM_SPEC_VALUE_TYPE(ps) == G_TYPE_STRING) {
                char *v = g_key_file_get_string(kf, "Settings", keys[i], nullptr);
                if (v) g_object_set(gs, keys[i], v, nullptr);
                g_free(v);
            } else if (G_PARAM_SPEC_VALUE_TYPE(ps) == G_TYPE_BOOLEAN) {
                g_object_set(gs, keys[i], g_key_file_get_boolean(kf, "Settings", keys[i], nullptr), nullptr);
            } else if (G_PARAM_SPEC_VALUE_TYPE(ps) == G_TYPE_INT) {
                g_object_set(gs, keys[i], g_key_file_get_integer(kf, "Settings", keys[i], nullptr), nullptr);
            }
        }
        g_strfreev(keys);
    }
    g_key_file_free(kf);
    // The user's style sheet (pywal colours), as GTK loads it for the user.
    std::string css = dir + "/gtk.css";
    if (g_file_test(css.c_str(), G_FILE_TEST_EXISTS)) {
        GtkCssProvider *p = gtk_css_provider_new();
        gtk_css_provider_load_from_path(p, css.c_str());
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_object_unref(p);
    }
    GtkIconTheme *it = gtk_icon_theme_get_for_display(gdk_display_get_default());
    gtk_icon_theme_add_search_path(it, (home + "/.icons").c_str());
}

void act_as_admin(GtkWidget *parent, const std::string &uri)
{
    auto run = [parent, uri] {
        char *pkexec = g_find_program_in_path("pkexec");
        BLOG_INFO("admin", "Act as Administrator for %s", uri.c_str());
        if (!pkexec) {
            BLOG_ERROR("admin", "pkexec not found");
            show_error(parent, _("Beluga could not act as administrator."),
                       _("pkexec (polkit) is needed to ask for the administrator password. Install polkit and a polkit "
                         "agent for your desktop."));
            return;
        }
        char *self = g_file_read_link("/proc/self/exe", nullptr);
        std::vector<std::string> args{pkexec, self ? self : "beluga"};
        g_free(self);
        g_free(pkexec);
        for (int i = 0; kPassedEnv[i]; ++i) {
            const char *v = g_getenv(kPassedEnv[i]);
            if (!v || !*v) continue;
            std::string value = v;
            // pkexec's root doesn't share the user's runtime folder: the Wayland socket by its full path.
            if (!strcmp(kPassedEnv[i], "WAYLAND_DISPLAY") && value[0] != '/')
                if (const char *rt = g_getenv("XDG_RUNTIME_DIR")) value = std::string(rt) + "/" + value;
            args.push_back("--root-env");
            args.push_back(std::string(kPassedEnv[i]) + "=" + value);
        }
        args.push_back("--new-window");
        args.push_back(uri);
        std::vector<const char *> argv;
        for (auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);
        GError *err = nullptr;
        GSubprocess *p = g_subprocess_newv(argv.data(), G_SUBPROCESS_FLAGS_STDIN_INHERIT, &err);
        if (!p) {
            show_error(parent, _("Beluga could not act as administrator."), err ? err->message : "");
            g_clear_error(&err);
            return;
        }
        // pkexec ends with 126 when the password prompt was dismissed and 127 when it was refused.
        g_subprocess_wait_async(p, nullptr, [](GObject *o, GAsyncResult *r, gpointer) {
            g_subprocess_wait_finish(G_SUBPROCESS(o), r, nullptr);
            g_object_unref(o);
        }, nullptr);
    };
    if (!settings().confirmActAsAdmin) {
        run();
        return;
    }
    ask(parent, _("Risks of Acting as an Administrator"),
        _("A Beluga window with administrator rights can change and delete any file on this computer, including the "
          "files the system needs to start. A mistake there can make the system unusable. Use it only for files you "
          "can't change otherwise, and close it when you're done.\n\nBeluga asks for the administrator password next."),
        {_("Cancel"), _("I Understand and Accept These Risks")}, 0, 0, [run](int r) {
            if (r == 1) run();
        });
}

} // namespace beluga
