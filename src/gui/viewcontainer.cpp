// SPDX-License-Identifier: GPL-2.0-or-later
#include "viewcontainer.hpp"

#include "archive.hpp"

#include "gtkutil.hpp"
#include "mainwindow.hpp"
#include "places.hpp"
#include "settings.hpp"

namespace beluga {

static GtkWidget *action_button(const char *icon, const char *label, const char *action)
{
    GtkWidget *b = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
    gtk_box_append(GTK_BOX(box), gtk_label_new_with_mnemonic(label));
    gtk_button_set_child(GTK_BUTTON(b), box);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_add_css_class(b, "flat");
    return b;
}

ViewContainer::ViewContainer(MainWindow *win, const std::string &startUri) : win_(win)
{
    view_ = std::make_unique<View>();
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(root_);
    gtk_widget_add_css_class(root_, "beluga-container");
    gtk_widget_set_hexpand(root_, TRUE);
    gtk_widget_set_vexpand(root_, TRUE);

    // The location bar lives in the window's tool bar (MainWindow::syncLocationBars), or at the top of this
    // container while the tool bar is hidden.

    message_ = gtk_revealer_new();
    GtkWidget *mbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(mbox, "beluga-message");
    messageLabel_ = gtk_label_new(nullptr);
    gtk_label_set_wrap(GTK_LABEL(messageLabel_), TRUE);
    gtk_label_set_xalign(GTK_LABEL(messageLabel_), 0);
    gtk_widget_set_hexpand(messageLabel_, TRUE);
    gtk_box_append(GTK_BOX(mbox), messageLabel_);
    GtkWidget *mclose = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(mclose, "flat");
    g_signal_connect_swapped(mclose, "clicked", CB([](GtkRevealer *r) { gtk_revealer_set_reveal_child(r, FALSE); }), message_);
    gtk_box_append(GTK_BOX(mbox), mclose);
    gtk_revealer_set_child(GTK_REVEALER(message_), mbox);
    gtk_box_append(GTK_BOX(root_), message_);

    gtk_box_append(GTK_BOX(root_), search_.widget());

    selTop_ = gtk_revealer_new();
    GtkWidget *tbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(tbox, "beluga-selection-bar");
    GtkWidget *tl = gtk_label_new(_("Selection Mode: click files or folders to select or deselect them."));
    gtk_label_set_xalign(GTK_LABEL(tl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(tl), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(tl, TRUE);
    gtk_box_append(GTK_BOX(tbox), tl);
    GtkWidget *exit = gtk_button_new_with_mnemonic(_("_Exit Selection Mode"));
    g_signal_connect_swapped(exit, "clicked", CB([](ViewContainer *c) { c->setSelectionMode(false); }), this);
    gtk_box_append(GTK_BOX(tbox), exit);
    gtk_revealer_set_child(GTK_REVEALER(selTop_), tbox);
    gtk_box_append(GTK_BOX(root_), selTop_);

    viewSlot_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(viewSlot_, TRUE);
    gtk_widget_set_vexpand(view_->widget(), TRUE);
    gtk_box_append(GTK_BOX(viewSlot_), view_->widget());
    gtk_box_append(GTK_BOX(root_), viewSlot_);
    gtk_overlay_add_overlay(GTK_OVERLAY(view_->overlay()), status_.smallLabel());

    selBottom_ = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(selBottom_), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    GtkWidget *bscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bscroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(bscroll), TRUE);
    GtkWidget *bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bbox, "beluga-selection-bar");
    selCount_ = gtk_label_new(nullptr);
    gtk_widget_set_margin_end(selCount_, 8);
    gtk_box_append(GTK_BOX(bbox), selCount_);
    gtk_box_append(GTK_BOX(bbox), action_button("edit-select-all", _("Select _All"), "win.select-all"));
    gtk_box_append(GTK_BOX(bbox), action_button("edit-cut", _("Cu_t"), "win.cut"));
    gtk_box_append(GTK_BOX(bbox), action_button("edit-copy", _("_Copy"), "win.copy"));
    gtk_box_append(GTK_BOX(bbox), action_button("edit-paste", _("_Paste"), "win.paste"));
    gtk_box_append(GTK_BOX(bbox), action_button("edit-rename", _("_Rename"), "win.rename"));
    gtk_box_append(GTK_BOX(bbox), action_button("edit-copy", _("_Duplicate"), "win.duplicate"));
    gtk_box_append(GTK_BOX(bbox), action_button("user-trash", _("Move to _Trash"), "win.move-to-trash"));
    gtk_box_append(GTK_BOX(bbox), action_button("edit-delete", _("De_lete"), "win.delete"));
    gtk_box_append(GTK_BOX(bbox), action_button("document-properties", _("Pr_operties"), "win.properties"));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(bscroll), bbox);
    gtk_revealer_set_child(GTK_REVEALER(selBottom_), bscroll);
    gtk_box_append(GTK_BOX(root_), selBottom_);

