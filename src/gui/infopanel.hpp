// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the Information panel: preview and details of the hovered or
// selected items, or of the current folder.
#pragma once

#include "item.hpp"

#include <memory>
#include <string>
#include <vector>

namespace beluga {

class InfoPanel {
public:
    InfoPanel();
    ~InfoPanel();
    GtkWidget *widget() const { return root_; }
    void showFolder(const std::string &uri);
    void showItems(const std::vector<BelugaItem *> &items);
    void showHovered(BelugaItem *item);   // nullptr: back to the selection

private:
    void render();
    void queueRender();
    void addRow(const std::string &key, const std::string &value);
    void setPreview(GFile *file, GFileInfo *info, const std::string &mime, gint64 mtime, guint64 size);
    GtkWidget *root_, *preview_, *media_, *name_, *grid_;
    std::vector<Ref<GFile>> files_;
    Ref<GFile> folder_;
    std::string virtualTitle_;   // search results, trash, recent files
    Ref<GFile> hovered_;
    int rows_ = 0;
    std::shared_ptr<int> alive_;
    guint renderSource_ = 0;
};

} // namespace beluga
