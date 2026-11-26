// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainwindow.hpp"

#include "admin.hpp"
#include "diskusagewindow.hpp"

#include "app.hpp"
#include "archive.hpp"
#include "dialogs.hpp"
#include "fileops.hpp"
#include "folderspanel.hpp"
#include "gtkutil.hpp"
#include "infopanel.hpp"
#include "jobs.hpp"
#include "openwith.hpp"
#include "places.hpp"
#include "placespanel.hpp"
#include "properties.hpp"
#include "settings.hpp"
#include "tabpage.hpp"
#include "terminal.hpp"
#include "undo.hpp"

#include <algorithm>
#include <thread>

namespace beluga {

MainWindow *MainWindow::from(GtkWindow *w)
{
    return w ? static_cast<MainWindow *>(g_object_get_data(G_OBJECT(w), "beluga-mainwindow")) : nullptr;
}

MainWindow::MainWindow(GtkApplication *app)
{
    const Settings &s = settings();
    win_ = gtk_application_window_new(app);
    g_object_set_data(G_OBJECT(win_), "beluga-mainwindow", this);
    gtk_widget_add_css_class(win_, "beluga-window");
    gtk_window_set_default_size(GTK_WINDOW(win_), s.windowWidth, s.windowHeight);
    gtk_window_set_icon_name(GTK_WINDOW(win_), BELUGA_APP_ID);
    if (s.windowMaximized) gtk_window_maximize(GTK_WINDOW(win_));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win_), vbox);

    setupActions();
    buildToolbar();
    buildMenus();
    menubar_ = gtk_popover_menu_bar_new_from_model(menu_);
    gtk_box_append(GTK_BOX(vbox), menubar_);
    gtk_box_append(GTK_BOX(vbox), toolbar_);
    if (running_as_root()) {
        // A root Beluga (Act as Administrator, sudo) says so in every window.
        GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(bar, "beluga-admin-bar");
        GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning");
        gtk_box_append(GTK_BOX(bar), icon);
        GtkWidget *l = gtk_label_new(_("Acting as administrator: changes in this window can damage the system."));
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_widget_set_hexpand(l, TRUE);
        gtk_box_append(GTK_BOX(bar), l);
        gtk_box_append(GTK_BOX(vbox), bar);
    }

    places_ = std::make_unique<PlacesPanel>();
    folders_ = std::make_unique<FoldersPanel>();
    info_ = std::make_unique<InfoPanel>();

    leftPaned_ = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_start_child(GTK_PANED(leftPaned_), places_->widget());
    gtk_paned_set_end_child(GTK_PANED(leftPaned_), folders_->widget());
    gtk_paned_set_resize_end_child(GTK_PANED(leftPaned_), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(leftPaned_), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(leftPaned_), FALSE);
    gtk_widget_add_css_class(leftPaned_, "beluga-panels");

    notebook_ = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(notebook_), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(notebook_), FALSE);
    gtk_widget_set_hexpand(notebook_, TRUE);
    gtk_widget_set_vexpand(notebook_, TRUE);
    GtkWidget *plus = gtk_button_new_from_icon_name("tab-new");
    gtk_widget_add_css_class(plus, "flat");
    gtk_widget_set_tooltip_text(plus, _("New Tab"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(plus), "win.new-tab");
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(notebook_), plus, GTK_PACK_END);
    g_signal_connect(notebook_, "switch-page", CB([](GtkNotebook *, GtkWidget *, guint index, gpointer p) {
        static_cast<MainWindow *>(p)->onTabSwitched((int)index);
    }), this);
    g_signal_connect(notebook_, "page-reordered", CB([](GtkNotebook *, GtkWidget *child, guint index, gpointer p) {
        auto *w = static_cast<MainWindow *>(p);
        for (size_t i = 0; i < w->tabs_.size(); ++i)
            if (w->tabs_[i]->widget() == child) {
                auto t = std::move(w->tabs_[i]);
                w->tabs_.erase(w->tabs_.begin() + i);
                w->tabs_.insert(w->tabs_.begin() + std::min<size_t>(index, w->tabs_.size()), std::move(t));
                break;
            }
    }), this);

    innerPaned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(innerPaned_), notebook_);
    // The right side shows the Information panel, or the Disk Usage pane while that is open.
    duPane_ = disk_usage_pane_new(this);
    rightStack_ = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(rightStack_), info_->widget(), "info");
    gtk_stack_add_named(GTK_STACK(rightStack_), duPane_, "du");
    gtk_stack_set_hhomogeneous(GTK_STACK(rightStack_), FALSE);
    gtk_stack_set_interpolate_size(GTK_STACK(rightStack_), FALSE);
    gtk_paned_set_end_child(GTK_PANED(innerPaned_), rightStack_);
    gtk_paned_set_resize_end_child(GTK_PANED(innerPaned_), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(innerPaned_), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(innerPaned_), FALSE);
    outerPaned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(outerPaned_), leftPaned_);
    gtk_paned_set_end_child(GTK_PANED(outerPaned_), innerPaned_);
    gtk_paned_set_resize_start_child(GTK_PANED(outerPaned_), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(outerPaned_), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(outerPaned_), FALSE);
    gtk_paned_set_position(GTK_PANED(outerPaned_), s.leftPanelWidth);
    gtk_widget_set_vexpand(outerPaned_, TRUE);
    gtk_box_append(GTK_BOX(vbox), outerPaned_);
    gtk_widget_set_size_request(info_->widget(), 200, -1);
    gtk_widget_set_size_request(places_->widget(), 120, -1);

    // Panel callbacks.
    places_->onNavigate = [this](const std::string &u) { if (auto *c = activeContainer()) c->navigate(u); };
    places_->onOpenInNewTab = [this](const std::string &u) { openInNewTab(u, false); };
    places_->onOpenInNewWindow = [this](const std::string &u) { openInNewWindow(u); };
    places_->onDrop = [this](GFile *d, GdkFileList *f, GdkModifierType m, GtkWidget *w, double x, double y) { handleDrop(d, f, m, w, x, y); };
    places_->onProperties = [this](const std::string &u) { showProperties({Ref<GFile>(g_file_new_for_uri(u.c_str()))}); };
    places_->onError = [this](const std::string &m) { show_error(win_, m); };
    folders_->onNavigate = [this](const std::string &u) { if (auto *c = activeContainer()) c->navigate(u); };
    folders_->onOpenInNewTab = [this](const std::string &u) { openInNewTab(u, false); };
    folders_->onDrop = [this](GFile *d, GdkFileList *f, GdkModifierType m, GtkWidget *w, double x, double y) { handleDrop(d, f, m, w, x, y); };

    g_signal_connect(win_, "close-request", CB([](GtkWindow *, gpointer p) -> gboolean {
        auto *w = static_cast<MainWindow *>(p);
        if (!w->closing_ && settings().confirmClosingMultipleTabs && w->tabs_.size() > 1 && !App::instance().selftest) {
            ask(w->win_, _("Close this window?"),
                printf_str(_("This window has %zu tabs open. Closing it closes all of them."), w->tabs_.size()),
                {_("Cancel"), _("Close Window")}, 1, 0, [w](int r) {
                    if (r != 1) return;
                    w->closing_ = true;
                    gtk_window_close(w->window());
                });
            return TRUE;
        }
        w->closeWindow();
        return FALSE;
    }), this);

    // GTK runs application shortcuts before the focused widget sees a key, so Delete, BackSpace,
    // Space, "/", Ctrl+C and the like would act on the files while you type in the location bar,
    // the filter or an inline rename. Offer those keys to a focused text field first; the shortcut
    // still runs when the field has no use for the key (F2, F5, Ctrl+W, ...). Added last, so it
    // runs before the window's other capture-phase controllers.
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", CB([](GtkEventControllerKey *k, guint, guint, GdkModifierType, gpointer p) -> gboolean {
        GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(static_cast<MainWindow *>(p)->win_));
        if (!focus || !GTK_IS_EDITABLE(focus)) return FALSE;
        GdkEvent *ev = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(k));
        if (!ev || !App::instance().isShortcut(ev)) return FALSE;
        return gtk_event_controller_key_forward(k, focus);
    }), this);

    settingsListener_ = add_settings_listener([this] { applySettings(); });
    undoListener_ = UndoManager::instance().addListener([this] { updateActions(); });
    applySettings();
    gtk_widget_add_controller(win_, keys);
}