    gtk_box_append(GTK_BOX(root_), filter_.widget());
    gtk_box_append(GTK_BOX(root_), status_.widget());

    // ---- view callbacks
    View *v = view_.get();
    v->onActivate = [this](const std::vector<BelugaItem *> &items, GdkModifierType mods) { win_->openItems(this, items, mods); };
    v->onMiddleClick = [this](BelugaItem *it) {
        if (item_data(it).isDir) win_->openInNewTab(item_data(it).uri, false);
        else win_->openItems(this, {it}, GdkModifierType(0));
    };
    v->onContextMenu = [this](int pos, double x, double y) { win_->showContextMenu(this, pos, x, y); };
    v->onSelectionChanged = [this] {
        scheduleStatus();
        win_->selectionChanged(this);
        if (columnsActive_ && !updatingColumns_ && !selectionMode()) {
            auto sel = view_->selectedItems();
            if (sel.size() == 1 && item_data(sel[0]).isDir) columnNavigateLater(item_data(sel[0]).uri);
        }
    };
    v->onHoverChanged = [this] {
        updateStatus();
        win_->hoverChanged(this);
    };
    v->onPropsChanged = [this] {
        updateColumns();
        status_.setZoom(view_->zoomLevel());
        if (active_) win_->updateActions();
    };
    v->onContentsChanged = [this] { scheduleStatus(); };
    v->onLoading = [this](bool on) {
        loc_.setLoading(on);
        scheduleStatus();
        if (active_) win_->updateActions();
    };
    v->onError = [this](const std::string &e) { showMessage(e, true); };
    v->onLocationGone = [this] { handleLocationGone(); };
    v->onDrop = [this](GFile *dest, GdkFileList *files, GdkModifierType mods, double x, double y) {
        win_->handleDrop(dest, files, mods, view_->itemView()->widget(), x, y);
    };
    v->onSpringLoad = [this](const std::string &uri) { navigate(uri); };
    v->onExtraButton = [this](int b) {
        if (b == 8) goBack();
        else if (b == 9) goForward();
    };
    v->onEmptyDoubleClick = [this] {
        switch (settings().doubleClickViewAction) {
        case 1: view_->selectAll(); break;
        case 2: goUp(); break;
        case 3: win_->openInNewTab(uri(), true); break;
        case 4: view_->setHiddenShown(!view_->props().hiddenFilesShown); break;
        default: break;
        }
    };
    v->onEscape = [this] {
        if (selectionMode()) setSelectionMode(false);
        else if (filterBarVisible()) setFilterBarVisible(false);
        else view_->clearSelection();
    };
    v->onFocusIn = [this] { win_->containerActivated(this); };
    v->mountOperation = [this]() -> GMountOperation * { return gtk_mount_operation_new(win_->window()); };

    // ---- location bar
    loc_.onNavigate = [this](const std::string &u) { navigate(u); };
    loc_.onOpenInNewTab = [this](const std::string &u) { win_->openInNewTab(u, false); };
    loc_.onDrop = [this](GFile *dest, GdkFileList *files, GdkModifierType mods, GtkWidget *w, double x, double y) {
        win_->handleDrop(dest, files, mods, w, x, y);
    };
    loc_.onActivated = [this] { win_->containerActivated(this); };
    loc_.onEditingFinished = [this] { view_->focus(); };
    loc_.showHidden = [this] { return view_->props().hiddenFilesShown; };

