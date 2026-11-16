// SPDX-License-Identifier: GPL-2.0-or-later
#include "terminal.hpp"

#include "settings.hpp"
#include "util.hpp"

namespace beluga::terminal {

static const char *kKnown[] = {
    "xdg-terminal-exec", "kitty", "foot", "alacritty", "wezterm", "ghostty", "konsole", "gnome-terminal",
    "kgx", "ptyxis", "xfce4-terminal", "tilix", "terminator", "lxterminal", "mate-terminal", "qterminal",
    "st", "urxvt", "xterm", nullptr,
};

static bool in_path(const std::string &prog)
{
    char *p = g_find_program_in_path(prog.c_str());
    bool ok = p != nullptr;
    g_free(p);
    return ok;
}

std::vector<std::string> installed()
{
    std::vector<std::string> v;
    for (int i = 0; kKnown[i]; ++i)
        if (in_path(kKnown[i])) v.push_back(kKnown[i]);
    return v;
}

std::string detect()
{
    const char *env = g_getenv("TERMINAL");
    if (env && *env) {
        int argc = 0;
        char **argv = nullptr;
        if (g_shell_parse_argv(env, &argc, &argv, nullptr)) {
            bool ok = argc > 0 && in_path(argv[0]);
            g_strfreev(argv);
            if (ok) return env;
        }
    }
    for (int i = 0; kKnown[i]; ++i)
        if (in_path(kKnown[i])) return kKnown[i];
    return {};
}

std::string effective_command()
{
    std::string c = trim(settings().terminalCommand);
    return c.empty() ? detect() : c;
}

bool open(const std::string &folder, GError **err)
{
    std::string cmd = effective_command();
    if (cmd.empty()) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    _("No terminal emulator was found. Set one in Configure Beluga › General › Terminal."));
        return false;
    }
    std::string dir = folder.empty() ? home_dir() : folder;
    if (cmd.find("%d") != std::string::npos) cmd = replace_all(cmd, "%d", shell_quote(dir));
    return spawn_command(cmd, dir, err);
}

} // namespace beluga::terminal
