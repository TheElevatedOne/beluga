// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: DolphinView. One listed location with its view properties: the model
// pipeline (filter, tree, sort, groups, selection), previews, folder sizes, renaming
// and drops. The ItemView draws it.
#pragma once

#include "dirmodel.hpp"
#include "itemview.hpp"
#include "viewprops.hpp"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace beluga {

class View {
public:
    View();
    ~View();
    View(const View &) = delete;
    View &operator=(const View &) = delete;

    GtkWidget *widget() const { return overlay_; }
    GtkWidget *overlay() const { return overlay_; }
    ItemView *itemView() const { return iv_; }

    void setLocation(const std::string &uri);
    const std::string &location() const { return uri_; }
    Ref<GFile> locationFile() const;
    void reload();
    void stop();
    bool loading() const { return lister_.loading(); }
    bool isVirtual() const { return is_virtual_uri(uri_); }
    // The columns shown for the current mode: the folder's own, plus Disk Usage in the Details
    // view when that setting is on.
    std::vector<std::string> shownRoles() const;
    bool isTrash() const { return is_trash_uri(uri_); }
    bool isSearch() const { return is_search_uri(uri_); }

    // View properties.
    const ViewProps &props() const { return props_; }
    void setViewMode(int mode);
    void setZoomLevel(int level);
    int zoomLevel() const;
    void zoomIn() { setZoomLevel(zoomLevel() + 1); }
    void zoomOut() { setZoomLevel(zoomLevel() - 1); }
    void resetZoom();
    void setPreviewsShown(bool on);
    void setHiddenShown(bool on);
    void setSortRole(const std::string &role);
    void setSortDescending(bool on);
    void setFoldersFirst(bool on);
    void setHiddenLast(bool on);
    void setGrouped(bool on);
    void toggleRole(const std::string &role);
    void setRoles(const std::vector<std::string> &roles);
    void applyProps(const ViewProps &p, bool save);
    void restoreDefaultProps();
    // "Remember view for this folder": whether the folder has its own view settings instead of the
    // default ones. Turning it off returns the folder to the default settings.
    bool hasOwnProps() const { return ownProps_; }
    void setOwnProps(bool on);
    // Re-reads the settings for the current folder after they were stored elsewhere.
    void reloadProps();
    void settingsChanged();

    void setNameFilter(const std::string &f);
    const std::string &nameFilter() const { return nameFilter_; }

    // Items and selection.
    GListModel *visibleModel() const { return G_LIST_MODEL(sel_); }
    BelugaItem *itemAt(int pos) const;
    std::vector<BelugaItem *> selectedItems() const;
    std::vector<Ref<GFile>> selectedFiles() const;
    int selectionCount() const;
    BelugaItem *hoveredItem() const { return hovered_; }
    void selectAll();
    void invertSelection();
    void clearSelection();
    void selectUris(const std::vector<std::string> &uris);       // now or when they appear
    void setCutUris(const std::set<std::string> &uris);
    BelugaItem *findItem(const std::string &uri) const;
    void selectAndShow(const std::string &uri);
    void countItems(int &files, int &folders, guint64 &size) const;

    void renameSelected();
    // Plain name list, used for the columns of the Columns view.
    void setColumnMode(bool on) { columnMode_ = on; applyToItemView(); }
    bool listMode() const { return columnMode_ || props_.viewMode == ModeColumns; }
    void setSelectionMode(bool on);
    bool selectionMode() const { return selectionMode_; }
    void focus();

    // For history: scroll offset and the current item.
    double scrollPosition() const { return iv_->scrollPosition(); }
    void setScrollPosition(double v) { iv_->setScrollPosition(v); }

    // Callbacks for the container.
    std::function<void(const std::vector<BelugaItem *> &, GdkModifierType)> onActivate;
    std::function<void(BelugaItem *)> onMiddleClick;
    std::function<void(int pos, double x, double y)> onContextMenu;
    std::function<void()> onSelectionChanged;
    std::function<void()> onHoverChanged;
    std::function<void()> onPropsChanged;
    std::function<void()> onContentsChanged;      // items added/removed
    std::function<void(bool)> onLoading;
    std::function<void(const std::string &)> onError;
    std::function<void()> onLocationGone;
    std::function<void(GFile *dest, GdkFileList *files, GdkModifierType mods, double x, double y)> onDrop;
    std::function<void(const std::string &uri)> onSpringLoad;
    std::function<void(int button)> onExtraButton;
    std::function<void()> onEmptyDoubleClick;
    std::function<void()> onEscape;
    std::function<void()> onFocusIn;
    std::function<GMountOperation *()> mountOperation;

private:
    void buildPipeline();
    void applyToItemView();
    void updatePlaceholder();
    void requestPreview(BelugaItem *item);
    void requestDirSize(BelugaItem *item);
    void requestUsage(BelugaItem *item);
    // Recounts what the shown items take up on disk (the Disk Usage bars) and re-sorts by it, at
    // most every 200 ms while folder sizes come in.
    void scheduleUsageRefresh();
    guint usageSource_ = 0;
    void doRename(BelugaItem *item, const std::string &newName);
    void checkPendingSelection();
    std::string groupLabelFor(BelugaItem *item) const;
    static int compareItems(gconstpointer a, gconstpointer b, gpointer self);
    static int compareGroups(gconstpointer a, gconstpointer b, gpointer self);
    static gboolean filterItem(gpointer item, gpointer self);
    static GListModel *createChildModel(gpointer item, gpointer self);
    bool useTree() const;
    int compareRole(BelugaItem *a, BelugaItem *b, const std::string &role) const;

    GtkWidget *overlay_;
    GtkWidget *scrolled_;
    GtkWidget *placeholder_;
    GtkWidget *ivWidget_;
    ItemView *iv_;
    DirLister lister_;
    std::string uri_;
    ViewProps props_;
    bool ownProps_ = false;
    std::string nameFilter_;
    GtkFilter *filter_ = nullptr;
    GtkFilterListModel *filterModel_ = nullptr;
    GtkTreeListModel *treeModel_ = nullptr;
    GtkSortListModel *sortModel_ = nullptr;
    GtkSelectionModel *sel_ = nullptr;
    bool treeBuilt_ = false;
    bool selectionMode_ = false;
    bool columnMode_ = false;
    BelugaItem *hovered_ = nullptr;
    std::set<std::string> pendingSelect_;
    gint64 pendingSelectUntil_ = 0;
    std::set<std::string> cutUris_;
    std::shared_ptr<int> alive_;
    int settingsListener_ = 0;
};

} // namespace beluga
