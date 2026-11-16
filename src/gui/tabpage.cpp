// SPDX-License-Identifier: GPL-2.0-or-later
#include "tabpage.hpp"

#include <algorithm>

#include "dialogs.hpp"
#include "gtkutil.hpp"
#include "mainwindow.hpp"
#include "places.hpp"
#include "settings.hpp"

namespace beluga {

TabPage::TabPage(MainWindow *win, const std::string &uri, const std::string &secondary) : win_(win)
{
    root_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g_object_ref_sink(root_);
    g_object_set_data(G_OBJECT(root_), "beluga-tab", this);
    paned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_wide_handle(GTK_PANED(paned_), FALSE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned_), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned_), FALSE);
    gtk_widget_set_hexpand(paned_, TRUE);
    gtk_box_append(GTK_BOX(root_), paned_);

    label_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_object_ref_sink(label_);
    labelIcon_ = gtk_image_new_from_icon_name("folder");
    labelText_ = gtk_label_new(nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(labelText_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(labelText_), 24);
    gtk_label_set_width_chars(GTK_LABEL(labelText_), 6);
    gtk_box_append(GTK_BOX(label_), labelIcon_);
    gtk_box_append(GTK_BOX(label_), labelText_);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_add_css_class(close, "tab-close");
    gtk_widget_set_focusable(close, FALSE);
    gtk_widget_set_tooltip_text(close, _("Close Tab"));
    gtk_widget_set_visible(close, settings().showCloseButtonOnTabs);
    g_object_set_data(G_OBJECT(label_), "beluga-close", close);
    g_signal_connect_swapped(close, "clicked", CB([](TabPage *t) { t->win_->closeTab(t->win_->tabIndex(t)); }), this);
    gtk_box_append(GTK_BOX(label_), close);

    // Middle click closes, right click shows the tab menu.
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", CB([](GtkGestureClick *g, int, double x, double y, gpointer p) {
        auto *t = static_cast<TabPage *>(p);
        guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
        int idx = t->win_->tabIndex(t);
        if (b == GDK_BUTTON_MIDDLE) {
            t->win_->closeTab(idx);
        } else if (b == GDK_BUTTON_SECONDARY) {
            GMenu *m = g_menu_new();
            GMenu *s1 = g_menu_new();
            auto item = [&](GMenu *menu, const char *label, const char *action) {
                GMenuItem *mi = g_menu_item_new(label, nullptr);
                g_menu_item_set_action_and_target_value(mi, action, g_variant_new_int32(idx));
                g_menu_append_item(menu, mi);
                g_object_unref(mi);
            };
            g_menu_append(s1, _("New Tab"), "win.new-tab");
            item(s1, _("Duplicate Tab"), "win.tab-duplicate");
            item(s1, _("Detach Tab"), "win.tab-detach");
            GMenu *s2 = g_menu_new();
            item(s2, _("Close Other Tabs"), "win.tab-close-others");
            item(s2, _("Close Tab"), "win.tab-close");
            g_menu_append_section(m, nullptr, G_MENU_MODEL(s1));
            g_menu_append_section(m, nullptr, G_MENU_MODEL(s2));
            popup_menu(t->label_, G_MENU_MODEL(m), x, y);
            g_object_unref(s1);
            g_object_unref(s2);
            g_object_unref(m);
        }
    }), this);
    gtk_widget_add_controller(label_, GTK_EVENT_CONTROLLER(click));

    // Dragging files over a tab switches to it; dropping copies or moves into it.
    GtkDropControllerMotion *dm = GTK_DROP_CONTROLLER_MOTION(gtk_drop_controller_motion_new());
    g_signal_connect(dm, "enter", CB([](GtkDropControllerMotion *, double, double, gpointer p) {
        auto *t = static_cast<TabPage *>(p);
        g_timeout_add(500, [](gpointer q) -> gboolean {
            auto *root = GTK_WIDGET(q);
            if (auto *tp = static_cast<TabPage *>(g_object_get_data(G_OBJECT(root), "beluga-tab")))
                if (tp->win_) tp->win_->activateTab(tp->win_->tabIndex(tp));
            g_object_unref(root);
            return G_SOURCE_REMOVE;
        }, g_object_ref(t->root_));
    }), this);
    gtk_widget_add_controller(label_, GTK_EVENT_CONTROLLER(dm));
    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GdkDragAction(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
    g_signal_connect(dt, "drop", CB([](GtkDropTarget *d, const GValue *v, double x, double y, gpointer p) -> gboolean {
        auto *t = static_cast<TabPage *>(p);
        if (!t->active_ || t->active_->view()->isVirtual()) return FALSE;
        Ref<GFile> dest = t->active_->view()->locationFile();
        t->win_->handleDrop(dest.get(), static_cast<GdkFileList *>(g_value_get_boxed(v)),
                            gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(d)), t->label_, x, y);
        return TRUE;
    }), this);
    gtk_widget_add_controller(label_, GTK_EVENT_CONTROLLER(dt));

    primary_ = std::make_unique<ViewContainer>(win, uri);
    active_ = primary_.get();
    if (!secondary.empty()) secondary_ = std::make_unique<ViewContainer>(win, secondary);
    attach();
    setActive(primary_.get());
}