MainWindow::~MainWindow()
{
    remove_settings_listener(settingsListener_);
    UndoManager::instance().removeListener(undoListener_);
    tabs_.clear();
    places_.reset();
    folders_.reset();
    info_.reset();
    if (menu_) g_object_unref(menu_);
    if (closedTabsMenu_) g_object_unref(closedTabsMenu_);
}

void MainWindow::closeWindow()
{
    if (closing_ && !win_) return;
    closing_ = true;
    Settings &s = settings();
    s.windowMaximized = gtk_window_is_maximized(GTK_WINDOW(win_));
    if (!s.windowMaximized) {
        int w = 0, h = 0;
        gtk_window_get_default_size(GTK_WINDOW(win_), &w, &h);
        if (gtk_widget_get_width(win_) > 0) {
            w = gtk_widget_get_width(win_);
            h = gtk_widget_get_height(win_);
        }
        if (w > 200 && h > 150) {
            s.windowWidth = w;
            s.windowHeight = h;
        }
    }
    if (gtk_widget_get_visible(leftPaned_)) s.leftPanelWidth = gtk_paned_get_position(GTK_PANED(outerPaned_));
    if (duPaneOpen_ && gtk_widget_get_width(innerPaned_) > 0)
        s.duPaneWidth = std::max(200, gtk_widget_get_width(innerPaned_) - gtk_paned_get_position(GTK_PANED(innerPaned_)));
    s.save();
    App::instance().windowClosed(this);
    g_object_set_data(G_OBJECT(win_), "beluga-mainwindow", nullptr);
    life_.reset();
    g_idle_add([](gpointer p) -> gboolean {
        delete static_cast<MainWindow *>(p);
        return G_SOURCE_REMOVE;
    }, this);
}

void MainWindow::present() { gtk_window_present(GTK_WINDOW(win_)); }

// ---- tool bar and menus ------------------------------------------------------------------

static GtkWidget *tool_button(const char *const *icons, const char *tooltip, const char *action)
{
    GtkWidget *b = gtk_button_new();
    GIcon *icon = g_themed_icon_new_from_names((char **)icons, -1);
    gtk_button_set_child(GTK_BUTTON(b), gtk_image_new_from_gicon(icon));
    g_object_unref(icon);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_focusable(b, FALSE);
    return b;
}

static GtkWidget *tool_toggle(const char *const *icons, const char *tooltip, const char *action)
{
    GtkWidget *b = gtk_toggle_button_new();
    GIcon *icon = g_themed_icon_new_from_names((char **)icons, -1);
    gtk_button_set_child(GTK_BUTTON(b), gtk_image_new_from_gicon(icon));
    g_object_unref(icon);
    gtk_widget_set_tooltip_text(b, tooltip);
    gtk_actionable_set_detailed_action_name(GTK_ACTIONABLE(b), action);
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_focusable(b, FALSE);
    return b;
}

