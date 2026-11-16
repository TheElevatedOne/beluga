// SPDX-License-Identifier: GPL-2.0-or-later
#include "customactions.hpp"

#include "terminal.hpp"
#include "util.hpp"

namespace beluga {

static std::string store_path() { return config_dir() + "/actions.conf"; }

std::vector<CustomAction> default_custom_actions()
{
    std::vector<CustomAction> v;
    char *bsdtar = g_find_program_in_path("bsdtar");
    if (bsdtar) {
        v.push_back({_("Extract Here"), "archive-extract", "bsdtar -xf %f -C %d",
                     "*.zip;*.tar;*.tar.*;*.tgz;*.tbz2;*.txz;*.7z;*.rar;*.cpio;*.iso;*.xz;*.gz;*.bz2;*.zst", "",
                     true, false, false, false});
        g_free(bsdtar);
    }
    v.push_back({_("Compress to .tar.gz"), "archive-insert", "tar -czf \"$(basename %f).tar.gz\" -- %N", "", "", true, true, true, false});
    return v;
}

std::vector<CustomAction> load_custom_actions()
{
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, store_path().c_str(), G_KEY_FILE_NONE, nullptr)) {
        g_key_file_free(kf);
        auto d = default_custom_actions();
        save_custom_actions(d);
        return d;
    }
    std::vector<CustomAction> v;
    gsize n = 0;
    char **groups = g_key_file_get_groups(kf, &n);
    for (gsize i = 0; i < n; ++i) {
        const char *g = groups[i];
        if (!starts_with(g, "Action ")) continue;
        CustomAction a;
        a.name = take(g_key_file_get_string(kf, g, "Name", nullptr));
        a.icon = take(g_key_file_get_string(kf, g, "Icon", nullptr));
        a.command = take(g_key_file_get_string(kf, g, "Command", nullptr));
        a.patterns = take(g_key_file_get_string(kf, g, "Patterns", nullptr));
        a.mimeTypes = take(g_key_file_get_string(kf, g, "MimeTypes", nullptr));
        a.forFiles = g_key_file_get_boolean(kf, g, "Files", nullptr);
        a.forFolders = g_key_file_get_boolean(kf, g, "Folders", nullptr);
        a.multiple = g_key_file_get_boolean(kf, g, "Multiple", nullptr);
        a.runInTerminal = g_key_file_get_boolean(kf, g, "Terminal", nullptr);
        if (!a.name.empty() && !a.command.empty()) v.push_back(a);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    return v;
}

void save_custom_actions(const std::vector<CustomAction> &actions)
{
    GKeyFile *kf = g_key_file_new();
    for (size_t i = 0; i < actions.size(); ++i) {
        const CustomAction &a = actions[i];
        std::string g = printf_str("Action %03zu", i);
        const char *G = g.c_str();
        g_key_file_set_string(kf, G, "Name", a.name.c_str());
        g_key_file_set_string(kf, G, "Icon", a.icon.c_str());
        g_key_file_set_string(kf, G, "Command", a.command.c_str());
        g_key_file_set_string(kf, G, "Patterns", a.patterns.c_str());
        g_key_file_set_string(kf, G, "MimeTypes", a.mimeTypes.c_str());
        g_key_file_set_boolean(kf, G, "Files", a.forFiles);
        g_key_file_set_boolean(kf, G, "Folders", a.forFolders);
        g_key_file_set_boolean(kf, G, "Multiple", a.multiple);
        g_key_file_set_boolean(kf, G, "Terminal", a.runInTerminal);
    }
    g_key_file_save_to_file(kf, store_path().c_str(), nullptr);
    g_key_file_free(kf);
}

bool action_applies(const CustomAction &a, const std::vector<ActionTarget> &targets)
{
    if (targets.empty()) return false;
    if (targets.size() > 1 && !a.multiple) return false;
    auto patterns = split(a.patterns, ';');
    auto mimes = split(a.mimeTypes, ';');
    for (auto &t : targets) {
        if (t.isDir ? !a.forFolders : !a.forFiles) return false;
        if (!patterns.empty()) {
            bool any = false;
            for (auto &p : patterns)
                if (filter_match(trim(p), t.name)) any = true;
            if (!any) return false;
        }
        if (!mimes.empty()) {
            bool any = false;
            for (auto &m : mimes) {
                std::string mm = trim(m);
                if (mm == t.mime || (ends_with(mm, "/*") && starts_with(t.mime, mm.substr(0, mm.size() - 1)))
                    || g_content_type_is_a(t.mime.c_str(), mm.c_str()))
                    any = true;
            }
            if (!any) return false;
        }
    }
    return true;
}

std::string expand_action_command(const CustomAction &a, const std::vector<ActionTarget> &targets, const std::string &folder)
{
    auto list = [&](auto get) {
        std::vector<std::string> v;
        for (auto &t : targets) v.push_back(shell_quote(get(t)));
        return join(v, " ");
    };
    auto path = [](const ActionTarget &t) { return t.path.empty() ? t.uri : t.path; };
    std::string out;
    const std::string &c = a.command;
    for (size_t i = 0; i < c.size(); ++i) {
        if (c[i] != '%' || i + 1 >= c.size()) {
            out += c[i];
            continue;
        }
        char k = c[++i];
        switch (k) {
        case 'f': out += targets.empty() ? "" : shell_quote(path(targets[0])); break;
        case 'F': out += list(path); break;
        case 'u': out += targets.empty() ? "" : shell_quote(targets[0].uri); break;
        case 'U': out += list([](const ActionTarget &t) { return t.uri; }); break;
        case 'n': out += targets.empty() ? "" : shell_quote(targets[0].name); break;
        case 'N': out += list([](const ActionTarget &t) { return t.name; }); break;
        case 'd': out += shell_quote(folder); break;
        case '%': out += '%'; break;
        default: out += '%'; out += k;
        }
    }
    return out;
}

bool run_custom_action(const CustomAction &a, const std::vector<ActionTarget> &targets, const std::string &folder, GError **err)
{
    std::string cmd = expand_action_command(a, targets, folder);
    std::string full;
    if (a.runInTerminal) {
        std::string term = terminal::effective_command();
        if (term.empty()) {
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No terminal emulator was found."));
            return false;
        }
        full = term + " -e sh -c " + shell_quote(cmd + "; echo; read -r -p 'Press Enter to close' _");
    } else {
        full = "sh -c " + shell_quote(cmd);
    }
    return spawn_command(full, folder, err);
}

} // namespace beluga
