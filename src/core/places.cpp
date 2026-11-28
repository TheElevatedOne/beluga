// SPDX-License-Identifier: GPL-2.0-or-later
#include "places.hpp"
#include "log.hpp"

#include "search.hpp"
#include "util.hpp"

#include <glib.h>

#include <cstring>

namespace beluga {

PlaceSection section_for_uri(const std::string &uri)
{
    if (is_recent_uri(uri)) return PlaceSection::Recent;
    if (is_search_uri(uri)) return PlaceSection::Search;
    if (starts_with(uri, "file:") || is_trash_uri(uri)) return PlaceSection::Places;
    return PlaceSection::Remote;
}

const char *section_title(PlaceSection s)
{
    switch (s) {
    case PlaceSection::Places: return _("Places");
    case PlaceSection::Remote: return _("Remote");
    case PlaceSection::Recent: return _("Recent");
    case PlaceSection::Search: return _("Search For");
    case PlaceSection::Devices: return _("Devices");
    }
    return "";
}

const char *section_id(PlaceSection s)
{
    switch (s) {
    case PlaceSection::Places: return "places";
    case PlaceSection::Remote: return "remote";
    case PlaceSection::Recent: return "recent";
    case PlaceSection::Search: return "search";
    case PlaceSection::Devices: return "devices";
    }
    return "";
}

static std::string uri_for_path(const std::string &p)
{
    return take(g_filename_to_uri(p.c_str(), nullptr, nullptr));
}

std::string Places::iconForUri(const std::string &uri)
{
    if (is_trash_uri(uri)) return "user-trash";
    if (is_recent_uri(uri)) return "document-open-recent";
    if (is_search_uri(uri)) return "system-search";
    if (starts_with(uri, "network:")) return "network-workgroup";
    if (!starts_with(uri, "file:")) return "folder-remote";
    char *p = g_filename_from_uri(uri.c_str(), nullptr, nullptr);
    std::string path = take(p);
    struct { GUserDirectory d; const char *icon; } dirs[] = {
        {G_USER_DIRECTORY_DESKTOP, "user-desktop"}, {G_USER_DIRECTORY_DOCUMENTS, "folder-documents"},
        {G_USER_DIRECTORY_DOWNLOAD, "folder-download"}, {G_USER_DIRECTORY_MUSIC, "folder-music"},
        {G_USER_DIRECTORY_PICTURES, "folder-pictures"}, {G_USER_DIRECTORY_VIDEOS, "folder-videos"},
        {G_USER_DIRECTORY_TEMPLATES, "folder-templates"}, {G_USER_DIRECTORY_PUBLIC_SHARE, "folder-publicshare"},
    };
    if (path == home_dir() || path == home_dir() + "/") return "user-home";
    if (path == "/") return "drive-harddisk";
    for (auto &d : dirs)
        if (!user_dir(d.d).empty() && user_dir(d.d) == path) return d.icon;
    return "folder";
}

std::vector<Place> default_places()
{
    std::vector<Place> v;
    v.push_back({_("Home"), uri_for_path(home_dir()), "user-home", false});
    struct { GUserDirectory d; const char *label; const char *icon; } dirs[] = {
        {G_USER_DIRECTORY_DESKTOP, N_("Desktop"), "user-desktop"},
        {G_USER_DIRECTORY_DOCUMENTS, N_("Documents"), "folder-documents"},
        {G_USER_DIRECTORY_DOWNLOAD, N_("Downloads"), "folder-download"},
        {G_USER_DIRECTORY_MUSIC, N_("Music"), "folder-music"},
        {G_USER_DIRECTORY_PICTURES, N_("Pictures"), "folder-pictures"},
        {G_USER_DIRECTORY_VIDEOS, N_("Videos"), "folder-videos"},
    };
    for (auto &d : dirs) {
        std::string p = user_dir(d.d);
        if (!p.empty() && g_file_test(p.c_str(), G_FILE_TEST_IS_DIR)) v.push_back({_(d.label), uri_for_path(p), d.icon, false});
    }
    v.push_back({_("Trash"), "trash:///", "user-trash", false});
    const char *const *schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
    for (int i = 0; schemes && schemes[i]; ++i)
        if (strcmp(schemes[i], "network") == 0) v.push_back({_("Network"), "network:///", "network-workgroup", false});
    v.push_back({_("Recent Files"), "recent:///", "document-open-recent", false});
    v.push_back({_("Documents"), search_uri_for_type("documents"), "folder-text", false});
    v.push_back({_("Images"), search_uri_for_type("images"), "folder-images", false});
    v.push_back({_("Audio"), search_uri_for_type("audio"), "folder-sound", false});
    v.push_back({_("Videos"), search_uri_for_type("videos"), "folder-videos", false});
    return v;
}

namespace {

// Minimal XBEL reader for the one-time import of an existing places file.
struct XbelState {
    std::vector<Place> out;
    Place cur;
    bool inBookmark = false;
    std::string text;
    bool hidden = false;
    bool hiddenKnown = false;
};

void xbel_start(GMarkupParseContext *, const char *element, const char **names, const char **values, gpointer data, GError **)
{
    auto *st = static_cast<XbelState *>(data);
    st->text.clear();
    if (strcmp(element, "bookmark") == 0) {
        st->inBookmark = true;
        st->cur = Place{};
        st->hidden = false;
        for (int i = 0; names[i]; ++i)
            if (strcmp(names[i], "href") == 0) st->cur.uri = values[i];
    } else if (st->inBookmark && strcmp(element, "bookmark:icon") == 0) {
        for (int i = 0; names[i]; ++i)
            if (strcmp(names[i], "name") == 0) st->cur.icon = values[i];
    }
}

void xbel_end(GMarkupParseContext *, const char *element, gpointer data, GError **)
{
    auto *st = static_cast<XbelState *>(data);
    if (!st->inBookmark) return;
    if (strcmp(element, "title") == 0) {
        st->cur.label = trim(st->text);
    } else if (strcmp(element, "IsHidden") == 0) {
        st->hidden = trim(st->text) == "true";
    } else if (strcmp(element, "bookmark") == 0) {
        st->inBookmark = false;
        Place p = st->cur;
        p.hidden = st->hidden;
        if (p.uri.empty()) return;
        if (starts_with(p.uri, "trash:")) p.uri = "trash:///";
        else if (starts_with(p.uri, "remote:")) p.uri = "network:///";
        else if (starts_with(p.uri, "recentlyused:/files")) p.uri = "recent:///";
        else if (starts_with(p.uri, "timeline:") || starts_with(p.uri, "recentlyused:")
                 || starts_with(p.uri, "baloosearch:") || starts_with(p.uri, "tags:")
                 || starts_with(p.uri, "activities:") || starts_with(p.uri, "stash:"))
            return;
        if (p.label.empty()) p.label = p.uri;
        if (p.icon.empty()) p.icon = Places::iconForUri(p.uri);
        st->out.push_back(p);
    }
}

void xbel_text(GMarkupParseContext *, const char *text, gsize len, gpointer data, GError **)
{
    static_cast<XbelState *>(data)->text.append(text, len);
}

std::vector<Place> import_xbel(const std::string &path)
{
    char *contents = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path.c_str(), &contents, &len, nullptr)) return {};
    XbelState st;
    GMarkupParser parser = {xbel_start, xbel_end, xbel_text, nullptr, nullptr};
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, G_MARKUP_DEFAULT_FLAGS, &st, nullptr);
    bool ok = g_markup_parse_context_parse(ctx, contents, len, nullptr);
    g_markup_parse_context_free(ctx);
    g_free(contents);
    return ok ? st.out : std::vector<Place>{};
}