    // ---- filter and search
    filter_.onChanged = [this](const std::string &t) {
        view_->setNameFilter(t);
        scheduleStatus();
    };
    filter_.onClose = [this] { setFilterBarVisible(false); };
    filter_.onFocusView = [this] { view_->focus(); };
    search_.onSearch = [this](const std::string &u) {
        searchNavigation_ = true;
        navigate(u, !is_search_uri(uri()));
        searchNavigation_ = false;
    };
    search_.onClose = [this] { setSearchBarVisible(false); };
    search_.onFocusView = [this] { view_->focus(); };

    status_.onZoom = [this](int level) { view_->setZoomLevel(level); };

    settingsListener_ = add_settings_listener([this] { applySettings(); });
    applySettings();
    if (settings().filterBar) setFilterBarVisible(true);
    navigate(startUri);
}

ViewContainer::~ViewContainer()
{
    remove_settings_listener(settingsListener_);
    if (statusSource_) g_source_remove(statusSource_);
    if (messageSource_) g_source_remove(messageSource_);
    if (columnNavSource_) g_source_remove(columnNavSource_);
    parents_.clear();
    view_->stop();
    g_object_unref(root_);
}

bool ViewContainer::canGoUp() const
{
    if (archive::is_archive_uri(uri())) return true;
    if (is_virtual_uri(uri())) return is_search_uri(uri());
    Ref<GFile> f(g_file_new_for_uri(uri().c_str()));
    Ref<GFile> parent(g_file_get_parent(f.get()));
    return (bool)parent;
}

void ViewContainer::saveHistoryState()
{
    if (index_ < 0 || index_ >= (int)history_.size()) return;
    HistoryEntry &e = history_[index_];
    e.scroll = view_->scrollPosition();
    BelugaItem *cur = view_->itemView()->currentItem();
    e.current = cur ? item_data(cur).uri : std::string();
}

void ViewContainer::restoreHistoryState(const HistoryEntry &e)
{
    if (!e.current.empty()) view_->selectAndShow(e.current);
    if (e.scroll > 0) view_->setScrollPosition(e.scroll);
}

void ViewContainer::open(const std::string &u)
{
    view_->setLocation(u);
    loc_.setUri(u);
    status_.setLocation(u);
    status_.setZoom(view_->zoomLevel());
    search_.setFolder(u);
    if (is_search_uri(u)) {
        SearchQuery q;
        SearchQuery::fromUri(u, q);
        if (!searchNavigation_) search_.showQuery(q);
        search_.setVisible(true);
    } else if (search_.visible() && !searchNavigation_) {
        search_.setVisible(false);
    }
    if (!filter_.locked() && !filter_.text().empty()) filter_.clear();
    else if (filter_.locked()) view_->setNameFilter(filter_.text());
    gtk_revealer_set_reveal_child(GTK_REVEALER(message_), FALSE);
    updateColumns();
    updateStatus();
    win_->containerUriChanged(this);
}

// ---- Columns view --------------------------------------------------------------------------
// The current folder is the last column (the main view); the folders above it, up to the
// folder where the mode was entered, are narrow list views to its left.

void ViewContainer::columnNavigateLater(const std::string &target)
{
    columnNavTarget_ = target;
    if (columnNavSource_) return;
    columnNavSource_ = g_idle_add([](gpointer p) -> gboolean {
        auto *c = static_cast<ViewContainer *>(p);
        c->columnNavSource_ = 0;
        std::string t = c->columnNavTarget_;
        if (c->columnsActive_ && !t.empty() && t != c->uri()) c->navigate(t);
        return G_SOURCE_REMOVE;
    }, this);
}