static void history_menu_on_right_click(GtkWidget *button, MainWindow *w, bool back)
{
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_object_set_data(G_OBJECT(g), "beluga-back", GINT_TO_POINTER(back));
    g_signal_connect(g, "pressed", CB([](GtkGestureClick *g, int, double x, double y, gpointer p) {
        auto *w = static_cast<MainWindow *>(p);
        ViewContainer *c = w->activeContainer();
        if (!c) return;
        bool back = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g), "beluga-back"));
        GMenu *m = g_menu_new();
        for (auto &[idx, uri] : back ? c->backList() : c->forwardList()) {
            GMenuItem *mi = g_menu_item_new(display_location(uri).c_str(), nullptr);
            g_menu_item_set_action_and_target_value(mi, "win.go-history", g_variant_new_int32(idx));
            g_menu_append_item(m, mi);
            g_object_unref(mi);
        }
        if (g_menu_model_get_n_items(G_MENU_MODEL(m)) > 0)
            popup_menu(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g)), G_MENU_MODEL(m), x, y);
        g_object_unref(m);
    }), w);
    gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(g));
}

void MainWindow::buildToolbar()
{
    toolbar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(toolbar_, "toolbar");
    gtk_widget_add_css_class(toolbar_, "beluga-toolbar");
    static const char *back[] = {"go-previous", "go-previous-symbolic", nullptr};
    static const char *fwd[] = {"go-next", "go-next-symbolic", nullptr};
    static const char *up[] = {"go-up", "go-up-symbolic", nullptr};
    static const char *home[] = {"go-home", "user-home", nullptr};
    static const char *icons[] = {"view-list-icons", "view-grid-symbolic", nullptr};
    static const char *compact[] = {"view-list-details", "view-list-compact-symbolic", "view-list-symbolic", nullptr};
    static const char *details[] = {"view-list-tree", "view-list-symbolic", nullptr};
    static const char *columns[] = {"view-column", "view-file-columns", "view-columns-symbolic", "view-dual-symbolic", "view-list-symbolic", nullptr};
    static const char *split[] = {"view-split-left-right", "view-dual-symbolic", nullptr};
    static const char *search[] = {"edit-find", "system-search-symbolic", nullptr};
    static const char *filter[] = {"view-filter", "edit-find-replace", nullptr};
    static const char *menu[] = {"application-menu", "open-menu-symbolic", nullptr};
    static const char *sort[] = {"view-sort", "view-sort-ascending-symbolic", nullptr};

    backButton_ = tool_button(back, _("Back"), "win.back");
    forwardButton_ = tool_button(fwd, _("Forward"), "win.forward");
    history_menu_on_right_click(backButton_, this, true);
    history_menu_on_right_click(forwardButton_, this, false);
    gtk_box_append(GTK_BOX(toolbar_), backButton_);
    gtk_box_append(GTK_BOX(toolbar_), forwardButton_);
    gtk_box_append(GTK_BOX(toolbar_), tool_button(up, _("Up"), "win.up"));
    gtk_box_append(GTK_BOX(toolbar_), tool_button(home, _("Home"), "win.home"));
    gtk_box_append(GTK_BOX(toolbar_), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(icons, _("Icons"), "win.view-mode::icons"));
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(compact, _("Compact"), "win.view-mode::compact"));
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(details, _("Details"), "win.view-mode::details"));
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(columns, _("Columns"), "win.view-mode::columns"));

    GtkWidget *viewMenu = gtk_menu_button_new();
    GIcon *si = g_themed_icon_new_from_names((char **)sort, -1);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(viewMenu), gtk_image_new_from_gicon(si));
    g_object_unref(si);
    gtk_widget_set_tooltip_text(viewMenu, _("View Settings"));
    gtk_widget_add_css_class(viewMenu, "flat");
    gtk_widget_set_focusable(viewMenu, FALSE);
    g_object_set_data(G_OBJECT(toolbar_), "beluga-viewmenu", viewMenu);
    gtk_box_append(GTK_BOX(toolbar_), viewMenu);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar_), spacer);
    gtk_box_append(GTK_BOX(toolbar_), jobs_indicator_new());
    static const char *usage[] = {"disk-quota", "baobab", "org.gnome.baobab", "drive-harddisk", nullptr};
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(usage, _("Disk Usage"), "win.show-disk-usage"));
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(split, _("Split View"), "win.split"));
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(filter, _("Filter"), "win.toggle-filter"));
    gtk_box_append(GTK_BOX(toolbar_), tool_toggle(search, _("Search"), "win.toggle-search"));
    GtkWidget *ham = gtk_menu_button_new();
    GIcon *mi = g_themed_icon_new_from_names((char **)menu, -1);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(ham), gtk_image_new_from_gicon(mi));
    g_object_unref(mi);
    gtk_widget_set_tooltip_text(ham, _("Menu"));
    gtk_widget_add_css_class(ham, "flat");
    gtk_widget_set_focusable(ham, FALSE);
    g_object_set_data(G_OBJECT(toolbar_), "beluga-hamburger", ham);
    gtk_box_append(GTK_BOX(toolbar_), ham);
}

static void add(GMenu *m, const char *label, const char *action)
{
    g_menu_append(m, label, action);
}

static GMenu *section(GMenu *parent)
{
    GMenu *s = g_menu_new();
    g_menu_append_section(parent, nullptr, G_MENU_MODEL(s));
    g_object_unref(s);
    return s;
}

static GMenu *submenu(GMenu *parent, const char *label)
{
    GMenu *s = g_menu_new();
    g_menu_append_submenu(parent, label, G_MENU_MODEL(s));
    g_object_unref(s);
    return s;
}

