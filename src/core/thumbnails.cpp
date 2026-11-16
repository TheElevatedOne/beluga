// SPDX-License-Identifier: GPL-2.0-or-later
#include "thumbnails.hpp"

#include "fileops.hpp"
#include "settings.hpp"
#include "util.hpp"
#include "workqueue.hpp"

#include <glib/gstdio.h>

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace beluga::thumbs {

namespace {

struct Thumbnailer {
    std::string exec;
    std::vector<std::string> mimes;
};

std::mutex g_mutex;
bool g_initialised = false;
std::set<std::string> g_pixbufMimes;
std::vector<Thumbnailer> g_thumbnailers;
std::set<std::string> g_tumblerMimes;
bool g_tumblerChecked = false;

WorkQueue &queue()
{
    static WorkQueue *q = new WorkQueue(3, true);   // never destroyed: threads wait on it at exit
    return *q;
}

std::string cache_root() { return std::string(g_get_user_cache_dir()) + "/thumbnails"; }

const char *flavor_for(int px)
{
    if (px <= 128) return "normal";
    if (px <= 256) return "large";
    if (px <= 512) return "x-large";
    return "xx-large";
}

int flavor_pixels(const char *f)
{
    if (!strcmp(f, "normal")) return 128;
    if (!strcmp(f, "large")) return 256;
    if (!strcmp(f, "x-large")) return 512;
    return 1024;
}

std::string md5(const std::string &s) { return take(g_compute_checksum_for_string(G_CHECKSUM_MD5, s.c_str(), -1)); }

std::string thumb_path(const Request &r) { return cache_root() + "/" + flavor_for(r.pixels) + "/" + md5(r.uri) + ".png"; }
std::string fail_path(const Request &r) { return cache_root() + "/fail/beluga/" + md5(r.uri) + ".png"; }

void load_thumbnailers()
{
    std::vector<std::string> dirs;
    dirs.push_back(std::string(g_get_user_data_dir()) + "/thumbnailers");
    for (const char *const *d = g_get_system_data_dirs(); *d; ++d) dirs.push_back(std::string(*d) + "/thumbnailers");
    std::set<std::string> seen;
    for (auto &dir : dirs) {
        GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
        if (!d) continue;
        while (const char *name = g_dir_read_name(d)) {
            if (!g_str_has_suffix(name, ".thumbnailer") || seen.count(name)) continue;
            seen.insert(name);
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, (dir + "/" + name).c_str(), G_KEY_FILE_NONE, nullptr)) {
                const char *G = "Thumbnailer Entry";
                std::string tryExec = take(g_key_file_get_string(kf, G, "TryExec", nullptr));
                std::string exec = take(g_key_file_get_string(kf, G, "Exec", nullptr));
                bool ok = !exec.empty();
                if (ok && !tryExec.empty()) {
                    char *found = g_find_program_in_path(tryExec.c_str());
                    ok = found != nullptr;
                    g_free(found);
                }
                if (ok) {
                    Thumbnailer t;
                    t.exec = exec;
                    t.mimes = split(take(g_key_file_get_string(kf, G, "MimeType", nullptr)), ';');
                    g_thumbnailers.push_back(t);
                }
            }
            g_key_file_free(kf);
        }
        g_dir_close(d);
    }
}

void init()
{
    std::lock_guard<std::mutex> l(g_mutex);
    if (g_initialised) return;
    g_initialised = true;
    GSList *formats = gdk_pixbuf_get_formats();
    for (GSList *f = formats; f; f = f->next) {
        auto *fmt = static_cast<GdkPixbufFormat *>(f->data);
        if (gdk_pixbuf_format_is_disabled(fmt)) continue;
        char **mimes = gdk_pixbuf_format_get_mime_types(fmt);
        for (int i = 0; mimes && mimes[i]; ++i) g_pixbufMimes.insert(mimes[i]);
        g_strfreev(mimes);
    }
    g_slist_free(formats);
    load_thumbnailers();
}

void check_tumbler()
{
    {
        std::lock_guard<std::mutex> l(g_mutex);
        if (g_tumblerChecked) return;
        g_tumblerChecked = true;
    }
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return;
    GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.thumbnails.Thumbnailer1",
                                              "/org/freedesktop/thumbnails/Thumbnailer1",
                                              "org.freedesktop.thumbnails.Thumbnailer1", "GetSupported", nullptr,
                                              G_VARIANT_TYPE("(asas)"), G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
    if (r) {
        GVariantIter *schemes = nullptr, *mimes = nullptr;
        g_variant_get(r, "(asas)", &schemes, &mimes);
        const char *scheme, *mime;
        std::lock_guard<std::mutex> l(g_mutex);
        while (g_variant_iter_next(schemes, "&s", &scheme) && g_variant_iter_next(mimes, "&s", &mime))
            if (!strcmp(scheme, "file")) g_tumblerMimes.insert(mime);
        g_variant_iter_free(schemes);
        g_variant_iter_free(mimes);
        g_variant_unref(r);
    }
    g_object_unref(bus);
}