void ViewContainer::updateColumns()
{
    bool want = view_->props().viewMode == ModeColumns && !view_->isVirtual();
    GtkWidget *main = view_->widget();
    if (!want) {
        if (!columnsActive_) return;
        gtk_box_remove(GTK_BOX(colBox_), main);
        for (auto &pv : parents_) gtk_box_remove(GTK_BOX(colBox_), pv->widget());
        parents_.clear();
        gtk_box_remove(GTK_BOX(viewSlot_), colScroll_);
        colScroll_ = colBox_ = nullptr;
        gtk_widget_set_size_request(main, -1, -1);
        gtk_box_append(GTK_BOX(viewSlot_), main);
        columnsActive_ = false;
        return;
    }
    updatingColumns_ = true;
    if (!columnsActive_) {
        gtk_box_remove(GTK_BOX(viewSlot_), main);
        colScroll_ = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(colScroll_), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
        gtk_widget_set_vexpand(colScroll_, TRUE);
        colBox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(colScroll_), colBox_);
        gtk_box_append(GTK_BOX(viewSlot_), colScroll_);
        columnsActive_ = true;
        columnsBase_.clear();
    } else {
        gtk_box_remove(GTK_BOX(colBox_), main);
    }
    Ref<GFile> curFile(g_file_new_for_uri(uri().c_str()));
    std::string cur = file_uri(curFile.get());
    if (columnsBase_.empty() || !(cur == columnsBase_ || starts_with(cur, columnsBase_ + "/"))) columnsBase_ = cur;
    std::vector<std::string> chain;   // base ... parent of the current folder
    for (Ref<GFile> f(g_file_get_parent(curFile.get())); f && cur != columnsBase_; f = Ref<GFile>(g_file_get_parent(f.get()))) {
        chain.insert(chain.begin(), file_uri(f.get()));
        if (chain.front() == columnsBase_) break;
    }
    size_t keep = 0;
    while (keep < parents_.size() && keep < chain.size() && parents_[keep]->location() == chain[keep]) ++keep;
    while (parents_.size() > keep) {
        gtk_box_remove(GTK_BOX(colBox_), parents_.back()->widget());
        parents_.pop_back();
    }
    for (size_t i = keep; i < chain.size(); ++i) {
        auto pv = std::make_unique<View>();
        View *raw = pv.get();
        raw->setColumnMode(true);
        raw->setLocation(chain[i]);
        raw->setColumnMode(true);
        gtk_widget_set_hexpand(raw->widget(), FALSE);
        gtk_widget_set_size_request(raw->widget(), 230, -1);
        gtk_widget_add_css_class(raw->widget(), "beluga-column");
        raw->onActivate = [this](const std::vector<BelugaItem *> &items, GdkModifierType mods) { win_->openItems(this, items, mods); };
        raw->onSelectionChanged = [this, raw] {
            if (updatingColumns_) return;
            auto sel = raw->selectedItems();
            if (sel.size() != 1 || !item_data(sel[0]).isDir) return;
            const std::string &u = item_data(sel[0]).uri;
            std::string here = uri();
            if (here == u || starts_with(here, u + "/")) return;   // already part of the shown path
            columnNavigateLater(u);
        };
        raw->onFocusIn = [this] { win_->containerActivated(this); };
        raw->onDrop = [this, raw](GFile *dest, GdkFileList *files, GdkModifierType mods, double x, double y) {
            win_->handleDrop(dest, files, mods, raw->itemView()->widget(), x, y);
        };
        raw->onEmptyDoubleClick = [this, raw] { navigate(raw->location()); };
        gtk_box_append(GTK_BOX(colBox_), raw->widget());
        parents_.push_back(std::move(pv));
    }
    for (size_t i = 0; i < parents_.size(); ++i) parents_[i]->selectUris({i + 1 < chain.size() ? chain[i + 1] : cur});
    gtk_widget_set_hexpand(main, TRUE);
    gtk_widget_set_size_request(main, 260, -1);
    gtk_box_append(GTK_BOX(colBox_), main);
    updatingColumns_ = false;
    g_idle_add([](gpointer p) -> gboolean {
        GtkAdjustment *a = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(p));
        gtk_adjustment_set_value(a, gtk_adjustment_get_upper(a));
        g_object_unref(p);
        return G_SOURCE_REMOVE;
    }, g_object_ref(colScroll_));
}

void ViewContainer::navigate(const std::string &targetIn, bool addHistory)
{
    if (targetIn.empty()) return;
    // A typed path that runs into an archive (~/x.zip/docs) opens inside it.
    std::string target = targetIn;
    if (starts_with(target, "file:")) {
        std::string path = take(g_filename_from_uri(target.c_str(), nullptr, nullptr));
        if (!path.empty() && !g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
            std::string inArchive = archive::resolve_path(path);
            if (!inArchive.empty()) target = inArchive;
        }
    }
    if (index_ >= 0 && target == uri()) {
        view_->focus();
        return;
    }
    saveHistoryState();
    if (addHistory || index_ < 0) {
        if (index_ + 1 < (int)history_.size()) history_.erase(history_.begin() + index_ + 1, history_.end());
        history_.push_back({target, 0, {}});
        if (history_.size() > 100) history_.erase(history_.begin());
        index_ = (int)history_.size() - 1;
    } else {
        history_[index_].uri = target;
    }
    open(target);
}

