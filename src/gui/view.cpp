// SPDX-License-Identifier: GPL-2.0-or-later
#include "view.hpp"

#include "dialogs.hpp"
#include "dirsize.hpp"
#include "gtkutil.hpp"
#include "settings.hpp"
#include "thumbnails.hpp"
#include "undo.hpp"

#include <algorithm>

namespace beluga {

View::View()
{
    alive_ = std::make_shared<int>(0);
    overlay_ = gtk_overlay_new();
    g_object_ref_sink(overlay_);
    gtk_widget_add_css_class(overlay_, "beluga-view-area");
    scrolled_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_, TRUE);
    gtk_widget_set_hexpand(scrolled_, TRUE);
    ivWidget_ = item_view_new();
    iv_ = item_view_get(ivWidget_);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_), ivWidget_);
    gtk_overlay_set_child(GTK_OVERLAY(overlay_), scrolled_);
    placeholder_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(placeholder_, "dim-label");
    gtk_widget_add_css_class(placeholder_, "beluga-placeholder");
    gtk_widget_set_halign(placeholder_, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(placeholder_, GTK_ALIGN_CENTER);
    gtk_widget_set_can_target(placeholder_, FALSE);
    gtk_widget_set_visible(placeholder_, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), placeholder_);

    iv_->onActivate = [this](int pos, GdkModifierType mods) {
        BelugaItem *item = itemAt(pos);
        if (!item || !onActivate) return;
        std::vector<BelugaItem *> items;
        if (gtk_selection_model_is_selected(sel_, pos) && selectionCount() > 1) items = selectedItems();
        else items.push_back(item);
        onActivate(items, mods);
    };
    iv_->onMiddleClick = [this](int pos) {
        if (BelugaItem *it = itemAt(pos))
            if (onMiddleClick) onMiddleClick(it);
    };
    iv_->onContextMenu = [this](int pos, double x, double y) {
        if (onContextMenu) onContextMenu(pos, x, y);
    };
    iv_->onHover = [this](int pos) {
        BelugaItem *it = pos >= 0 ? itemAt(pos) : nullptr;
        g_set_weak_pointer(&hovered_, it);
        if (onHoverChanged) onHoverChanged();
    };
    iv_->onHeaderClicked = [this](const std::string &role) {
        if (role == props_.sortRole) setSortDescending(!props_.sortDescending);
        else setSortRole(role);
    };
    iv_->onHeaderMenu = [this](double x, double y) {
        GtkWidget *pop = gtk_popover_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        GtkWidget *title = gtk_label_new(_("Show Columns"));
        gtk_widget_add_css_class(title, "heading");
        gtk_label_set_xalign(GTK_LABEL(title), 0);
        gtk_box_append(GTK_BOX(box), title);
        auto roles = props_.rolesForMode(props_.viewMode);
        for (auto &r : all_roles()) {
            std::string id = r.id;
            if (id == "text") continue;
            if ((id == "deletiontime" || id == "originalpath") && !isTrash()) continue;
            GtkWidget *cb = gtk_check_button_new_with_label(_(r.title));
            gtk_check_button_set_active(GTK_CHECK_BUTTON(cb), std::find(roles.begin(), roles.end(), id) != roles.end());
            g_object_set_data_full(G_OBJECT(cb), "role", g_strdup(r.id), g_free);
            g_signal_connect(cb, "toggled", CB([](GtkCheckButton *b, gpointer self) {
                static_cast<View *>(self)->toggleRole(static_cast<const char *>(g_object_get_data(G_OBJECT(b), "role")));
            }), this);
            gtk_box_append(GTK_BOX(box), cb);
        }
        gtk_popover_set_child(GTK_POPOVER(pop), box);
        gtk_widget_set_parent(pop, ivWidget_);
        GdkRectangle r = {(int)x, (int)y, 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
        gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
        g_signal_connect(pop, "closed", CB([](GtkPopover *p) {
            g_idle_add([](gpointer w) -> gboolean {
                if (gtk_widget_get_parent(GTK_WIDGET(w))) gtk_widget_unparent(GTK_WIDGET(w));
                g_object_unref(w);
                return G_SOURCE_REMOVE;
            }, g_object_ref(p));
        }), nullptr);
        gtk_popover_popup(GTK_POPOVER(pop));
    };
    iv_->onColumnsResized = [this] {
        props_.columnWidths = iv_->columnWidths();
        save_view_props(uri_, props_);
    };
    iv_->onZoom = [this](int steps) { setZoomLevel(zoomLevel() + steps); };
    iv_->onExtraButton = [this](int b) { if (onExtraButton) onExtraButton(b); };
    iv_->onEmptyDoubleClick = [this] { if (onEmptyDoubleClick) onEmptyDoubleClick(); };
    iv_->onEscape = [this] { if (onEscape) onEscape(); };
    iv_->onNeedPreview = [this](BelugaItem *it) { requestPreview(it); };
    iv_->onNeedDirSize = [this](BelugaItem *it) { requestDirSize(it); };
    iv_->groupLabel = [this](int pos) { BelugaItem *it = itemAt(pos); return it ? groupLabelFor(it) : std::string(); };
    iv_->onTooltip = [this](int pos, GtkTooltip *t) -> gboolean {
        if (!settings().showToolTips) return FALSE;
        BelugaItem *it = itemAt(pos);
        if (!it) return FALSE;
        ItemData &d = item_data(it);
        std::string m = "<b>" + markup_escape(d.name) + "</b>\n" + markup_escape(item_type_text(it));
        std::string size = item_size_text(it);
        if (!size.empty()) m += "\n" + printf_str(_("Size: %s"), markup_escape(size).c_str());
        m += "\n" + printf_str(_("Modified: %s"), markup_escape(format_date(d.mtime, DateStyle::Long)).c_str());
        if (!d.originalPath.empty()) m += "\n" + printf_str(_("Original location: %s"), markup_escape(d.originalPath).c_str());
        gtk_tooltip_set_markup(t, m.c_str());
        if (d.thumb) gtk_tooltip_set_icon(t, d.thumb.get());
        else gtk_tooltip_set_icon_from_gicon(t, d.icon.get());
        return TRUE;
    };
    iv_->canDropOn = [this](int pos) {
        BelugaItem *it = itemAt(pos);
        return it && item_data(it).isDir && !isTrash();
    };
    iv_->onDrop = [this](int pos, GdkFileList *files, GdkModifierType mods, double x, double y) {
        if (!onDrop) return;
        BelugaItem *it = pos >= 0 ? itemAt(pos) : nullptr;
        if (it) {
            onDrop(item_data(it).file.get(), files, mods, x, y);
            return;
        }
        if (is_search_uri(uri_) || is_recent_uri(uri_)) return;
        if (isTrash()) {
            onDrop(nullptr, files, mods, x, y);
            return;
        }
        Ref<GFile> dir = locationFile();
        onDrop(dir.get(), files, mods, x, y);
    };
    iv_->onSpringLoad = [this](int pos) {
        BelugaItem *it = itemAt(pos);
        if (it && item_data(it).isDir && onSpringLoad) onSpringLoad(item_data(it).uri);
    };
    iv_->dragContent = [this](const std::vector<int> &positions) -> GdkContentProvider * {
        std::vector<GFile *> files;
        for (int p : positions)
            if (BelugaItem *it = itemAt(p)) files.push_back(item_data(it).file.get());
        if (files.empty()) return nullptr;
        GdkFileList *list = gdk_file_list_new_from_array(files.data(), files.size());
        GdkContentProvider *p = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, list);
        g_boxed_free(GDK_TYPE_FILE_LIST, list);
        return p;
    };

    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus, "enter", CB([](View *v) { if (v->onFocusIn) v->onFocusIn(); }), this);
    gtk_widget_add_controller(ivWidget_, focus);
    GtkGesture *anyClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(anyClick), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(anyClick), GTK_PHASE_CAPTURE);
    g_signal_connect_swapped(anyClick, "pressed", CB([](View *v) { if (v->onFocusIn) v->onFocusIn(); }), this);
    gtk_widget_add_controller(overlay_, GTK_EVENT_CONTROLLER(anyClick));

    lister_.onStarted = [this] {
        updatePlaceholder();
        if (onLoading) onLoading(true);
    };
    lister_.onFinished = [this] {
        updatePlaceholder();
        checkPendingSelection();
        if (onLoading) onLoading(false);
    };
    lister_.onError = [this](const std::string &e) { if (onError) onError(e); };
    lister_.onLocationGone = [this] { if (onLocationGone) onLocationGone(); };
    lister_.mountOperation = [this]() -> GMountOperation * { return mountOperation ? mountOperation() : nullptr; };

    settingsListener_ = add_settings_listener([this] { settingsChanged(); });
    buildPipeline();
}