GMenu *build_sort_menu();
GMenu *build_view_mode_menu();
GMenu *build_roles_menu();

GMenu *build_sort_menu()
{
    GMenu *m = g_menu_new();
    GMenu *roles = section(m);
    static const char *ids[] = {"text", "size", "diskusage", "modificationtime", "creationtime", "accesstime", "type", "extension", "deletiontime", nullptr};
    for (int i = 0; ids[i]; ++i) {
        GMenuItem *it = g_menu_item_new(role_title(ids[i]).c_str(), nullptr);
        g_menu_item_set_action_and_target_value(it, "win.sort-by", g_variant_new_string(ids[i]));
        g_menu_append_item(roles, it);
        g_object_unref(it);
    }
    GMenu *order = section(m);
    add(order, _("Descending"), "win.sort-descending");
    GMenu *more = section(m);
    add(more, _("Folders First"), "win.folders-first");
    add(more, _("Hidden Files Last"), "win.hidden-last");
    add(more, _("Show in Groups"), "win.show-groups");
    return m;
}

GMenu *build_view_mode_menu()
{
    GMenu *m = g_menu_new();
    add(m, _("Icons"), "win.view-mode::icons");
    add(m, _("Compact"), "win.view-mode::compact");
    add(m, _("Details"), "win.view-mode::details");
    add(m, _("Columns"), "win.view-mode::columns");
    return m;
}

GMenu *build_roles_menu()
{
    GMenu *m = g_menu_new();
    for (auto &r : all_roles()) {
        if (std::string(r.id) == "text") continue;
        std::string action = std::string("win.role-") + r.id;
        add(m, _(r.title), action.c_str());
    }
    return m;
}

