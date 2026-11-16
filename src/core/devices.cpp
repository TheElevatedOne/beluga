// SPDX-License-Identifier: GPL-2.0-or-later
#include "devices.hpp"

#include "util.hpp"

#include <gio/gunixmounts.h>

#include <algorithm>
#include <cstring>

namespace beluga {

namespace {
const char *kBus = "org.freedesktop.UDisks2";
const char *kBlock = "org.freedesktop.UDisks2.Block";
const char *kFs = "org.freedesktop.UDisks2.Filesystem";
const char *kDrive = "org.freedesktop.UDisks2.Drive";

std::string prop_str(GDBusProxy *p, const char *name)
{
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    if (!v) return {};
    std::string r;
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
        r = g_variant_get_string(v, nullptr);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING))
        r = g_variant_get_bytestring(v);
    g_variant_unref(v);
    return r;
}

bool prop_bool(GDBusProxy *p, const char *name)
{
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    if (!v) return false;
    bool r = g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
    g_variant_unref(v);
    return r;
}

guint64 prop_u64(GDBusProxy *p, const char *name)
{
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    if (!v) return 0;
    guint64 r = g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64) ? g_variant_get_uint64(v) : 0;
    g_variant_unref(v);
    return r;
}

std::string first_mount_point(GDBusProxy *fs)
{
    GVariant *v = g_dbus_proxy_get_cached_property(fs, "MountPoints");
    if (!v) return {};
    std::string r;
    if (g_variant_n_children(v) > 0) {
        GVariant *c = g_variant_get_child_value(v, 0);
        r = g_variant_get_bytestring(c);
        g_variant_unref(c);
    }
    g_variant_unref(v);
    return r;
}

struct CallData {
    DeviceManager::Done done;
};

void call_done(GObject *src, GAsyncResult *res, gpointer data)
{
    auto *cd = static_cast<CallData *>(data);
    GError *err = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    if (r) {
        std::string result;
        if (g_variant_is_of_type(r, G_VARIANT_TYPE("(s)"))) {
            const char *s = nullptr;
            g_variant_get(r, "(&s)", &s);
            result = s ? s : "";
        }
        g_variant_unref(r);
        if (cd->done) cd->done(true, result);
    } else {
        g_dbus_error_strip_remote_error(err);
        if (cd->done) cd->done(false, err->message);
        g_error_free(err);
    }
    delete cd;
}

void proxy_call(GDBusProxy *p, const char *method, DeviceManager::Done done)
{
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_dbus_proxy_call(p, method, g_variant_new("(a{sv})", &b), G_DBUS_CALL_FLAGS_NONE, G_MAXINT, nullptr, call_done,
                      new CallData{std::move(done)});
}
} // namespace

DeviceManager &DeviceManager::instance()
{
    static DeviceManager m;
    return m;
}

void DeviceManager::init()
{
    if (initialised_) return;
    initialised_ = true;
    fallbackMounts();
    g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, kBus,
                                             "/org/freedesktop/UDisks2", nullptr, nullptr, nullptr, nullptr,
                                             managerReady, this);
}

void DeviceManager::managerReady(GObject *, GAsyncResult *res, gpointer data)
{
    auto *self = static_cast<DeviceManager *>(data);
    GError *err = nullptr;
    self->mgr_ = g_dbus_object_manager_client_new_for_bus_finish(res, &err);
    if (!self->mgr_) {
        g_error_free(err);
        return;
    }
    // Swapped: the user data comes first, so one handler fits every signal signature.
    auto changed = G_CALLBACK(+[](gpointer d) { static_cast<DeviceManager *>(d)->scheduleRebuild(); });
    for (const char *sig : {"object-added", "object-removed", "interface-added", "interface-removed",
                            "interface-proxy-properties-changed"})
        g_signal_connect_swapped(self->mgr_, sig, changed, self);
    self->rebuild();
}

void DeviceManager::scheduleRebuild()
{
    if (rebuildSource_) return;
    rebuildSource_ = g_timeout_add(150, [](gpointer d) -> gboolean {
        auto *self = static_cast<DeviceManager *>(d);
        self->rebuildSource_ = 0;
        self->rebuild();
        return G_SOURCE_REMOVE;
    }, this);
}

GDBusProxy *DeviceManager::iface(const std::string &objectPath, const char *name) const
{
    if (!mgr_ || objectPath.empty() || objectPath == "/") return nullptr;
    GDBusInterface *i = g_dbus_object_manager_get_interface(mgr_, objectPath.c_str(), name);
    return i ? G_DBUS_PROXY(i) : nullptr;   // caller unrefs
}

void DeviceManager::fallbackMounts()
{
    devices_.clear();
    GList *mounts = g_unix_mount_entries_get(nullptr);
    for (GList *l = mounts; l; l = l->next) {
        auto *m = static_cast<GUnixMountEntry *>(l->data);
        std::string path = g_unix_mount_entry_get_mount_path(m);
        if (path == "/" || g_unix_mount_entry_guess_should_display(m)) {
            Device d;
            d.id = "mount:" + path;
            d.mountPath = path;
            d.devicePath = g_unix_mount_entry_get_device_path(m);
            d.fsType = g_unix_mount_entry_get_fs_type(m);
            d.label = path == "/" ? std::string(_("Root")) : take(g_unix_mount_entry_guess_name(m));
            d.icon = path == "/" ? "drive-harddisk" : "drive-removable-media";
            d.canEject = g_unix_mount_entry_guess_can_eject(m);
            devices_.push_back(d);
        }
        g_unix_mount_entry_free(m);
    }
    g_list_free(mounts);
}