void ViewContainer::goHistory(int index)
{
    if (index < 0 || index >= (int)history_.size() || index == index_) return;
    saveHistoryState();
    index_ = index;
    HistoryEntry e = history_[index_];
    open(e.uri);
    restoreHistoryState(e);
}

void ViewContainer::goBack() { goHistory(index_ - 1); }
void ViewContainer::goForward() { goHistory(index_ + 1); }

void ViewContainer::goUp()
{
    std::string cur = uri();
    if (is_search_uri(cur)) {
        SearchQuery q;
        SearchQuery::fromUri(cur, q);
        navigate(q.rootUri);
        return;
    }
    if (archive::is_archive_uri(cur)) {
        // Up from the top of an archive: its folder, with the archive selected.
        std::string a, inner;
        archive::parse_uri(cur, a, inner);
        navigate(archive::parent_uri(cur));
        view_->selectAndShow(inner.empty() ? file_uri(Ref<GFile>(g_file_new_for_path(a.c_str())).get()) : cur);
        return;
    }
    if (is_virtual_uri(cur)) return;
    Ref<GFile> f(g_file_new_for_uri(cur.c_str()));
    Ref<GFile> parent(g_file_get_parent(f.get()));
    if (!parent) return;
    navigate(file_uri(parent.get()));
    view_->selectAndShow(cur);
}

void ViewContainer::goHome() { navigate(settings().homeUrl); }

std::vector<std::pair<int, std::string>> ViewContainer::backList() const
{
    std::vector<std::pair<int, std::string>> v;
    for (int i = index_ - 1; i >= 0 && v.size() < 15; --i) v.emplace_back(i, history_[i].uri);
    return v;
}

std::vector<std::pair<int, std::string>> ViewContainer::forwardList() const
{
    std::vector<std::pair<int, std::string>> v;
    for (int i = index_ + 1; i < (int)history_.size() && v.size() < 15; ++i) v.emplace_back(i, history_[i].uri);
    return v;
}

void ViewContainer::reload()
{
    saveHistoryState();
    double scroll = view_->scrollPosition();
    view_->reload();
    if (scroll > 0) view_->setScrollPosition(scroll);
}

void ViewContainer::handleLocationGone()
{
    std::string cur = uri();
    Ref<GFile> f(g_file_new_for_uri(cur.c_str()));
    Ref<GFile> p = f;
    while (p && !g_file_query_exists(p.get(), nullptr)) p = Ref<GFile>(g_file_get_parent(p.get()));
    std::string target = p ? file_uri(p.get()) : settings().homeUrl;
    navigate(target, false);
    showMessage(printf_str(_("The folder “%s” no longer exists."), display_location(cur).c_str()), false);
}

void ViewContainer::setActive(bool on)
{
    active_ = on;
    loc_.setActive(on);
    if (on) gtk_widget_remove_css_class(root_, "inactive");
    else gtk_widget_add_css_class(root_, "inactive");
}

void ViewContainer::setFilterBarVisible(bool on)
{
    filter_.setVisible(on);
    if (on) filter_.focus();
    else {
        if (!filter_.text().empty()) filter_.clear();
        view_->setNameFilter({});
        view_->focus();
    }
    if (active_) win_->updateActions();
}

void ViewContainer::setSearchBarVisible(bool on)
{
    search_.setVisible(on);
    if (on) {
        search_.setFolder(uri());
        search_.focus();
    } else {
        if (is_search_uri(uri())) {
            SearchQuery q;
            SearchQuery::fromUri(uri(), q);
            navigate(q.rootUri);
        }
        view_->focus();
    }
    if (active_) win_->updateActions();
}

void ViewContainer::setSelectionMode(bool on)
{
    view_->setSelectionMode(on);
    gtk_revealer_set_reveal_child(GTK_REVEALER(selTop_), on);
    gtk_revealer_set_reveal_child(GTK_REVEALER(selBottom_), on);
    updateStatus();
    if (active_) win_->updateActions();
}