void MainWindow::buildMenus()
{
    GMenu *bar = g_menu_new();

    GMenu *file = submenu(bar, _("_File"));
    GMenu *s = section(file);
    add(s, _("New _Window"), "win.new-window");
    add(s, _("New _Tab"), "win.new-tab");
    add(s, _("_Close Tab"), "win.close-tab");
    add(s, _("Undo Close Tab"), "win.undo-close-tab");
    s = section(file);
    GMenu *create = submenu(s, _("Create _New"));
    add(create, _("Folder…"), "win.create-folder");
    add(create, _("Text File…"), "win.create-file");
    add(create, _("Link to File or Folder…"), "win.create-link");
    add(s, _("Add to Places"), "win.add-to-places");
    s = section(file);
    add(s, _("_Rename…"), "win.rename");
    add(s, _("_Duplicate Here"), "win.duplicate");
    add(s, _("Move to _Trash"), "win.move-to-trash");
    add(s, _("_Delete"), "win.delete");
    add(s, _("Restore from Trash"), "win.restore");
    add(s, _("Empty Trash"), "win.empty-trash");
    s = section(file);
    add(s, _("Show Target"), "win.show-target");
    add(s, _("_Properties"), "win.properties");
    s = section(file);
    add(s, _("_Quit"), "win.quit");

    GMenu *edit = submenu(bar, _("_Edit"));
    s = section(edit);
    add(s, _("_Undo"), "win.undo");
    add(s, _("_Redo"), "win.redo");
    s = section(edit);
    add(s, _("Cu_t"), "win.cut");
    add(s, _("_Copy"), "win.copy");
    add(s, _("Copy Location"), "win.copy-location");
    add(s, _("_Paste"), "win.paste");
    s = section(edit);
    add(s, _("Filter…"), "win.filter");
    add(s, _("_Search…"), "win.find");
    s = section(edit);
    add(s, _("Select Files and Folders"), "win.selection-mode");
    add(s, _("Copy to Other View"), "win.copy-to-other-view");
    add(s, _("Move to Other View"), "win.move-to-other-view");
    add(s, _("Select _All"), "win.select-all");
    add(s, _("Invert Selection"), "win.invert-selection");

    GMenu *view = submenu(bar, _("_View"));
    s = section(view);
    add(s, _("Zoom _In"), "win.zoom-in");
    add(s, _("Reset Zoom Level"), "win.zoom-reset");
    add(s, _("Zoom _Out"), "win.zoom-out");
    s = section(view);
    GMenu *sortMenu = build_sort_menu();
    g_menu_append_submenu(s, _("Sort By"), G_MENU_MODEL(sortMenu));
    g_object_unref(sortMenu);
    GMenu *modeMenu = build_view_mode_menu();
    g_menu_append_submenu(s, _("View Mode"), G_MENU_MODEL(modeMenu));
    g_object_unref(modeMenu);
    GMenu *rolesMenu = build_roles_menu();
    g_menu_append_submenu(s, _("Show Additional Information"), G_MENU_MODEL(rolesMenu));
    g_object_unref(rolesMenu);
    add(s, _("Show Previews"), "win.show-previews");
    add(s, _("Show Hidden Files"), "win.show-hidden");
    add(s, _("Act as Administrator"), "win.act-as-admin");
    s = section(view);
    add(s, _("Remember View for This Folder"), "win.remember-view");
    add(s, _("Restore to Defaults"), "win.reset-view-props");
    add(s, _("Adjust View Display Style…"), "win.view-properties");
    s = section(view);
    add(s, _("Split View"), "win.split");
    add(s, _("Pop out Split View"), "win.popout-split");
    add(s, _("Split View to Tabs"), "win.split-to-tabs");
    add(s, _("Focus Other View"), "win.focus-other-view");
    add(s, _("Re_load"), "win.reload");
    add(s, _("Stop"), "win.stop");
    s = section(view);
    GMenu *panels = submenu(s, _("Panels"));
    add(panels, _("Places"), "win.show-places");
    add(panels, _("Folders"), "win.show-folders");
    add(panels, _("Information"), "win.show-info");
    add(panels, _("Disk Usage"), "win.show-disk-usage");
    add(panels, _("Focus Places Panel"), "win.focus-places");
    GMenu *loc = submenu(s, _("Location Bar"));
    add(loc, _("Editable Location"), "win.editable-location");
    add(loc, _("Replace Location"), "win.replace-location");
    s = section(view);
    add(s, _("Show _Menubar"), "win.show-menubar");
    add(s, _("Show _Toolbar"), "win.show-toolbar");
    add(s, _("Show _Statusbar"), "win.show-statusbar");

    GMenu *go = submenu(bar, _("_Go"));
    s = section(go);
    add(s, _("_Back"), "win.back");
    add(s, _("_Forward"), "win.forward");
    add(s, _("_Up"), "win.up");
    add(s, _("_Home"), "win.home");
    s = section(go);
    closedTabsMenu_ = g_menu_new();
    g_menu_append_submenu(s, _("Recently Closed Tabs"), G_MENU_MODEL(closedTabsMenu_));
    rebuildClosedTabsMenu();

    GMenu *tools = submenu(bar, _("_Tools"));
    s = section(tools);
    add(s, _("Open _Terminal"), "win.open-terminal");
    add(s, _("Open Terminal _Here"), "win.open-terminal-here");
    s = section(tools);
    add(s, _("_Disk Usage…"), "win.disk-usage");

    GMenu *set = submenu(bar, _("_Settings"));
    s = section(set);
    add(s, _("Show _Menubar"), "win.show-menubar");
    add(s, _("Show _Toolbar"), "win.show-toolbar");
    add(s, _("Show _Statusbar"), "win.show-statusbar");
    add(s, _("_Configure Beluga…"), "win.preferences");

    menu_ = G_MENU_MODEL(bar);

    // Hamburger: the frequent actions plus the full menus as submenus.
    GMenu *ham = g_menu_new();
    s = section(ham);
    add(s, _("New _Window"), "win.new-window");
    add(s, _("New _Tab"), "win.new-tab");
    s = section(ham);
    add(s, _("_Undo"), "win.undo");
    add(s, _("Select Files and Folders"), "win.selection-mode");
    add(s, _("Show Hidden Files"), "win.show-hidden");
    s = section(ham);
    g_menu_append_submenu(s, _("_File"), G_MENU_MODEL(file));
    g_menu_append_submenu(s, _("_Edit"), G_MENU_MODEL(edit));
    g_menu_append_submenu(s, _("_View"), G_MENU_MODEL(view));
    g_menu_append_submenu(s, _("_Go"), G_MENU_MODEL(go));
    g_menu_append_submenu(s, _("_Tools"), G_MENU_MODEL(tools));
    s = section(ham);
    add(s, _("Show _Menubar"), "win.show-menubar");
    add(s, _("_Configure Beluga…"), "win.preferences");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(g_object_get_data(G_OBJECT(toolbar_), "beluga-hamburger")), G_MENU_MODEL(ham));
    g_object_unref(ham);

    GMenu *vm = g_menu_new();
    s = section(vm);
    modeMenu = build_view_mode_menu();
    g_menu_append_section(vm, nullptr, G_MENU_MODEL(modeMenu));
    g_object_unref(modeMenu);
    sortMenu = build_sort_menu();
    g_menu_append_submenu(vm, _("Sort By"), G_MENU_MODEL(sortMenu));
    g_object_unref(sortMenu);
    rolesMenu = build_roles_menu();
    g_menu_append_submenu(vm, _("Show Additional Information"), G_MENU_MODEL(rolesMenu));
    g_object_unref(rolesMenu);
    s = section(vm);
    add(s, _("Show Previews"), "win.show-previews");
    add(s, _("Show Hidden Files"), "win.show-hidden");
    add(s, _("Show in Groups"), "win.show-groups");
    s = section(vm);
    add(s, _("Zoom _In"), "win.zoom-in");
    add(s, _("Zoom _Out"), "win.zoom-out");
    s = section(vm);
    add(s, _("Remember View for This Folder"), "win.remember-view");
    add(s, _("Adjust View Display Style…"), "win.view-properties");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(g_object_get_data(G_OBJECT(toolbar_), "beluga-viewmenu")), G_MENU_MODEL(vm));
    g_object_unref(vm);
}

void MainWindow::rebuildClosedTabsMenu()
{
    if (!closedTabsMenu_) return;
    g_menu_remove_all(closedTabsMenu_);
    for (int i = (int)closedTabs_.size() - 1; i >= 0; --i) {
        const ClosedTab &t = closedTabs_[i];
        std::string label = display_location(t.primary);
        if (!t.secondary.empty()) label += " | " + display_location(t.secondary);
        GMenuItem *mi = g_menu_item_new(label.c_str(), nullptr);
        g_menu_item_set_action_and_target_value(mi, "win.reopen-closed-tab", g_variant_new_int32(i));
        g_menu_append_item(closedTabsMenu_, mi);
        g_object_unref(mi);
    }
    if (closedTabs_.empty()) {
        GMenuItem *mi = g_menu_item_new(_("No recently closed tabs"), "win.nothing");
        g_menu_append_item(closedTabsMenu_, mi);
        g_object_unref(mi);
    }
}

// ---- tabs -----------------------------------------------------------------------------------

