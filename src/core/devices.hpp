// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: drives and partitions for the Places panel, read from UDisks2 over
// D-Bus (what KDE's Solid did). Mount, unmount and safely remove go through UDisks2 too.
#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>
#include <vector>

namespace beluga {

struct Device {
    std::string id;             // UDisks2 block object path, or "mount:<path>"
    std::string label;
    std::string icon;
    std::string devicePath;
    std::string mountPath;      // "" when not mounted
    std::string fsType;
    guint64 size = 0;
    bool removable = false;
    bool canEject = false;
    bool canPowerOff = false;
    bool optical = false;
    std::string drive;          // drive object path
};

class DeviceManager {
public:
    static DeviceManager &instance();
    void init();
    const std::vector<Device> &devices() const { return devices_; }
    const Device *find(const std::string &id) const;
    const Device *forMountPath(const std::string &path) const;

    // ok + mount path, or !ok + error message. Called on the main thread.
    using Done = std::function<void(bool ok, const std::string &result)>;
    void mount(const std::string &id, Done done);
    void unmount(const std::string &id, Done done);
    void eject(const std::string &id, Done done);   // unmounts, then ejects or powers off

    int addListener(std::function<void()> l);
    void removeListener(int id);

private:
    DeviceManager() = default;
    void rebuild();
    void scheduleRebuild();
    void fallbackMounts();
    GDBusProxy *iface(const std::string &objectPath, const char *iface) const;
    static void managerReady(GObject *, GAsyncResult *res, gpointer self);
    GDBusObjectManager *mgr_ = nullptr;
    std::vector<Device> devices_;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
    guint rebuildSource_ = 0;
    bool initialised_ = false;
};

} // namespace beluga
