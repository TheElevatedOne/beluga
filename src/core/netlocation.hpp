// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

namespace beluga {

// A server to connect to through gvfs, as the Add Network Location dialog fills it in.
enum class Logon { Anonymous, Normal, Ask, KeyFile };

struct NetProtocol {
    const char *scheme;
    const char *label;   // translatable (N_)
    int port;            // default port, 0 when the protocol has none
    bool anonymous;      // Anonymous logon makes sense
    bool keyFile;        // Key file logon (SFTP only)
};
const std::vector<NetProtocol> &net_protocols();
int net_protocol_index(const std::string &scheme);   // -1 when unknown

struct NetLocation {
    int protocol = 0;    // index into net_protocols()
    std::string host, user, folder, password, keyFile;
    int port = 0;        // 0: the protocol's default
    Logon logon = Logon::Ask;
};

// sftp://user@host:port/folder; empty without a host.
std::string net_location_uri(const NetLocation &n);
// Fills protocol, user, host, port and folder from an address; false when it isn't one of the protocols.
bool net_location_from_uri(const std::string &uri, NetLocation &n);

// Key files: gvfs runs ssh without a way to pass -i, so the key goes into an ssh Host alias.
std::string ssh_alias_for(const NetLocation &n);
// Adds or replaces Beluga's Host block for the alias in the ssh config file (created with mode 600).
bool write_ssh_alias(const std::string &configPath, const NetLocation &n, std::string *error);
// The address that uses the alias: sftp://alias/folder.
std::string net_location_alias_uri(const NetLocation &n);

} // namespace beluga