View::~View()
{
    remove_settings_listener(settingsListener_);
    alive_.reset();
    lister_.stop();
    g_clear_weak_pointer(&hovered_);
    iv_->setModel(nullptr);
    if (sel_) g_signal_handlers_disconnect_by_data(sel_, this);
    g_clear_object(&sel_);
    g_clear_object(&sortModel_);
    g_clear_object(&treeModel_);
    g_clear_object(&filterModel_);
    g_clear_object(&filter_);
    g_object_unref(overlay_);
}

Ref<GFile> View::locationFile() const
{
    if (is_virtual_uri(uri_)) return {};
    return Ref<GFile>(g_file_new_for_uri(uri_.c_str()));
}

bool View::useTree() const
{
    return props_.viewMode == ModeDetails && !columnMode_ && settings().detailsExpandableFolders && !props_.groupedSorting && !isVirtual();
}

// ---- model pipeline ------------------------------------------------------------------

gboolean View::filterItem(gpointer obj, gpointer self)
{
    auto *v = static_cast<View *>(self);
    BelugaItem *it = item_from_object(obj);
    if (!it) return FALSE;
    const ItemData &d = item_data(it);
    if (!v->props_.hiddenFilesShown && d.isHidden) return FALSE;
    if (settings().hideBackupFiles && (d.isBackup || d.mime == "application/x-trash")) return FALSE;
    if (!v->nameFilter_.empty() && !filter_match(v->nameFilter_, d.name)) return FALSE;
    return TRUE;
}