TabPage *MainWindow::openTab(const std::string &uri, const std::string &secondary, bool activate)
{
    auto tab = std::make_unique<TabPage>(this, uri, secondary);
    TabPage *t = tab.get();
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_));
    int pos = settings().openNewTabAfterLastTab || cur < 0 ? (int)tabs_.size() : cur + 1;
    tabs_.insert(tabs_.begin() + pos, std::move(tab));
    gtk_notebook_insert_page(GTK_NOTEBOOK(notebook_), t->widget(), t->label(), pos);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(notebook_), t->widget(), TRUE);
    updateTabBar();
    if (activate || tabs_.size() == 1) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), pos);
        onTabSwitched(pos);
        t->active()->view()->focus();
    }
    t->updateLabel();
    t->active()->view()->setCutUris(App::instance().cutUris());
    return t;
}

void MainWindow::openInNewTab(const std::string &uri, bool activate) { openTab(uri, {}, activate); }

void MainWindow::openInNewWindow(const std::string &uri) { App::instance().newWindow({uri}, false); }

int MainWindow::tabIndex(TabPage *t) const
{
    for (size_t i = 0; i < tabs_.size(); ++i)
        if (tabs_[i].get() == t) return (int)i;
    return -1;
}

void MainWindow::closeTab(int index, bool remember)
{
    if (index < 0 || index >= (int)tabs_.size()) return;
    if (tabs_.size() == 1) {
        gtk_window_close(GTK_WINDOW(win_));
        return;
    }
    TabPage *t = tabs_[index].get();
    if (remember) {
        closedTabs_.push_back({t->primary()->uri(), t->secondary() ? t->secondary()->uri() : std::string()});
        if (closedTabs_.size() > 10) closedTabs_.erase(closedTabs_.begin());
        rebuildClosedTabsMenu();
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(notebook_), index);
    tabs_.erase(tabs_.begin() + index);
    updateTabBar();
    onTabSwitched(gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_)));
}

void MainWindow::detachTab(int index)
{
    TabPage *t = tab(index);
    if (!t || tabs_.size() < 2) return;
    std::string a = t->primary()->uri(), b = t->secondary() ? t->secondary()->uri() : std::string();
    closeTab(index, false);
    MainWindow *w = App::instance().newWindow({a}, false);
    if (w && !b.empty() && w->activeTab()) w->activeTab()->setSplit(true, b);
}

TabPage *MainWindow::activeTab() const
{
    int cur = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_));
    return tab(cur);
}

ViewContainer *MainWindow::activeContainer() const
{
    TabPage *t = activeTab();
    return t ? t->active() : nullptr;
}

View *MainWindow::activeView() const
{
    ViewContainer *c = activeContainer();
    return c ? c->view() : nullptr;
}

void MainWindow::activateTab(int index)
{
    if (index < 0 || index >= (int)tabs_.size()) return;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(notebook_), index);
}

void MainWindow::onTabSwitched(int index)
{
    TabPage *t = tab(index);
    if (!t) return;
    updateTitle();
    syncPanels();
    updateActions();
}

void MainWindow::updateTabBar()
{
    gtk_notebook_set_show_tabs(GTK_NOTEBOOK(notebook_), tabs_.size() > 1 || settings().alwaysShowTabBar);
}

// ---- container events ----------------------------------------------------------------------

void MainWindow::containerActivated(ViewContainer *c)
{
    for (auto &t : tabs_)
        if (t->contains(c)) {
            if (t.get() != activeTab()) return;
            if (t->active() != c) {
                t->setActive(c);
                updateTitle();
                syncPanels();
                updateActions();
            }
            return;
        }
}

void MainWindow::containerUriChanged(ViewContainer *c)
{
    for (auto &t : tabs_)
        if (t->contains(c)) {
            t->updateLabel();
            c->view()->setCutUris(App::instance().cutUris());
            if (t.get() == activeTab() && t->active() == c) {
                updateTitle();
                syncPanels();
                updateActions();
            }
            return;
        }
}

void MainWindow::selectionChanged(ViewContainer *c)
{
    if (c != activeContainer()) return;
    updateActions();
    if (info_ && gtk_widget_get_visible(info_->widget()) && !duPaneOpen_) {
        auto items = c->view()->selectedItems();
        if (items.empty()) info_->showFolder(c->uri());
        else info_->showItems(items);
    }
}

void MainWindow::hoverChanged(ViewContainer *c)
{
    if (c != activeContainer() || !info_ || !gtk_widget_get_visible(info_->widget()) || duPaneOpen_) return;
    if (settings().infoShowHovered) info_->showHovered(c->view()->hoveredItem());
}

void MainWindow::syncPanels()
{
    ViewContainer *c = activeContainer();
    if (!c) return;
    places_->setCurrentUri(c->uri());
    if (gtk_widget_get_visible(folders_->widget()) && !is_virtual_uri(c->uri())) folders_->setUri(c->uri());
    if (duPaneOpen_) disk_usage_pane_show(duPane_, c->uri());
    if (gtk_widget_get_visible(info_->widget()) && !duPaneOpen_) {
        auto items = c->view()->selectedItems();
        if (items.empty()) info_->showFolder(c->uri());
        else info_->showItems(items);
    }
}

void MainWindow::updateTitle()
{
    ViewContainer *c = activeContainer();
    if (!c) return;
    std::string t = c->title() + (running_as_root() ? std::string(" — Beluga ") + _("(Administrator)") : std::string(" — Beluga"));
    gtk_window_set_title(GTK_WINDOW(win_), t.c_str());
}