void ViewContainer::showMessage(const std::string &text, bool error)
{
    gtk_label_set_text(GTK_LABEL(messageLabel_), text.c_str());
    GtkWidget *box = gtk_revealer_get_child(GTK_REVEALER(message_));
    if (error) gtk_widget_add_css_class(box, "error");
    else gtk_widget_remove_css_class(box, "error");
    gtk_revealer_set_reveal_child(GTK_REVEALER(message_), TRUE);
    if (messageSource_) g_source_remove(messageSource_);
    messageSource_ = 0;
    if (!error)
        messageSource_ = g_timeout_add_seconds(8, [](gpointer p) -> gboolean {
            auto *c = static_cast<ViewContainer *>(p);
            c->messageSource_ = 0;
            gtk_revealer_set_reveal_child(GTK_REVEALER(c->message_), FALSE);
            return G_SOURCE_REMOVE;
        }, this);
}

std::string ViewContainer::title() const
{
    const std::string &u = uri();
    if (is_trash_uri(u)) return _("Trash");
    if (is_recent_uri(u)) return _("Recent Files");
    if (is_search_uri(u)) {
        SearchQuery q;
        SearchQuery::fromUri(u, q);
        return q.title();
    }
    if (settings().showFullPathInTitlebar) return display_location(u);
    if (archive::is_archive_uri(u)) return basename_of(archive::display_path(u));   // the archive or its folder
    int pi = Places::instance().indexOf(u);
    if (pi >= 0) return Places::instance().list()[pi].label;
    Ref<GFile> f(g_file_new_for_uri(u.c_str()));
    std::string base = take(g_file_get_basename(f.get()));
    return base.empty() ? display_location(u) : base;
}

void ViewContainer::scheduleStatus()
{
    if (statusSource_) return;
    statusSource_ = g_timeout_add(60, [](gpointer p) -> gboolean {
        auto *c = static_cast<ViewContainer *>(p);
        c->statusSource_ = 0;
        c->updateStatus();
        return G_SOURCE_REMOVE;
    }, this);
}

void ViewContainer::updateStatus()
{
    View *v = view_.get();
    std::string text;
    bool transient = false;   // hover or selection info
    if (BelugaItem *h = v->hoveredItem()) {
        std::string size = item_size_text(h);
        text = item_data(h).name + " (" + item_type_text(h) + (size.empty() ? "" : ", " + size) + ")";
        transient = true;
    } else {
        auto sel = v->selectedItems();
        if (sel.size() == 1) {
            std::string size = item_size_text(sel[0]);
            text = printf_str(_("“%s” selected (%s%s)"), item_data(sel[0]).name.c_str(), item_type_text(sel[0]).c_str(),
                              size.empty() ? "" : (", " + size).c_str());
            transient = true;
        } else if (sel.size() > 1) {
            int files = 0, folders = 0;
            guint64 size = 0;
            for (BelugaItem *it : sel) {
                if (item_data(it).isDir) folders++;
                else {
                    files++;
                    size += item_data(it).size;
                }
            }
            text = printf_str(_("%s selected"), format_files_folders(files, folders).c_str());
            if (files) text += " (" + format_size(size) + ")";
            transient = true;
        } else if (v->loading() && v->isSearch()) {
            text = _("Searching…");
        } else if (v->loading()) {
            text = _("Loading folder…");
        } else {
            int files = 0, folders = 0;
            guint64 size = 0;
            v->countItems(files, folders, size);
            text = format_files_folders(files, folders);
            if (files) text += " (" + format_size(size) + ")";
        }
    }
    if (selectionMode()) {
        int n = v->selectionCount();
        gtk_label_set_text(GTK_LABEL(selCount_), printf_str(g_dngettext(GETTEXT_PACKAGE, "%d selected", "%d selected", n), n).c_str());
    }
    status_.setText(text);
    if (settings().statusBar == StatusBarMode::Small) gtk_widget_set_visible(status_.smallLabel(), transient && !text.empty());
    else gtk_widget_set_visible(status_.smallLabel(), FALSE);
}

void ViewContainer::applySettings()
{
    const Settings &s = settings();
    gtk_widget_set_visible(status_.widget(), s.statusBar == StatusBarMode::FullWidth);
    status_.applySettings();
    if (s.editableUrl != loc_.editing() && !loc_.editing()) loc_.setEditable(s.editableUrl);
    else if (!s.editableUrl && loc_.editing()) loc_.setEditable(false);
    if (index_ >= 0) loc_.setUri(uri());
    updateStatus();
}

} // namespace beluga