GListModel *View::createChildModel(gpointer obj, gpointer self)
{
    auto *v = static_cast<View *>(self);
    BelugaItem *it = item_from_object(obj);
    if (!it || !item_data(it).isDir) return nullptr;
    auto *lister = new DirLister;
    lister->open(item_data(it).uri);
    GtkFilterListModel *child = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(lister->model())), GTK_FILTER(g_object_ref(v->filter_)));
    g_object_set_data_full(G_OBJECT(child), "beluga-lister", lister, [](gpointer p) { delete static_cast<DirLister *>(p); });
    return G_LIST_MODEL(child);
}

static int cmp_num(gint64 a, gint64 b) { return a < b ? -1 : a > b ? 1 : 0; }

int View::compareRole(BelugaItem *a, BelugaItem *b, const std::string &role) const
{
    const ItemData &x = item_data(a), &y = item_data(b);
    if (role == "size") {
        if (x.isDir && y.isDir) {
            if (settings().dirSizeMode == DirSizeMode::Size) return cmp_num((gint64)x.dirSize, (gint64)y.dirSize);
            return cmp_num(x.dirCount, y.dirCount);
        }
        return cmp_num((gint64)x.size, (gint64)y.size);
    }
    if (role == "modificationtime") return cmp_num(x.mtime, y.mtime);
    if (role == "creationtime") return cmp_num(x.ctime, y.ctime);
    if (role == "accesstime") return cmp_num(x.atime, y.atime);
    if (role == "deletiontime") return cmp_num(x.deletionTime, y.deletionTime);
    if (role == "text") return 0;
    std::string ta = item_role_text(a, role), tb = item_role_text(b, role);
    return g_utf8_collate(ta.c_str(), tb.c_str());
}