void MainWindow::applySettings()
{
    const Settings &s = settings();
    gtk_widget_set_visible(menubar_, s.showMenuBar);
    gtk_widget_set_visible(toolbar_, s.showToolBar);
    gtk_widget_set_visible(places_->widget(), s.placesPanel);
    gtk_widget_set_visible(folders_->widget(), s.foldersPanel);
    gtk_widget_set_visible(leftPaned_, s.placesPanel || s.foldersPanel);
    if (s.placesPanel && s.foldersPanel)
        g_idle_add([](gpointer p) -> gboolean {
            auto *paned = GTK_PANED(p);
            int h = gtk_widget_get_height(GTK_WIDGET(paned));
            if (h > 0) gtk_paned_set_position(paned, std::max(120, h - settings().foldersPanelHeight));
            g_object_unref(paned);
            return G_SOURCE_REMOVE;
        }, g_object_ref(leftPaned_));
    gtk_widget_set_visible(info_->widget(), s.infoPanel);
    layoutRight();
    updateTabBar();
    for (auto &t : tabs_) t->updateLabel();
    updateTitle();
    syncPanels();
    updateActions();
}

// Shows the right side for the Information panel or the Disk Usage pane, at that one's width.
void MainWindow::layoutRight()
{
    const Settings &s = settings();
    bool show = s.infoPanel || duPaneOpen_;
    gtk_widget_set_visible(duPane_, duPaneOpen_);
    gtk_stack_set_visible_child(GTK_STACK(rightStack_), duPaneOpen_ ? duPane_ : info_->widget());
    gtk_widget_set_visible(rightStack_, show);
    if (!show) return;
    int width = duPaneOpen_ ? s.duPaneWidth : s.infoPanelWidth;
    int w = gtk_widget_get_width(innerPaned_);
    if (w > 0) {
        gtk_paned_set_position(GTK_PANED(innerPaned_), std::max(200, w - width));
        return;
    }
    g_object_set_data(G_OBJECT(innerPaned_), "beluga-width", GINT_TO_POINTER(width));
    g_idle_add([](gpointer p) -> gboolean {
        auto *paned = GTK_PANED(p);
        int w = gtk_widget_get_width(GTK_WIDGET(paned));
        int width = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(paned), "beluga-width"));
        if (w > 0) gtk_paned_set_position(paned, std::max(200, w - width));
        g_object_unref(paned);
        return G_SOURCE_REMOVE;
    }, g_object_ref(innerPaned_));
}

void MainWindow::toggleDiskUsagePane()
{
    Settings &s = settings();
    if (duPaneOpen_ && gtk_widget_get_width(innerPaned_) > 0)
        s.duPaneWidth = std::max(200, gtk_widget_get_width(innerPaned_) - gtk_paned_get_position(GTK_PANED(innerPaned_)));
    duPaneOpen_ = !duPaneOpen_;
    layoutRight();
    syncPanels();
    updateActions();
}

void MainWindow::focusPlaces()
{
    places_->focus();
}

void MainWindow::updateCutState()
{
    for (auto &t : tabs_) {
        t->primary()->view()->setCutUris(App::instance().cutUris());
        if (t->secondary()) t->secondary()->view()->setCutUris(App::instance().cutUris());
    }
}

// ---- opening ------------------------------------------------------------------------------------

void MainWindow::openItems(ViewContainer *c, const std::vector<BelugaItem *> &items, GdkModifierType mods)
{
    std::vector<std::string> dirs;
    std::vector<OpenTarget> files;
    for (BelugaItem *it : items) {
        const ItemData &d = item_data(it);
        bool mountable = d.info && g_file_info_get_file_type(d.info.get()) == G_FILE_TYPE_MOUNTABLE;
        // An archive opens as a folder (zip, tar, 7z, rar, ISO) when that is set, or when no program is set up
        // for it; otherwise it opens in its program, and Open in Beluga browses it.
        if (!d.isDir && archive::can_browse(d.mime, d.name)
            && (settings().browseArchives || !Ref<GAppInfo>(g_app_info_get_default_for_type(d.mime.c_str(), FALSE)))) {
            std::string path = take(g_file_get_path(d.file.get()));
            if (!path.empty()) {
                dirs.push_back(archive::make_uri(path, ""));
                continue;
            }
        }
        if (d.isDir || mountable) {
            std::string target = d.uri;
            if (mountable) {
                const char *t = g_file_info_get_attribute_string(d.info.get(), G_FILE_ATTRIBUTE_STANDARD_TARGET_URI);
                if (t) target = t;
            }
            dirs.push_back(target);
        } else {
            files.push_back({d.file, d.mime, is_executable_file(d.info.get())});
        }
    }
    auto openDirs = [this, c, dirs, mods] {
        if (dirs.size() == 1 && !(mods & GDK_CONTROL_MASK)) {
            if (mods & GDK_SHIFT_MASK) openInNewWindow(dirs[0]);
            else c->navigate(dirs[0]);
            return;
        }
        for (auto &d : dirs) openInNewTab(d, false);
    };
    if (dirs.size() > 10 && settings().confirmOpenManyFolders) {
        ask(win_, printf_str(_("Open %zu folders in new tabs?"), dirs.size()), {}, {_("Cancel"), _("Open Folders")}, 1, 0,
            [openDirs](int r) { if (r == 1) openDirs(); });
    } else if (!dirs.empty()) {
        openDirs();
    }
    // Files inside an archive open as a copy extracted to the cache folder.
    std::vector<std::string> inArchive;
    for (auto it = files.begin(); it != files.end();) {
        std::string u = file_uri(it->file.get());
        if (archive::is_archive_uri(u)) {
            inArchive.push_back(u);
            it = files.erase(it);
        } else {
            ++it;
        }
    }
    if (!files.empty()) open_files(win_, files);
    for (auto &u : inArchive) {
        std::thread([this, alive = lifeToken(), u] {
            auto err = std::make_shared<std::string>();
            auto path = std::make_shared<std::string>(archive::extract_to_cache(u, err.get()));
            run_on_main([this, alive, u, path, err] {
                if (alive.expired()) return;
                if (path->empty()) {
                    show_error(win_, printf_str(_("Could not open “%s”."), basename_of(archive::display_path(u)).c_str()), *err);
                    return;
                }
                Ref<GFile> f(g_file_new_for_path(path->c_str()));
                std::string ct = take(g_content_type_guess(path->c_str(), nullptr, 0, nullptr));
                // Never start programs or desktop entries straight out of an archive: show them as text.
                if (ct == "application/x-desktop" || g_content_type_can_be_executable(ct.c_str())) ct = "text/plain";
                open_files(win_, {{f, ct, false}});
            });
        }).detach();
    }
}