bool mime_in(const std::vector<std::string> &list, const std::string &mime)
{
    for (auto &m : list)
        if (m == mime || (ends_with(m, "/*") && starts_with(mime, m.substr(0, m.size() - 1)))) return true;
    return false;
}

const Thumbnailer *thumbnailer_for(const std::string &mime)
{
    for (auto &t : g_thumbnailers)
        if (mime_in(t.mimes, mime)) return &t;
    return nullptr;
}

bool valid_for(GdkPixbuf *p, const Request &r)
{
    const char *mt = gdk_pixbuf_get_option(p, "tEXt::Thumb::MTime");
    return mt && g_ascii_strtoll(mt, nullptr, 10) == r.mtime;
}

GdkPixbuf *load_valid(const std::string &path, const Request &r)
{
    if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) return nullptr;
    GdkPixbuf *p = gdk_pixbuf_new_from_file(path.c_str(), nullptr);
    if (p && !valid_for(p, r)) {
        g_object_unref(p);
        return nullptr;
    }
    return p;
}

void save(GdkPixbuf *p, const std::string &path, const Request &r)
{
    g_mkdir_with_parents(dirname_of(path).c_str(), 0700);
    std::string tmp = path + ".tmp-" + std::to_string(getpid()) + "-" + std::to_string(g_random_int());
    std::string mtime = std::to_string(r.mtime);
    std::string size = std::to_string(r.size);
    char *keys[] = {(char *)"tEXt::Thumb::URI", (char *)"tEXt::Thumb::MTime", (char *)"tEXt::Thumb::Size",
                    (char *)"tEXt::Software", nullptr};
    char *values[] = {(char *)r.uri.c_str(), (char *)mtime.c_str(), (char *)size.c_str(), (char *)"Beluga", nullptr};
    if (gdk_pixbuf_savev(p, tmp.c_str(), "png", keys, values, nullptr)) {
        g_chmod(tmp.c_str(), 0600);
        g_rename(tmp.c_str(), path.c_str());
    } else {
        g_unlink(tmp.c_str());
    }
}

void mark_failed(const Request &r)
{
    GdkPixbuf *p = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
    gdk_pixbuf_fill(p, 0);
    save(p, fail_path(r), r);
    g_object_unref(p);
}

GdkPixbuf *scale_to(GdkPixbuf *src, int edge)
{
    int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
    if (w <= edge && h <= edge) return GDK_PIXBUF(g_object_ref(src));
    double s = std::min((double)edge / w, (double)edge / h);
    return gdk_pixbuf_scale_simple(src, std::max(1, (int)(w * s)), std::max(1, (int)(h * s)), GDK_INTERP_BILINEAR);
}

GdkPixbuf *from_pixbuf_loader(const Request &r, int edge)
{
    int w = 0, h = 0;
    if (!gdk_pixbuf_get_file_info(r.path.c_str(), &w, &h)) return nullptr;
    GdkPixbuf *p = nullptr;
    if (w > 0 && h > 0 && w <= edge && h <= edge) p = gdk_pixbuf_new_from_file(r.path.c_str(), nullptr);
    else p = gdk_pixbuf_new_from_file_at_scale(r.path.c_str(), edge, edge, TRUE, nullptr);
    if (!p) return nullptr;
    GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation(p);
    g_object_unref(p);
    return o;
}

GdkPixbuf *from_thumbnailer(const Thumbnailer &t, const Request &r, int edge)
{
    std::string out = std::string(g_get_tmp_dir()) + "/beluga-thumb-" + std::to_string(getpid()) + "-" + std::to_string(g_random_int()) + ".png";
    std::string cmd;
    for (size_t i = 0; i < t.exec.size(); ++i) {
        if (t.exec[i] == '%' && i + 1 < t.exec.size()) {
            char c = t.exec[++i];
            if (c == 'i') cmd += shell_quote(r.path.empty() ? r.uri : r.path);
            else if (c == 'u') cmd += shell_quote(r.uri);
            else if (c == 'o') cmd += shell_quote(out);
            else if (c == 's') cmd += std::to_string(edge);
            else if (c == '%') cmd += '%';
        } else {
            cmd += t.exec[i];
        }
    }
    int argc = 0;
    char **argv = nullptr;
    if (!g_shell_parse_argv(cmd.c_str(), &argc, &argv, nullptr)) return nullptr;
    GPid pid = 0;
    bool ok = g_spawn_async(nullptr, argv, nullptr,
                            GSpawnFlags(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                            nullptr, nullptr, &pid, nullptr);
    g_strfreev(argv);
    if (!ok) return nullptr;
    int status = 0;
    bool exited = false;
    for (int i = 0; i < 600; ++i) {   // 30 s
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            break;
        }
        g_usleep(50000);
    }
    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    g_spawn_close_pid(pid);
    GdkPixbuf *p = nullptr;
    if (exited && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        GdkPixbuf *raw = gdk_pixbuf_new_from_file(out.c_str(), nullptr);
        if (raw) {
            p = scale_to(raw, edge);
            g_object_unref(raw);
        }
    }
    g_unlink(out.c_str());
    return p;
}