int View::compareItems(gconstpointer pa, gconstpointer pb, gpointer self)
{
    auto *v = static_cast<View *>(self);
    BelugaItem *a = item_from_object((gpointer)pa), *b = item_from_object((gpointer)pb);
    if (!a || !b) return 0;
    const ItemData &x = item_data(a), &y = item_data(b);
    if (v->props_.sortFoldersFirst && x.isDir != y.isDir) return x.isDir ? -1 : 1;
    if (v->props_.sortHiddenLast && x.isHidden != y.isHidden) return x.isHidden ? 1 : -1;
    int c = v->compareRole(a, b, v->props_.sortRole);
    if (c == 0) c = x.key.compare(y.key);
    if (c == 0) c = x.uri.compare(y.uri);
    if (v->props_.sortDescending) c = -c;
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

namespace {
// Group key: a comparable value plus a label.
struct GroupKey {
    gint64 num = 0;
    std::string text;
    std::string label;
};

gint64 day_start(GDateTime *dt)
{
    GDateTime *d = g_date_time_new_local(g_date_time_get_year(dt), g_date_time_get_month(dt), g_date_time_get_day_of_month(dt), 0, 0, 0);
    gint64 r = g_date_time_to_unix(d);
    g_date_time_unref(d);
    return r;
}

GroupKey date_group(gint64 t)
{
    GroupKey k;
    if (t <= 0) {
        k.num = 0;
        k.label = _("Unknown");
        return k;
    }
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *dt = g_date_time_new_from_unix_local(t);
    gint64 today = day_start(now);
    gint64 day = day_start(dt);
    gint64 days = (today - day) / 86400;
    if (days <= 0) {
        k.num = today;
        k.label = _("Today");
    } else if (days == 1) {
        k.num = day;
        k.label = _("Yesterday");
    } else if (days < 7) {
        k.num = day;
        k.label = take(g_date_time_format(dt, "%A"));
    } else if (days < 14) {
        k.num = today - 13 * 86400;
        k.label = _("One Week Ago");
    } else if (days < 21) {
        k.num = today - 20 * 86400;
        k.label = _("Two Weeks Ago");
    } else if (days < 28) {
        k.num = today - 27 * 86400;
        k.label = _("Three Weeks Ago");
    } else if (g_date_time_get_year(dt) == g_date_time_get_year(now) && g_date_time_get_month(dt) == g_date_time_get_month(now)) {
        k.num = today - 28 * 86400;
        k.label = _("Earlier this Month");
    } else {
        GDateTime *m = g_date_time_new_local(g_date_time_get_year(dt), g_date_time_get_month(dt), 1, 0, 0, 0);
        k.num = g_date_time_to_unix(m);
        if (k.num >= today - 28 * 86400) k.num = today - 29 * 86400;
        k.label = g_date_time_get_year(dt) == g_date_time_get_year(now) ? take(g_date_time_format(dt, "%B"))
                                                                          : take(g_date_time_format(dt, "%B %Y"));
        g_date_time_unref(m);
    }
    g_date_time_unref(dt);
    g_date_time_unref(now);
    return k;
}

GroupKey group_key(BelugaItem *it, const std::string &role)
{
    const ItemData &d = item_data(it);
    GroupKey k;
    if (role == "text") {
        gunichar c = g_utf8_get_char_validated(d.name.c_str(), -1);
        if (c == (gunichar)-1 || c == (gunichar)-2) c = '?';
        if (c == '.' && d.name.size() > 1) c = g_utf8_get_char(g_utf8_next_char(d.name.c_str()));
        if (g_unichar_isdigit(c)) {
            k.text = "0";
            k.label = _("0 - 9");
        } else if (g_unichar_isalpha(c)) {
            char buf[8];
            buf[g_unichar_to_utf8(g_unichar_toupper(c), buf)] = 0;
            k.label = buf;
            k.text = take(g_utf8_collate_key(buf, -1));
        } else {
            k.text = "";
            k.label = _("Others");
        }
        return k;
    }
    if (role == "size") {
        if (d.isDir) {
            k.num = -1;
            k.label = _("Folders");
        } else if (d.size < 5ull * 1024 * 1024) {
            k.num = 0;
            k.label = _("Small");
        } else if (d.size < 10ull * 1024 * 1024) {
            k.num = 1;
            k.label = _("Medium");
        } else {
            k.num = 2;
            k.label = _("Big");
        }
        return k;
    }
    if (role == "modificationtime") return date_group(d.mtime);
    if (role == "creationtime") return date_group(d.ctime);
    if (role == "accesstime") return date_group(d.atime);
    if (role == "deletiontime") return date_group(d.deletionTime);
    k.label = item_role_text(it, role);
    if (k.label.empty()) k.label = _("Unknown");
    k.text = take(g_utf8_collate_key(k.label.c_str(), -1));
    return k;
}
} // namespace

int View::compareGroups(gconstpointer pa, gconstpointer pb, gpointer self)
{
    auto *v = static_cast<View *>(self);
    BelugaItem *a = item_from_object((gpointer)pa), *b = item_from_object((gpointer)pb);
    if (!a || !b) return 0;
    GroupKey x = group_key(a, v->props_.sortRole), y = group_key(b, v->props_.sortRole);
    int c = x.num < y.num ? -1 : x.num > y.num ? 1 : x.text.compare(y.text);
    if (v->props_.sortDescending) c = -c;
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}

std::string View::groupLabelFor(BelugaItem *item) const { return group_key(item, props_.sortRole).label; }

void View::buildPipeline()
{
    iv_->setModel(nullptr);
    if (sel_) g_signal_handlers_disconnect_by_data(sel_, this);
    g_clear_object(&sel_);
    g_clear_object(&sortModel_);
    g_clear_object(&treeModel_);
    g_clear_object(&filterModel_);
    if (!filter_) filter_ = GTK_FILTER(gtk_custom_filter_new(filterItem, this, nullptr));
    filterModel_ = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(lister_.model())), GTK_FILTER(g_object_ref(filter_)));
    GListModel *base = G_LIST_MODEL(filterModel_);
    GtkSorter *sorter = GTK_SORTER(gtk_custom_sorter_new(compareItems, this, nullptr));
    bool tree = useTree();
    if (tree) {
        treeModel_ = gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(filterModel_)), FALSE, FALSE, createChildModel, this, nullptr);
        base = G_LIST_MODEL(treeModel_);
        sorter = GTK_SORTER(gtk_tree_list_row_sorter_new(sorter));
    }
    sortModel_ = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(base)), sorter);
    if (props_.groupedSorting && !tree) {
        GtkSorter *ss = GTK_SORTER(gtk_custom_sorter_new(compareGroups, this, nullptr));
        gtk_sort_list_model_set_section_sorter(sortModel_, ss);
        g_object_unref(ss);
    }
    sel_ = GTK_SELECTION_MODEL(gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(sortModel_))));
    g_signal_connect_swapped(sel_, "selection-changed", CB([](View *v) { if (v->onSelectionChanged) v->onSelectionChanged(); }), this);
    g_signal_connect_swapped(sel_, "items-changed", CB([](View *v) {
        v->updatePlaceholder();
        if (!v->pendingSelect_.empty()) v->checkPendingSelection();
        if (v->onContentsChanged) v->onContentsChanged();
    }), this);
    treeBuilt_ = tree;
    iv_->setTreeMode(tree);
    iv_->setModel(sel_);
}