void MainWindow::openTerminal(const std::string &folder)
{
    GError *err = nullptr;
    if (!terminal::open(folder, &err)) {
        show_error(win_, _("Could not open a terminal."), err ? err->message : "");
        g_clear_error(&err);
    }
}

void MainWindow::showProperties(const std::vector<Ref<GFile>> &files)
{
    if (!files.empty()) show_properties_dialog(win_, files);
}

// ---- drag and drop ------------------------------------------------------------------------------

void MainWindow::dropFiles(GFile *dest, const std::vector<Ref<GFile>> &files, GdkDragAction action)
{
    if (files.empty() || !action) return;
    // Out of an archive, files can only be copied (extracted).
    for (auto &f : files)
        if (archive::is_archive_uri(file_uri(f.get()))) {
            if (!dest) return;
            action = GDK_ACTION_COPY;
            break;
        }
    FileJob::Ptr job;
    if (!dest) job = FileJob::trash(files);
    else if (action == GDK_ACTION_MOVE) job = FileJob::move(files, dest);
    else if (action == GDK_ACTION_LINK) job = FileJob::link(files, dest);
    else job = FileJob::copy(files, dest);
    std::string destUri = dest ? file_uri(dest) : std::string();
    job->onFinished = [this, alive = lifeToken(), destUri](FileJob &j) {
        if (alive.expired()) return;
        View *v = activeView();
        if (!v || destUri.empty() || v->location() != destUri) return;
        std::vector<std::string> uris;
        for (auto &it : j.done())
            if (it.dest) uris.push_back(file_uri(it.dest.get()));
        v->selectUris(uris);
    };
    job->start();
}

void MainWindow::handleDrop(GFile *dest, GdkFileList *list, GdkModifierType mods, GtkWidget *w, double x, double y)
{
    std::vector<Ref<GFile>> files;
    GSList *l = gdk_file_list_get_files(list);
    for (GSList *i = l; i; i = i->next) files.push_back(Ref<GFile>::share(G_FILE(i->data)));
    g_slist_free(l);
    if (files.empty()) return;
    if (!dest) {
        dropFiles(nullptr, files, GDK_ACTION_MOVE);
        return;
    }
    bool allHere = true;
    for (auto &f : files) {
        Ref<GFile> parent(g_file_get_parent(f.get()));
        if (!parent || !g_file_equal(parent.get(), dest)) allHere = false;
        if (g_file_equal(f.get(), dest)) return;   // onto itself
    }
    if (allHere && !(mods & GDK_CONTROL_MASK)) return;
    Ref<GFile> d = Ref<GFile>::share(dest);
    if ((mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK)) dropFiles(dest, files, GDK_ACTION_LINK);
    else if (mods & GDK_CONTROL_MASK) dropFiles(dest, files, GDK_ACTION_COPY);
    else if (mods & GDK_SHIFT_MASK) dropFiles(dest, files, GDK_ACTION_MOVE);
    else
        show_drop_menu(w, x, y, !allHere, [this, d, files](GdkDragAction a) {
            if (a) dropFiles(d.get(), files, a);
        });
}

// ---- session ------------------------------------------------------------------------------------

void MainWindow::saveSession(GKeyFile *kf, const std::string &group) const
{
    const char *G = group.c_str();
    g_key_file_set_integer(kf, G, "Tabs", (int)tabs_.size());
    g_key_file_set_integer(kf, G, "Active", gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_)));
    for (size_t i = 0; i < tabs_.size(); ++i) {
        TabPage *t = tabs_[i].get();
        g_key_file_set_string(kf, G, printf_str("Tab%zuPrimary", i).c_str(), t->primary()->uri().c_str());
        g_key_file_set_string(kf, G, printf_str("Tab%zuSecondary", i).c_str(), t->secondary() ? t->secondary()->uri().c_str() : "");
        g_key_file_set_boolean(kf, G, printf_str("Tab%zuSecondaryActive", i).c_str(), t->secondary() && t->active() == t->secondary());
    }
}

bool MainWindow::restoreSession(GKeyFile *kf, const std::string &group)
{
    const char *G = group.c_str();
    int n = g_key_file_get_integer(kf, G, "Tabs", nullptr);
    for (int i = 0; i < n; ++i) {
        std::string a = take(g_key_file_get_string(kf, G, printf_str("Tab%dPrimary", i).c_str(), nullptr));
        std::string b = take(g_key_file_get_string(kf, G, printf_str("Tab%dSecondary", i).c_str(), nullptr));
        if (a.empty()) continue;
        TabPage *t = openTab(a, b, false);
        if (t && t->secondary() && g_key_file_get_boolean(kf, G, printf_str("Tab%dSecondaryActive", i).c_str(), nullptr))
            t->setActive(t->secondary());
    }
    if (tabs_.empty()) return false;
    int active = g_key_file_get_integer(kf, G, "Active", nullptr);
    activateTab(std::clamp(active, 0, (int)tabs_.size() - 1));
    onTabSwitched(gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook_)));
    return true;
}

} // namespace beluga