void DeviceManager::rebuild()
{
    if (!mgr_) return;
    std::vector<Device> out;
    GList *objects = g_dbus_object_manager_get_objects(mgr_);
    for (GList *o = objects; o; o = o->next) {
        GDBusObject *obj = G_DBUS_OBJECT(o->data);
        GDBusInterface *bi = g_dbus_object_get_interface(obj, kBlock);
        GDBusInterface *fi = g_dbus_object_get_interface(obj, kFs);
        if (bi && fi) {
            GDBusProxy *block = G_DBUS_PROXY(bi), *fs = G_DBUS_PROXY(fi);
            if (!prop_bool(block, "HintIgnore") && prop_str(block, "IdUsage") == "filesystem") {
                Device d;
                d.id = g_dbus_object_get_object_path(obj);
                d.devicePath = prop_str(block, "Device");
                d.fsType = prop_str(block, "IdType");
                d.size = prop_u64(block, "Size");
                d.mountPath = first_mount_point(fs);
                d.drive = prop_str(block, "Drive");
                bool system = prop_bool(block, "HintSystem");
                std::string label = prop_str(block, "IdLabel");
                if (label.empty()) label = prop_str(block, "HintName");
                d.icon = prop_str(block, "HintIconName");
                if (GDBusProxy *drive = iface(d.drive, kDrive)) {
                    d.removable = prop_bool(drive, "Removable") || prop_bool(drive, "MediaRemovable");
                    d.canEject = prop_bool(drive, "Ejectable");
                    d.canPowerOff = prop_bool(drive, "CanPowerOff");
                    d.optical = prop_bool(drive, "Optical");
                    if (prop_str(drive, "ConnectionBus") == "usb") d.removable = true;
                    g_object_unref(drive);
                }
                if (d.icon.empty())
                    d.icon = d.optical ? "media-optical" : d.removable ? "drive-removable-media" : "drive-harddisk";
                if (label.empty()) {
                    if (d.mountPath == "/") label = _("Root");
                    else label = printf_str(_("%s Volume"), format_size(d.size).c_str());
                }
                d.label = label;
                bool boot = starts_with(d.mountPath, "/boot") || d.mountPath == "/efi";
                bool efi = d.mountPath.empty() && system && d.fsType == "vfat" && d.size < (1ull << 30);
                if (!boot && !efi) out.push_back(d);
            }
        }
        if (bi) g_object_unref(bi);
        if (fi) g_object_unref(fi);
    }
    g_list_free_full(objects, g_object_unref);
    std::stable_sort(out.begin(), out.end(), [](const Device &a, const Device &b) {
        if ((a.mountPath == "/") != (b.mountPath == "/")) return a.mountPath == "/";
        if (a.removable != b.removable) return !a.removable;
        return a.devicePath < b.devicePath;
    });
    devices_ = out;
    auto copy = listeners_;
    for (auto &[id, l] : copy) l();
}

const Device *DeviceManager::find(const std::string &id) const
{
    for (auto &d : devices_)
        if (d.id == id) return &d;
    return nullptr;
}

const Device *DeviceManager::forMountPath(const std::string &path) const
{
    for (auto &d : devices_)
        if (!d.mountPath.empty() && d.mountPath == path) return &d;
    return nullptr;
}

void DeviceManager::mount(const std::string &id, Done done)
{
    GDBusProxy *fs = iface(id, kFs);
    if (!fs) {
        if (done) done(false, _("This device cannot be mounted."));
        return;
    }
    proxy_call(fs, "Mount", std::move(done));
    g_object_unref(fs);
}

void DeviceManager::unmount(const std::string &id, Done done)
{
    GDBusProxy *fs = iface(id, kFs);
    if (!fs) {
        if (done) done(false, _("This device cannot be unmounted."));
        return;
    }
    proxy_call(fs, "Unmount", std::move(done));
    g_object_unref(fs);
}

void DeviceManager::eject(const std::string &id, Done done)
{
    const Device *d = find(id);
    if (!d) {
        if (done) done(false, _("The device is gone."));
        return;
    }
    std::string drivePath = d->drive;
    bool canEject = d->canEject, canPowerOff = d->canPowerOff;
    auto afterUnmount = [this, drivePath, canEject, canPowerOff, done](bool ok, const std::string &err) {
        if (!ok) {
            if (done) done(false, err);
            return;
        }
        GDBusProxy *drive = iface(drivePath, kDrive);
        if (!drive || (!canEject && !canPowerOff)) {
            if (drive) g_object_unref(drive);
            if (done) done(true, {});
            return;
        }
        proxy_call(drive, canEject ? "Eject" : "PowerOff", [this, drivePath, canEject, canPowerOff, done](bool ok2, const std::string &e2) {
            // Some USB sticks refuse Eject but can be powered off.
            if (!ok2 && canEject && canPowerOff) {
                if (GDBusProxy *dr = iface(drivePath, kDrive)) {
                    proxy_call(dr, "PowerOff", done);
                    g_object_unref(dr);
                    return;
                }
            }
            if (done) done(ok2, e2);
        });
        g_object_unref(drive);
    };
    if (d->mountPath.empty()) afterUnmount(true, {});
    else unmount(id, afterUnmount);
}

int DeviceManager::addListener(std::function<void()> l)
{
    static int next = 1;
    listeners_.emplace_back(next, std::move(l));
    return next++;
}

void DeviceManager::removeListener(int id)
{
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it)
        if (it->first == id) {
            listeners_.erase(it);
            return;
        }
}

} // namespace beluga