void View::applyToItemView()
{
    int mode = props_.viewMode;
    bool list = listMode();
    iv_->setHeaderVisible(!list);
    iv_->setMode(list ? 2 : mode);
    iv_->setRoles(list ? std::vector<std::string>() : props_.rolesForMode(mode));
    iv_->setColumnWidths(list ? std::map<std::string, int>() : props_.columnWidths);
    iv_->setIconSize(icon_size_for_zoom(zoomLevel()));
    iv_->setPreviews(props_.previewsShown);
    iv_->setGrouped(props_.groupedSorting && !treeBuilt_ && !list);
    iv_->setSortIndicator(props_.sortRole, props_.sortDescending);
    const ModeSettings &ms = settings().mode(mode);
    iv_->setFont(ms.useSystemFont ? std::string() : ms.font);
    iv_->setSelectionMode(selectionMode_);
}

void View::setLocation(const std::string &uri)
{
    uri_ = uri;
    alive_ = std::make_shared<int>(0);
    g_clear_weak_pointer(&hovered_);
    props_ = load_view_props(uri);
    if (useTree() != treeBuilt_ || (props_.groupedSorting && !gtk_sort_list_model_get_section_sorter(sortModel_))
        || (!props_.groupedSorting && gtk_sort_list_model_get_section_sorter(sortModel_)))
        buildPipeline();
    else
        gtk_filter_changed(filter_, GTK_FILTER_CHANGE_DIFFERENT);
    applyToItemView();
    lister_.open(uri);
    iv_->scrollToTop();
    updatePlaceholder();
}

void View::reload()
{
    alive_ = std::make_shared<int>(0);
    lister_.reload();
}

void View::stop() { lister_.stop(); }

// ---- properties ------------------------------------------------------------------------

int View::zoomLevel() const
{
    if (props_.zoomLevel >= 0 && !columnMode_) return props_.zoomLevel;
    const ModeSettings &m = const_cast<Settings &>(settings()).mode(listMode() ? ModeDetails : props_.viewMode);
    return zoom_for_icon_size(props_.previewsShown ? m.previewSize : m.iconSize);
}