struct TumblerWait {
    guint handle = 0;
    bool done = false;
};

void tumbler_signal(GDBusConnection *, const char *, const char *, const char *, const char *signal, GVariant *params, gpointer data)
{
    auto *w = static_cast<TumblerWait *>(data);
    guint h = 0;
    g_variant_get_child(params, 0, "u", &h);
    if (h == w->handle && !strcmp(signal, "Finished")) w->done = true;
}

GdkPixbuf *from_tumbler(const Request &r, int edge)
{
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    GdkPixbuf *result = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus) {
        TumblerWait w;
        guint sub = g_dbus_connection_signal_subscribe(bus, "org.freedesktop.thumbnails.Thumbnailer1",
                                                       "org.freedesktop.thumbnails.Thumbnailer1", nullptr,
                                                       "/org/freedesktop/thumbnails/Thumbnailer1", nullptr,
                                                       G_DBUS_SIGNAL_FLAGS_NONE, tumbler_signal, &w, nullptr);
        const char *uris[] = {r.uri.c_str(), nullptr};
        const char *mimes[] = {r.mime.c_str(), nullptr};
        Request flav = r;
        flav.pixels = edge;
        GVariant *res = g_dbus_connection_call_sync(bus, "org.freedesktop.thumbnails.Thumbnailer1",
                                                    "/org/freedesktop/thumbnails/Thumbnailer1",
                                                    "org.freedesktop.thumbnails.Thumbnailer1", "Queue",
                                                    g_variant_new("(^as^asssu)", uris, mimes, flavor_for(edge), "foreground", 0u),
                                                    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        if (res) {
            g_variant_get(res, "(u)", &w.handle);
            g_variant_unref(res);
            gint64 deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
            while (!w.done && g_get_monotonic_time() < deadline) {
                if (!g_main_context_iteration(ctx, FALSE)) g_usleep(20000);
            }
            result = load_valid(thumb_path(flav), r);
        }
        g_dbus_connection_signal_unsubscribe(bus, sub);
        while (g_main_context_iteration(ctx, FALSE)) {}
        g_object_unref(bus);
    }
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return result;
}

GdkPixbuf *generate(const Request &r)
{
    const char *flavor = flavor_for(r.pixels);
    int edge = flavor_pixels(flavor);
    Request fr = r;
    fr.pixels = edge;
    if (GdkPixbuf *p = load_valid(thumb_path(fr), r)) return p;
    if (load_valid(fail_path(r), r)) return nullptr;
    if (!r.path.empty() && starts_with(r.path, cache_root() + "/")) return nullptr;

    GdkPixbuf *p = nullptr;
    if (!r.path.empty() && g_pixbufMimes.count(r.mime)) p = from_pixbuf_loader(r, edge);
    if (!p) {
        if (const Thumbnailer *t = thumbnailer_for(r.mime)) p = from_thumbnailer(*t, r, edge);
    }
    if (!p) {
        check_tumbler();
        bool tumbler;
        {
            std::lock_guard<std::mutex> l(g_mutex);
            tumbler = g_tumblerMimes.count(r.mime) > 0;
        }
        if (tumbler && !r.path.empty()) return from_tumbler(r, edge);
    }
    if (p) save(p, thumb_path(fr), r);
    else mark_failed(r);
    return p;
}

} // namespace

bool supported(const std::string &mime, bool local)
{
    init();
    if (local && g_pixbufMimes.count(mime)) return true;
    if (thumbnailer_for(mime)) return true;
    std::lock_guard<std::mutex> l(g_mutex);
    return g_tumblerMimes.count(mime) > 0 || !g_tumblerChecked;
}

bool allowed(const Request &r)
{
    const Settings &s = settings();
    for (auto &pattern : s.disabledPreviewMime)
        if (pattern == r.mime || (ends_with(pattern, "/*") && starts_with(r.mime, pattern.substr(0, pattern.size() - 1)))) return false;
    if (r.path.empty()) {
        if (s.maxRemotePreviewMiB <= 0) return false;
        return r.size <= (guint64)s.maxRemotePreviewMiB << 20;
    }
    if (s.maxLocalPreviewMiB > 0 && r.size > (guint64)s.maxLocalPreviewMiB << 20) return false;
    return true;
}

GdkPixbuf *cached(const Request &r)
{
    Request fr = r;
    fr.pixels = flavor_pixels(flavor_for(r.pixels));
    return load_valid(thumb_path(fr), r);
}

void request(const Request &r, std::weak_ptr<void> owner, Callback cb)
{
    init();
    queue().push([r, owner, cb] {
        if (owner.expired()) return;
        GdkPixbuf *p = generate(r);
        run_on_main([owner, cb, p] {
            if (!owner.expired()) cb(p);
            if (p) g_object_unref(p);
        });
    });
}

} // namespace beluga::thumbs
