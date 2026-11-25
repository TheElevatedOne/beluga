// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: one file or folder in a view. A GObject so it can live in GTK list
// models; the data is a C++ struct. "changed" is emitted when the preview, folder
// size or file info was updated.
#pragma once

#include "util.hpp"

#include <gtk/gtk.h>

#include <string>

G_BEGIN_DECLS
#define BELUGA_TYPE_ITEM (beluga_item_get_type())
G_DECLARE_FINAL_TYPE(BelugaItem, beluga_item, BELUGA, ITEM, GObject)
G_END_DECLS

namespace beluga {

struct ItemData {
    Ref<GFile> file;
    Ref<GFileInfo> info;
    std::string uri;
    std::string name;          // display name
    std::string key;           // collation key for sorting
    std::string mime;
    bool isDir = false;
    bool isHidden = false;
    bool isLink = false;
    bool isBackup = false;
    bool brokenLink = false;
    bool readable = true;
    bool writable = true;
    guint64 size = 0;
    gint64 mtime = 0, ctime = 0, atime = 0;
    Ref<GIcon> icon;

    Ref<GdkPaintable> thumb;   // preview
    int thumbPixels = 0;
    bool thumbPending = false;
    bool thumbFailed = false;

    int dirCount = -2;         // -2 unknown, -1 unreadable
    guint64 dirSize = 0;
    bool dirPending = false;
    gint64 usage = -2;         // folders: space on disk of everything inside; -2 not asked, -1 unknown
    bool usagePending = false;

    std::string originalPath;  // trash
    gint64 deletionTime = 0;   // trash
    std::string parentPath;    // search / recent results
    bool cut = false;
    int nameWidth = -1;        // cached pixel width (compact mode)
    std::string typeText;      // lazy
    std::string dims;          // lazy image dimensions
    bool dimsKnown = false;
};

BelugaItem *item_new(GFile *file, GFileInfo *info);
ItemData &item_data(BelugaItem *item);
void item_set_info(BelugaItem *item, GFileInfo *info);
void item_emit_changed(BelugaItem *item);
// BelugaItem from a model object (unwraps GtkTreeListRow).
BelugaItem *item_from_object(gpointer obj);
BelugaItem *item_at(GListModel *model, guint pos);   // borrowed

const std::string &item_type_text(BelugaItem *item);
std::string item_role_text(BelugaItem *item, const std::string &role);
std::string item_size_text(BelugaItem *item);
// Space on disk: a file's allocated size, a folder's whole contents once read; -1 when not known.
gint64 item_usage(BelugaItem *item);
void item_refresh_collation(BelugaItem *item);

} // namespace beluga