void View::applyProps(const ViewProps &p, bool save)
{
    bool rebuild = p.viewMode != props_.viewMode || p.groupedSorting != props_.groupedSorting;
    bool resort = p.sortRole != props_.sortRole || p.sortDescending != props_.sortDescending
               || p.sortFoldersFirst != props_.sortFoldersFirst || p.sortHiddenLast != props_.sortHiddenLast;
    bool refilter = p.hiddenFilesShown != props_.hiddenFilesShown;
    props_ = p;
    if (save) save_view_props(uri_, props_);
    if (rebuild && (useTree() != treeBuilt_ || p.groupedSorting != (gtk_sort_list_model_get_section_sorter(sortModel_) != nullptr))) {
        buildPipeline();
    } else {
        if (resort) {
            gtk_sorter_changed(gtk_sort_list_model_get_sorter(sortModel_), GTK_SORTER_CHANGE_DIFFERENT);
            if (GtkSorter *ss = gtk_sort_list_model_get_section_sorter(sortModel_)) gtk_sorter_changed(ss, GTK_SORTER_CHANGE_DIFFERENT);
        }
        if (refilter) gtk_filter_changed(filter_, GTK_FILTER_CHANGE_DIFFERENT);
    }
    applyToItemView();
    iv_->relayout();
    if (onPropsChanged) onPropsChanged();
}

void View::setViewMode(int mode)
{
    ViewProps p = props_;
    p.viewMode = mode;
    p.zoomLevel = -1;
    applyProps(p, true);
}

void View::setZoomLevel(int level)
{
    ViewProps p = props_;
    p.zoomLevel = std::clamp(level, kMinZoom, kMaxZoom);
    applyProps(p, true);
}

void View::resetZoom()
{
    ViewProps p = props_;
    p.zoomLevel = -1;
    applyProps(p, true);
}

void View::setPreviewsShown(bool on)
{
    ViewProps p = props_;
    p.previewsShown = on;
    p.zoomLevel = -1;
    applyProps(p, true);
}

void View::setHiddenShown(bool on)
{
    ViewProps p = props_;
    p.hiddenFilesShown = on;
    applyProps(p, true);
}

void View::setSortRole(const std::string &role)
{
    ViewProps p = props_;
    p.sortRole = role;
    applyProps(p, true);
}

void View::setSortDescending(bool on)
{
    ViewProps p = props_;
    p.sortDescending = on;
    applyProps(p, true);
}

void View::setFoldersFirst(bool on)
{
    ViewProps p = props_;
    p.sortFoldersFirst = on;
    applyProps(p, true);
}

void View::setHiddenLast(bool on)
{
    ViewProps p = props_;
    p.sortHiddenLast = on;
    applyProps(p, true);
}

void View::setGrouped(bool on)
{
    ViewProps p = props_;
    p.groupedSorting = on;
    applyProps(p, true);
}

void View::toggleRole(const std::string &role)
{
    auto roles = props_.rolesForMode(props_.viewMode);
    auto it = std::find(roles.begin(), roles.end(), role);
    if (it == roles.end()) roles.push_back(role);
    else roles.erase(it);
    // Keep the canonical column order.
    std::vector<std::string> ordered;
    for (auto &r : all_roles())
        if (std::find(roles.begin(), roles.end(), r.id) != roles.end()) ordered.push_back(r.id);
    setRoles(ordered);
}

void View::setRoles(const std::vector<std::string> &roles)
{
    ViewProps p = props_;
    p.setRolesForMode(p.viewMode, roles);
    applyProps(p, true);
}

void View::restoreDefaultProps()
{
    ViewProps p;
    p.previewsShown = settings().previewsDefault;
    if (isVirtual()) p = default_view_props(uri_);
    applyProps(p, true);
}

void View::settingsChanged()
{
    guint n = g_list_model_get_n_items(lister_.model());
    for (guint i = 0; i < n; ++i) {
        BelugaItem *it = item_at(lister_.model(), i);
        item_refresh_collation(it);
        item_data(it).nameWidth = -1;
        item_data(it).dirCount = -2;
    }
    if (useTree() != treeBuilt_) buildPipeline();
    gtk_filter_changed(filter_, GTK_FILTER_CHANGE_DIFFERENT);
    gtk_sorter_changed(gtk_sort_list_model_get_sorter(sortModel_), GTK_SORTER_CHANGE_DIFFERENT);
    // Cells are rebuilt to pick up label and highlight settings.
    int mode = props_.viewMode;
    iv_->setMode(mode == 0 ? 1 : 0);
    applyToItemView();
    iv_->relayout();
}

void View::setNameFilter(const std::string &f)
{
    if (f == nameFilter_) return;
    nameFilter_ = f;
    gtk_filter_changed(filter_, GTK_FILTER_CHANGE_DIFFERENT);
    updatePlaceholder();
}

// ---- items -------------------------------------------------------------------------------