std::vector<Place> import_gtk_bookmarks()
{
    std::vector<Place> v;
    std::string path = std::string(g_get_user_config_dir()) + "/gtk-3.0/bookmarks";
    char *contents = nullptr;
    if (!g_file_get_contents(path.c_str(), &contents, nullptr, nullptr)) return v;
    for (auto &line : split(take(contents), '\n')) {
        size_t sp = line.find(' ');
        Place p;
        p.uri = line.substr(0, sp);
        if (sp != std::string::npos) p.label = line.substr(sp + 1);
        if (p.label.empty()) {
            GFile *f = g_file_new_for_uri(p.uri.c_str());
            p.label = take(g_file_get_basename(f));
            g_object_unref(f);
        }
        p.icon = Places::iconForUri(p.uri);
        v.push_back(p);
    }
    return v;
}

} // namespace

Places &Places::instance()
{
    static Places p;
    return p;
}

Places::Places() { load(); }

void Places::load()
{
    std::string path = config_dir() + "/places.conf";
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path.c_str(), G_KEY_FILE_NONE, nullptr)) {
        gsize n = 0;
        char **groups = g_key_file_get_groups(kf, &n);
        for (gsize i = 0; i < n; ++i) {
            if (!starts_with(groups[i], "Place ")) continue;
            Place p;
            p.label = take(g_key_file_get_string(kf, groups[i], "Label", nullptr));
            p.uri = take(g_key_file_get_string(kf, groups[i], "Uri", nullptr));
            p.icon = take(g_key_file_get_string(kf, groups[i], "Icon", nullptr));
            p.hidden = g_key_file_get_boolean(kf, groups[i], "Hidden", nullptr);
            if (p.icon.empty()) p.icon = iconForUri(p.uri);
            if (!p.uri.empty()) places_.push_back(p);
        }
        g_strfreev(groups);
        g_key_file_free(kf);
        return;
    }
    g_key_file_free(kf);

    places_ = import_xbel(std::string(g_get_user_data_dir()) + "/user-places.xbel");
    if (places_.empty()) {
        places_ = default_places();
        for (auto &b : import_gtk_bookmarks())
            if (indexOf(b.uri) < 0) places_.push_back(b);
    } else {
        // Keep the Recent and Search For entries that KDE's file never had.
        for (auto &d : default_places()) {
            PlaceSection s = section_for_uri(d.uri);
            if ((s == PlaceSection::Recent || s == PlaceSection::Search) && indexOf(d.uri) < 0) places_.push_back(d);
        }
    }
    save();
}

