// SPDX-License-Identifier: GPL-2.0-or-later
#include "item.hpp"

#include "settings.hpp"

#include <sys/stat.h>

struct _BelugaItem {
    GObject parent;
    beluga::ItemData *d;
};

G_DEFINE_FINAL_TYPE(BelugaItem, beluga_item, G_TYPE_OBJECT)

static guint s_changedSignal = 0;

static void beluga_item_finalize(GObject *obj)
{
    delete BELUGA_ITEM(obj)->d;
    G_OBJECT_CLASS(beluga_item_parent_class)->finalize(obj);
}

static void beluga_item_class_init(BelugaItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = beluga_item_finalize;
    s_changedSignal = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}

static void beluga_item_init(BelugaItem *self) { self->d = new beluga::ItemData; }

namespace beluga {

static gint64 info_time(GFileInfo *info, const char *attr)
{
    if (!g_file_info_has_attribute(info, attr)) return 0;
    return (gint64)g_file_info_get_attribute_uint64(info, attr);
}

void item_set_info(BelugaItem *item, GFileInfo *info)
{
    ItemData &d = *item->d;
    d.info = Ref<GFileInfo>::share(info);
    const char *dn = g_file_info_get_display_name(info);
    d.name = dn ? dn : str(g_file_info_get_name(info));
    d.isDir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    d.isHidden = g_file_info_get_is_hidden(info);
    d.isBackup = g_file_info_get_is_backup(info) || ends_with(d.name, "~");
    d.isLink = g_file_info_get_is_symlink(info);
    d.mime = content_type_of(info);
    d.brokenLink = d.isLink && g_file_info_get_file_type(info) == G_FILE_TYPE_SYMBOLIC_LINK;
    d.size = d.isDir ? 0 : (guint64)g_file_info_get_size(info);
    d.mtime = info_time(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    d.ctime = info_time(info, G_FILE_ATTRIBUTE_TIME_CREATED);
    d.atime = info_time(info, G_FILE_ATTRIBUTE_TIME_ACCESS);
    d.readable = !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ)
              || g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
    d.writable = !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE)
              || g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE);
    GIcon *icon = g_file_info_get_icon(info);
    d.icon = Ref<GIcon>::share(icon);
    if (!icon) d.icon = Ref<GIcon>(g_themed_icon_new(d.isDir ? "folder" : "text-x-generic"));
    d.typeText.clear();
    d.dimsKnown = false;
    d.nameWidth = -1;
    item_refresh_collation(item);
}

void item_refresh_collation(BelugaItem *item)
{
    item->d->key = collate_key(item->d->name, settings().sortingChoice);
}

BelugaItem *item_new(GFile *file, GFileInfo *info)
{
    auto *item = BELUGA_ITEM(g_object_new(BELUGA_TYPE_ITEM, nullptr));
    item->d->file = Ref<GFile>::share(file);
    item->d->uri = file_uri(file);
    item_set_info(item, info);
    return item;
}

ItemData &item_data(BelugaItem *item) { return *item->d; }

void item_emit_changed(BelugaItem *item) { g_signal_emit(item, s_changedSignal, 0); }

BelugaItem *item_from_object(gpointer obj)
{
    if (!obj) return nullptr;
    if (GTK_IS_TREE_LIST_ROW(obj)) {
        gpointer inner = gtk_tree_list_row_get_item(GTK_TREE_LIST_ROW(obj));
        BelugaItem *it = BELUGA_IS_ITEM(inner) ? BELUGA_ITEM(inner) : nullptr;
        if (inner) g_object_unref(inner);
        return it;
    }
    return BELUGA_IS_ITEM(obj) ? BELUGA_ITEM(obj) : nullptr;
}

BelugaItem *item_at(GListModel *model, guint pos)
{
    gpointer obj = g_list_model_get_item(model, pos);
    if (!obj) return nullptr;
    BelugaItem *it = item_from_object(obj);
    g_object_unref(obj);   // the model keeps its own reference
    return it;
}

const std::string &item_type_text(BelugaItem *item)
{
    ItemData &d = *item->d;
    if (d.typeText.empty()) {
        if (d.isDir) d.typeText = _("Folder");
        else if (d.brokenLink) d.typeText = _("Broken Link");
        else d.typeText = take(g_content_type_get_description(d.mime.c_str()));
        if (d.isLink && !d.brokenLink) d.typeText = printf_str(_("%s (link)"), d.typeText.c_str());
    }
    return d.typeText;
}

std::string item_size_text(BelugaItem *item)
{
    ItemData &d = *item->d;
    if (!d.isDir) return format_size(d.size);
    const Settings &s = settings();
    if (s.dirSizeMode == DirSizeMode::None) return {};
    if (d.dirCount == -1) return _("Unknown");
    if (d.dirCount < 0) return {};
    if (s.dirSizeMode == DirSizeMode::Size) return format_size(d.dirSize);
    return format_items(d.dirCount);
}

gint64 item_usage(BelugaItem *item)
{
    ItemData &d = *item->d;
    if (d.isDir) return d.usage >= 0 ? d.usage : -1;
    GFileInfo *info = d.info.get();
    if (info && g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE))
        return (gint64)g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_STANDARD_ALLOCATED_SIZE);
    return (gint64)d.size;
}

static std::string date_text(gint64 t)
{
    return format_date(t, settings().useShortRelativeDates ? DateStyle::Relative : DateStyle::Short);
}

std::string item_role_text(BelugaItem *item, const std::string &role)
{
    ItemData &d = *item->d;
    GFileInfo *info = d.info.get();
    if (role == "text") return d.name;
    if (role == "size") return item_size_text(item);
    if (role == "diskusage") {
        gint64 u = item_usage(item);
        if (u >= 0) return format_size((guint64)u);
        return d.isDir && d.usagePending ? std::string("…") : std::string();
    }
    if (role == "modificationtime") return date_text(d.mtime);
    if (role == "creationtime") return date_text(d.ctime);
    if (role == "accesstime") return date_text(d.atime);
    if (role == "type") return item_type_text(item);
    if (role == "extension") {
        auto ext = split_extension(d.name, d.isDir).second;
        return ext.empty() ? std::string() : ext.substr(1);
    }
    if (role == "permissions") {
        if (!g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE)) return {};
        return format_permissions(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE), settings().permissionsFormat);
    }
    if (role == "owner") return str(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER));
    if (role == "group") return str(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP));
    if (role == "path") {
        if (!d.parentPath.empty()) return d.parentPath;
        Ref<GFile> parent(g_file_get_parent(d.file.get()));
        return parent ? display_location(parent.get()) : std::string();
    }
    if (role == "destination") return d.isLink ? str(g_file_info_get_symlink_target(info)) : std::string();
    if (role == "deletiontime") return date_text(d.deletionTime);
    if (role == "originalpath") return d.originalPath.empty() ? std::string() : dirname_of(d.originalPath);
    if (role == "dimensions") {
        if (!starts_with(d.mime, "image/")) return {};
        if (!d.dimsKnown) {
            d.dimsKnown = true;
            char *path = g_file_get_path(d.file.get());
            int w = 0, h = 0;
            if (path) gdk_pixbuf_get_file_info(path, &w, &h);
            g_free(path);
            if (w > 0) d.dims = printf_str("%d × %d", w, h);
        }
        return d.dims;
    }
    return {};
}

} // namespace beluga