BelugaItem *View::itemAt(int pos) const
{
    if (!sel_ || pos < 0 || pos >= (int)g_list_model_get_n_items(G_LIST_MODEL(sel_))) return nullptr;
    return item_at(G_LIST_MODEL(sel_), pos);
}

std::vector<BelugaItem *> View::selectedItems() const
{
    std::vector<BelugaItem *> v;
    for (int p : iv_->selectedPositions())
        if (BelugaItem *it = itemAt(p)) v.push_back(it);
    return v;
}

std::vector<Ref<GFile>> View::selectedFiles() const
{
    std::vector<Ref<GFile>> v;
    for (BelugaItem *it : selectedItems()) v.push_back(item_data(it).file);
    return v;
}

int View::selectionCount() const
{
    if (!sel_) return 0;
    GtkBitset *bs = gtk_selection_model_get_selection(sel_);
    int n = (int)gtk_bitset_get_size(bs);
    gtk_bitset_unref(bs);
    return n;
}

void View::selectAll() { iv_->selectAll(); }
void View::invertSelection() { iv_->invertSelection(); }
void View::clearSelection() { iv_->clearSelection(); }

BelugaItem *View::findItem(const std::string &uri) const
{
    if (!sel_) return nullptr;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sel_));
    for (guint i = 0; i < n; ++i) {
        BelugaItem *it = item_at(G_LIST_MODEL(sel_), i);
        if (it && item_data(it).uri == uri) return it;
    }
    return nullptr;
}

void View::selectUris(const std::vector<std::string> &uris)
{
    pendingSelect_.clear();
    for (auto &u : uris) pendingSelect_.insert(u);
    pendingSelectUntil_ = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    checkPendingSelection();
}

void View::selectAndShow(const std::string &uri) { selectUris({uri}); }

void View::checkPendingSelection()
{
    if (pendingSelect_.empty() || !sel_) return;
    if (g_get_monotonic_time() > pendingSelectUntil_) {
        pendingSelect_.clear();
        return;
    }
    std::vector<BelugaItem *> found;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sel_));
    for (guint i = 0; i < n; ++i) {
        BelugaItem *it = item_at(G_LIST_MODEL(sel_), i);
        if (it && pendingSelect_.count(item_data(it).uri)) found.push_back(it);
    }
    if (found.empty()) return;
    bool all = found.size() >= pendingSelect_.size();
    iv_->selectItems(found, true);
    if (all) pendingSelect_.clear();
}

void View::setCutUris(const std::set<std::string> &uris)
{
    cutUris_ = uris;
    guint n = g_list_model_get_n_items(lister_.model());
    for (guint i = 0; i < n; ++i) {
        BelugaItem *it = item_at(lister_.model(), i);
        bool cut = uris.count(item_data(it).uri) > 0;
        if (cut != item_data(it).cut) {
            item_data(it).cut = cut;
            item_emit_changed(it);
        }
    }
}

void View::countItems(int &files, int &folders, guint64 &size) const
{
    files = folders = 0;
    size = 0;
    if (!sel_) return;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(sel_));
    for (guint i = 0; i < n; ++i) {
        BelugaItem *it = item_at(G_LIST_MODEL(sel_), i);
        if (!it) continue;
        if (item_data(it).isDir) folders++;
        else {
            files++;
            size += item_data(it).size;
        }
    }
}

void View::updatePlaceholder()
{
    bool empty = !sel_ || g_list_model_get_n_items(G_LIST_MODEL(sel_)) == 0;
    const char *text = nullptr;
    if (empty) {
        if (lister_.loading()) text = isSearch() ? _("Searching…") : nullptr;
        else if (!nameFilter_.empty()) text = _("No items matching the filter");
        else if (isSearch()) text = _("No items found");
        else if (isTrash()) text = _("Trash is empty");
        else text = _("Folder is empty");
    }
    gtk_label_set_text(GTK_LABEL(placeholder_), text ? text : "");
    gtk_widget_set_visible(placeholder_, text != nullptr);
}

void View::focus() { gtk_widget_grab_focus(ivWidget_); }

void View::setSelectionMode(bool on)
{
    selectionMode_ = on;
    iv_->setSelectionMode(on);
}

// ---- previews and folder sizes ------------------------------------------------------------