void Places::save()
{
    GKeyFile *kf = g_key_file_new();
    for (size_t i = 0; i < places_.size(); ++i) {
        std::string g = printf_str("Place %03zu", i);
        g_key_file_set_string(kf, g.c_str(), "Label", places_[i].label.c_str());
        g_key_file_set_string(kf, g.c_str(), "Uri", places_[i].uri.c_str());
        g_key_file_set_string(kf, g.c_str(), "Icon", places_[i].icon.c_str());
        g_key_file_set_boolean(kf, g.c_str(), "Hidden", places_[i].hidden);
    }
    g_key_file_save_to_file(kf, (config_dir() + "/places.conf").c_str(), nullptr);
    g_key_file_free(kf);
}

void Places::changed()
{
    save();
    auto copy = listeners_;
    for (auto &[id, l] : copy) l();
}

void Places::add(const Place &p, int index)
{
    if (index < 0 || index > (int)places_.size()) places_.push_back(p);
    else places_.insert(places_.begin() + index, p);
    BLOG_INFO("places", "add \"%s\" %s", p.label.c_str(), p.uri.c_str());
    changed();
}

void Places::remove(int index)
{
    if (index < 0 || index >= (int)places_.size()) return;
    BLOG_INFO("places", "remove \"%s\" %s", places_[index].label.c_str(), places_[index].uri.c_str());
    places_.erase(places_.begin() + index);
    changed();
}

void Places::update(int index, const Place &p)
{
    if (index < 0 || index >= (int)places_.size()) return;
    BLOG_INFO("places", "edit \"%s\" -> \"%s\" %s", places_[index].label.c_str(), p.label.c_str(), p.uri.c_str());
    places_[index] = p;
    changed();
}

void Places::move(int from, int to)
{
    if (from < 0 || from >= (int)places_.size() || to < 0 || to > (int)places_.size() || from == to) return;
    Place p = places_[from];
    places_.erase(places_.begin() + from);
    if (to > from) --to;
    places_.insert(places_.begin() + to, p);
    changed();
}

void Places::setHidden(int index, bool hidden)
{
    if (index < 0 || index >= (int)places_.size()) return;
    places_[index].hidden = hidden;
    changed();
}

static std::string norm(std::string u)
{
    while (u.size() > 1 && u.back() == '/' && !ends_with(u, ":///") && !ends_with(u, "://")) u.pop_back();
    return u;
}

int Places::indexOf(const std::string &uri) const
{
    std::string n = norm(uri);
    for (size_t i = 0; i < places_.size(); ++i)
        if (norm(places_[i].uri) == n) return (int)i;
    return -1;
}

int Places::bestMatch(const std::string &uri) const
{
    std::string n = norm(uri);
    int best = -1;
    size_t bestLen = 0;
    for (size_t i = 0; i < places_.size(); ++i) {
        std::string p = norm(places_[i].uri);
        if (is_search_uri(p) || is_recent_uri(p)) {
            if (p == n) return (int)i;
            continue;
        }
        bool match = n == p || (starts_with(n, p) && (ends_with(p, "/") || n[p.size()] == '/'));
        if (match && p.size() > bestLen) {
            best = (int)i;
            bestLen = p.size();
        }
    }
    return best;
}

int Places::addListener(std::function<void()> l)
{
    static int next = 1;
    listeners_.emplace_back(next, std::move(l));
    return next++;
}

void Places::removeListener(int id)
{
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->first == id) {
            listeners_.erase(it);
            return;
        }
}

} // namespace beluga