TabPage::~TabPage()
{
    g_object_set_data(G_OBJECT(root_), "beluga-tab", nullptr);
    gtk_paned_set_start_child(GTK_PANED(paned_), nullptr);
    gtk_paned_set_end_child(GTK_PANED(paned_), nullptr);
    secondary_.reset();
    primary_.reset();
    g_object_unref(label_);
    g_object_unref(root_);
}

void TabPage::attach()
{
    gtk_paned_set_start_child(GTK_PANED(paned_), nullptr);
    gtk_paned_set_end_child(GTK_PANED(paned_), nullptr);
    gtk_paned_set_start_child(GTK_PANED(paned_), primary_->widget());
    if (secondary_) {
        gtk_paned_set_end_child(GTK_PANED(paned_), secondary_->widget());
        // Even split once the paned has a size.
        g_idle_add([](gpointer p) -> gboolean {
            auto *paned = GTK_PANED(p);
            int w = gtk_widget_get_width(GTK_WIDGET(paned));
            if (w > 0) gtk_paned_set_position(paned, w / 2);
            g_object_unref(paned);
            return G_SOURCE_REMOVE;
        }, g_object_ref(paned_));
    }
}

ViewContainer *TabPage::other(ViewContainer *c) const
{
    if (!secondary_) return nullptr;
    return c == primary_.get() ? secondary_.get() : primary_.get();
}

void TabPage::setActive(ViewContainer *c)
{
    if (!contains(c)) return;
    active_ = c;
    primary_->setActive(c == primary_.get());
    if (secondary_) secondary_->setActive(c == secondary_.get());
    updateLabel();
}

void TabPage::setSplit(bool on, const std::string &uri)
{
    if (on == split()) return;
    if (on) {
        secondary_ = std::make_unique<ViewContainer>(win_, uri.empty() ? active_->uri() : uri);
        attach();
        setActive(secondary_.get());
        secondary_->view()->focus();
    } else {
        ViewContainer *close = active_;
        int choice = settings().closeSplitViewChoice;
        if (choice == 1) close = other(active_);
        else if (choice == 2) close = secondary_.get();
        closeView(close);
    }
}

void TabPage::closeView(ViewContainer *c)
{
    if (!secondary_ || !contains(c)) return;
    if (c == primary_.get()) {
        gtk_paned_set_start_child(GTK_PANED(paned_), nullptr);
        primary_ = std::move(secondary_);
    } else {
        gtk_paned_set_end_child(GTK_PANED(paned_), nullptr);
    }
    secondary_.reset();
    attach();
    setActive(primary_.get());
    primary_->view()->focus();
}

void TabPage::updateLabel()
{
    if (!active_) return;
    std::string title = active_->title();
    if (secondary_) title = primary_->title() + " | " + secondary_->title();
    gtk_label_set_text(GTK_LABEL(labelText_), title.c_str());
    gtk_label_set_width_chars(GTK_LABEL(labelText_), std::min<int>(24, std::max<int>(4, g_utf8_strlen(title.c_str(), -1))));
    gtk_widget_set_tooltip_text(label_, display_location(active_->uri()).c_str());
    std::string icon = Places::iconForUri(active_->uri());
    gtk_image_set_from_icon_name(GTK_IMAGE(labelIcon_), icon.c_str());
    if (auto *close = GTK_WIDGET(g_object_get_data(G_OBJECT(label_), "beluga-close")))
        gtk_widget_set_visible(close, settings().showCloseButtonOnTabs);
}

} // namespace beluga