void View::requestPreview(BelugaItem *item)
{
    ItemData &d = item_data(item);
    if (d.isDir) {
        d.thumbFailed = true;
        return;
    }
    thumbs::Request r;
    r.uri = d.uri;
    r.path = take(g_file_get_path(d.file.get()));
    r.mime = d.mime;
    r.mtime = d.mtime;
    r.size = d.size;
    r.pixels = iv_->iconSize() * gtk_widget_get_scale_factor(ivWidget_);
    if (!thumbs::supported(r.mime, !r.path.empty()) || !thumbs::allowed(r)) {
        d.thumbFailed = true;
        return;
    }
    d.thumbPending = true;
    Ref<BelugaItem> keep = Ref<BelugaItem>::share(item);
    int pixels = r.pixels;
    thumbs::request(r, alive_, [keep, pixels](GdkPixbuf *p) {
        ItemData &dd = item_data(keep.get());
        dd.thumbPending = false;
        if (p) {
            dd.thumb = Ref<GdkPaintable>(GDK_PAINTABLE(texture_from_pixbuf(p)));
            dd.thumbPixels = pixels;
        } else if (!dd.thumb) {
            dd.thumbFailed = true;
        } else {
            dd.thumbPixels = pixels;
        }
        item_emit_changed(keep.get());
    });
}

void View::requestDirSize(BelugaItem *item)
{
    ItemData &d = item_data(item);
    if (settings().dirSizeMode == DirSizeMode::None) return;
    d.dirPending = true;
    Ref<BelugaItem> keep = Ref<BelugaItem>::share(item);
    dirsize::request(d.uri, props_.hiddenFilesShown, settings().dirSizeMode == DirSizeMode::Size,
                     settings().recursiveDirSizeLimit, alive_, [keep](int count, guint64 size) {
        ItemData &dd = item_data(keep.get());
        dd.dirPending = false;
        dd.dirCount = count;
        dd.dirSize = size;
        item_emit_changed(keep.get());
    });
}

// ---- renaming --------------------------------------------------------------------------------

void View::doRename(BelugaItem *item, const std::string &newName)
{
    ItemData &d = item_data(item);
    std::string name = trim(newName);
    if (name.empty() || name == d.name) return;
    if (name.find('/') != std::string::npos) {
        show_error(overlay_, _("A file name cannot contain “/”."));
        return;
    }
    Ref<BelugaItem> keep = Ref<BelugaItem>::share(item);
    auto perform = [this, keep, name] {
        ItemData &dd = item_data(keep.get());
        Ref<GFile> renamed;
        GError *err = nullptr;
        std::string oldUri = dd.uri;
        if (!rename_file(dd.file.get(), name, renamed, &err)) {
            show_error(overlay_, printf_str(_("Could not rename “%s”."), dd.name.c_str()), err->message);
            g_error_free(err);
            return;
        }
        UndoCommand c;
        c.type = UndoCommand::Rename;
        c.pairs.emplace_back(oldUri, file_uri(renamed.get()));
        UndoManager::instance().push(c);
        selectUris({file_uri(renamed.get())});
    };
    std::string oldExt = split_extension(d.name, d.isDir).second, newExt = split_extension(name, d.isDir).second;
    if (settings().confirmRenameFileType && !d.isDir && casefold(oldExt) != casefold(newExt)) {
        ask(overlay_, _("Change the file type?"),
            printf_str(_("Renaming “%s” to “%s” changes its extension. The file may open with a different application or not at all."),
                       d.name.c_str(), name.c_str()),
            {_("Cancel"), _("Rename")}, 1, 0, [perform](int r) {
                if (r == 1) perform();
            });
        return;
    }
    perform();
}

void View::renameSelected()
{
    auto items = selectedItems();
    if (items.empty() || isTrash()) return;
    if (items.size() == 1 && settings().renameInline) {
        BelugaItem *it = items[0];
        int pos = -1;
        guint n = g_list_model_get_n_items(G_LIST_MODEL(sel_));
        for (guint i = 0; i < n; ++i)
            if (item_at(G_LIST_MODEL(sel_), i) == it) {
                pos = (int)i;
                break;
            }
        if (pos < 0) return;
        const ItemData &d = item_data(it);
        int stem = (int)g_utf8_strlen(split_extension(d.name, d.isDir).first.c_str(), -1);
        Ref<BelugaItem> keep = Ref<BelugaItem>::share(it);
        iv_->startRename(pos, 0, stem, [this, keep](const std::string &name) { doRename(keep.get(), name); });
        return;
    }
    show_rename_dialog(overlay_, items);
}

} // namespace beluga
