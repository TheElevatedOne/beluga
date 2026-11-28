// SPDX-License-Identifier: GPL-2.0-or-later
#include "netlocation.hpp"

#include "util.hpp"

#include <glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>

#include <cerrno>
#include <cstdlib>

namespace beluga {

const std::vector<NetProtocol> &net_protocols()
{
    static const std::vector<NetProtocol> v = {
        {"sftp", "SFTP (SSH)", 22, false, true},
        {"ftp", "FTP", 21, true, false},
        {"ftps", N_("FTP over TLS (explicit)"), 21, true, false},
        {"ftpis", N_("FTP over TLS (implicit)"), 990, true, false},
        {"smb", N_("Windows share (SMB)"), 0, true, false},
        {"dav", "WebDAV", 80, true, false},
        {"davs", N_("Secure WebDAV"), 443, true, false},
    };
    return v;
}

int net_protocol_index(const std::string &scheme)
{
    const auto &v = net_protocols();
    for (size_t i = 0; i < v.size(); ++i)
        if (scheme == v[i].scheme) return (int)i;
    return -1;
}

namespace {

std::string build_uri(const char *scheme, const std::string &user, const std::string &host, int port, std::string folder)
{
    if (host.empty()) return {};
    if (folder.empty() || folder[0] != '/') folder = "/" + folder;
    char *uri = g_uri_join(G_URI_FLAGS_NONE, scheme, user.empty() ? nullptr : user.c_str(), host.c_str(),
                           port > 0 && port <= 65535 ? port : -1, folder.c_str(), nullptr, nullptr);
    std::string r = uri ? uri : "";
    g_free(uri);
    return r;
}

} // namespace

std::string net_location_uri(const NetLocation &n)
{
    const auto &v = net_protocols();
    if (n.protocol < 0 || n.protocol >= (int)v.size()) return {};
    const NetProtocol &p = v[n.protocol];
    // The default port isn't written, so the address stays short.
    int port = n.port == p.port ? 0 : n.port;
    std::string user = n.logon == Logon::Anonymous ? std::string() : n.user;
    return build_uri(p.scheme, user, n.host, port, n.folder);
}

bool net_location_from_uri(const std::string &uri, NetLocation &n)
{
    GUri *u = g_uri_parse(uri.c_str(), G_URI_FLAGS_NONE, nullptr);
    if (!u) return false;
    int i = g_uri_get_scheme(u) ? net_protocol_index(g_uri_get_scheme(u)) : -1;
    bool ok = i >= 0;
    if (ok) {
        n.protocol = i;
        n.host = g_uri_get_host(u) ? g_uri_get_host(u) : "";
        n.user = g_uri_get_user(u) ? g_uri_get_user(u) : "";
        n.port = g_uri_get_port(u) > 0 ? g_uri_get_port(u) : 0;
        std::string path = g_uri_get_path(u) ? g_uri_get_path(u) : "";
        n.folder = path == "/" ? std::string() : path;
    }
    g_uri_unref(u);
    return ok;
}

std::string ssh_alias_for(const NetLocation &n)
{
    std::string a = "beluga-";
    for (char c : (n.user.empty() ? n.host : n.user + "-" + n.host))
        a += (g_ascii_isalnum(c) || c == '.' || c == '-' || c == '_') ? c : '_';
    return a;
}

std::string net_location_alias_uri(const NetLocation &n)
{
    return build_uri("sftp", {}, ssh_alias_for(n), 0, n.folder);
}

bool write_ssh_alias(const std::string &configPathIn, const NetLocation &n, std::string *error)
{
    // A config kept elsewhere (dotfiles) and linked in: write the real file, keep the link.
    std::string configPath = configPathIn;
    if (char *real = realpath(configPathIn.c_str(), nullptr)) {
        configPath = real;
        free(real);
    }
    std::string alias = ssh_alias_for(n);
    std::string begin = "# Beluga: " + alias + " (begin)", end = "# Beluga: " + alias + " (end)";
    std::string text;
    gchar *old = nullptr;
    if (g_file_get_contents(configPath.c_str(), &old, nullptr, nullptr)) {
        text = old;
        g_free(old);
    }
    // Drop an earlier block for the same alias.
    size_t b = text.find(begin);
    if (b != std::string::npos) {
        size_t e = text.find(end, b);
        if (e != std::string::npos) {
            e += end.size();
            if (e < text.size() && text[e] == '\n') ++e;
            text.erase(b, e - b);
        }
    }
    if (!text.empty() && text.back() != '\n') text += '\n';
    if (!text.empty() && text.size() >= 2 && text.compare(text.size() - 2, 2, "\n\n") != 0) text += '\n';
    int port = n.port > 0 ? n.port : 22;
    text += begin + "\n";
    text += "Host " + alias + "\n";
    text += "    HostName " + n.host + "\n";
    if (!n.user.empty()) text += "    User " + n.user + "\n";
    text += "    Port " + std::to_string(port) + "\n";
    text += "    IdentityFile \"" + n.keyFile + "\"\n";
    text += "    IdentitiesOnly yes\n";
    text += end + "\n";

    std::string dir = dirname_of(configPath);
    if (g_mkdir_with_parents(dir.c_str(), 0700) != 0) {
        if (error) *error = g_strerror(errno);
        return false;
    }
    GError *err = nullptr;
    if (!g_file_set_contents_full(configPath.c_str(), text.c_str(), (gssize)text.size(), G_FILE_SET_CONTENTS_CONSISTENT, 0600, &err)) {
        if (error) *error = err->message;
        g_error_free(err);
        return false;
    }
    return true;
}

} // namespace beluga
