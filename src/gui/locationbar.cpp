// SPDX-License-Identifier: GPL-2.0-or-later
#include "locationbar.hpp"

#include "gtkutil.hpp"
#include "places.hpp"
#include "search.hpp"
#include "settings.hpp"

#include <algorithm>

namespace beluga {

LocationBar::LocationBar()
{
    root_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g_object_ref_sink(root_);
    g_object_set_data(G_OBJECT(root_), "beluga-bar", this);
    gtk_widget_add_css_class(root_, "beluga-location");
    stack_ = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack_), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_hexpand(stack_, TRUE);
    gtk_box_append(GTK_BOX(root_), stack_);

    // Breadcrumb page: scrollable crumbs + empty area that switches to editing.
    GtkWidget *crumbPage = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    crumbScroll_ = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(crumbScroll_), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(crumbScroll_), TRUE);
    gtk_widget_set_hexpand(crumbScroll_, FALSE);
    crumbBox_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(crumbBox_, "crumbs");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(crumbScroll_), crumbBox_);
    // Keep the end of a long path (the current folder) visible whenever the crumbs or the
    // width change; scrolling by hand only changes the value, which this leaves alone.
    g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(crumbScroll_)), "changed",
                     CB([](GtkAdjustment *a) {
                         // Not during the allocation that emitted "changed": the viewport would not move.
                         g_idle_add([](gpointer p) -> gboolean {
                             auto *adj = GTK_ADJUSTMENT(p);
                             gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
                             g_object_unref(adj);
                             return G_SOURCE_REMOVE;
                         }, g_object_ref(a));
                     }), nullptr);
    gtk_box_append(GTK_BOX(crumbPage), crumbScroll_);
    GtkWidget *filler = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(filler, TRUE);
    gtk_widget_add_css_class(filler, "crumb-filler");
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect_swapped(click, "released", CB([](LocationBar *b) {
        if (b->onActivated) b->onActivated();
        b->startEditing(true);
    }), this);
    gtk_widget_add_controller(filler, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(crumbPage), filler);
    gtk_stack_add_named(GTK_STACK(stack_), crumbPage, "crumbs");

    entry_ = gtk_entry_new();
    gtk_widget_set_hexpand(entry_, TRUE);
    gtk_entry_set_input_purpose(GTK_ENTRY(entry_), GTK_INPUT_PURPOSE_URL);
    gtk_stack_add_named(GTK_STACK(stack_), entry_, "edit");
    g_signal_connect_swapped(entry_, "activate", CB([](LocationBar *b) { b->commit(); }), this);
    g_signal_connect_swapped(entry_, "changed", CB([](LocationBar *b) {
        if (!b->settingText_ && gtk_widget_has_focus(b->entry_)) b->updateCompletion();
    }), this);
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer p) -> gboolean {
        auto *b = static_cast<LocationBar *>(p);
        if (b->completionKey(keyval)) return TRUE;
        if (keyval == GDK_KEY_Escape) {
            b->hideCompletion();
            b->settingText_ = true;
            gtk_editable_set_text(GTK_EDITABLE(b->entry_), display_location(b->uri_).c_str());
            b->settingText_ = false;
            if (!b->alwaysEditable_) b->stopEditing();
            if (b->onEditingFinished) b->onEditingFinished();
            return TRUE;
        }
        return FALSE;
    }), this);
    gtk_widget_add_controller(entry_, key);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus, "leave", CB([](LocationBar *b) {
        // Checked a little later: focus may move into the completion list.
        g_timeout_add(150, [](gpointer p) -> gboolean {
            auto *root = GTK_WIDGET(p);
            auto *b = static_cast<LocationBar *>(g_object_get_data(G_OBJECT(root), "beluga-bar"));
            if (b) {
                GtkRoot *win = gtk_widget_get_root(root);
                GtkWidget *f = win ? gtk_root_get_focus(win) : nullptr;
                bool inside = f && (f == b->entry_ || gtk_widget_is_ancestor(f, b->entry_));
                if (!inside) {
                    b->hideCompletion();
                    if (!b->alwaysEditable_ && b->editing_) b->stopEditing();
                }
            }
            g_object_unref(root);
            return G_SOURCE_REMOVE;
        }, g_object_ref(b->root_));
    }), this);
    gtk_widget_add_controller(entry_, focus);
    GtkGesture *anyClick = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(anyClick), GTK_PHASE_CAPTURE);
    g_signal_connect_swapped(anyClick, "pressed", CB([](LocationBar *b) { if (b->onActivated) b->onActivated(); }), this);
    gtk_widget_add_controller(root_, GTK_EVENT_CONTROLLER(anyClick));

    spinner_ = gtk_spinner_new();
    gtk_widget_set_visible(spinner_, FALSE);
    gtk_widget_set_margin_start(spinner_, 4);
    gtk_widget_set_margin_end(spinner_, 4);
    gtk_box_append(GTK_BOX(root_), spinner_);

    alwaysEditable_ = settings().editableUrl;
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), alwaysEditable_ ? "edit" : "crumbs");
    editing_ = alwaysEditable_;
}

LocationBar::~LocationBar()
{
    g_object_set_data(G_OBJECT(root_), "beluga-bar", nullptr);
    if (completionPop_) gtk_widget_unparent(completionPop_);
    g_object_unref(root_);
}

void LocationBar::setUri(const std::string &uri)
{
    uri_ = uri;
    settingText_ = true;
    gtk_editable_set_text(GTK_EDITABLE(entry_), is_virtual_uri(uri) ? uri.c_str() : display_location(uri).c_str());
    settingText_ = false;
    rebuild();
}

void LocationBar::setEditable(bool on)
{
    alwaysEditable_ = on;
    if (on) startEditing(false);
    else stopEditing();
}

void LocationBar::startEditing(bool selectAll)
{
    editing_ = true;
    gtk_stack_set_visible_child_name(GTK_STACK(stack_), "edit");
    gtk_widget_grab_focus(entry_);
    if (selectAll) gtk_editable_select_region(GTK_EDITABLE(entry_), 0, -1);
    else gtk_editable_set_position(GTK_EDITABLE(entry_), -1);
}

void LocationBar::stopEditing()
{
    hideCompletion();
    editing_ = alwaysEditable_;
    if (!alwaysEditable_) gtk_stack_set_visible_child_name(GTK_STACK(stack_), "crumbs");
}

void LocationBar::setActive(bool active)
{
    if (active) gtk_widget_remove_css_class(root_, "inactive");
    else gtk_widget_add_css_class(root_, "inactive");
}

void LocationBar::setLoading(bool loading)
{
    gtk_widget_set_visible(spinner_, loading);
    if (loading) gtk_spinner_start(GTK_SPINNER(spinner_));
    else gtk_spinner_stop(GTK_SPINNER(spinner_));
}

void LocationBar::navigateTo(const std::string &uri)
{
    if (onActivated) onActivated();
    if (onNavigate) onNavigate(uri);
}

void LocationBar::commit()
{
    hideCompletion();
    std::string text = trim(gtk_editable_get_text(GTK_EDITABLE(entry_)));
    if (text.empty()) return;
    std::string target;
    if (is_virtual_uri(text) || starts_with(text, "network:")) {
        target = text;
    } else {
        Ref<GFile> base;
        if (!is_virtual_uri(uri_)) base = Ref<GFile>(g_file_new_for_uri(uri_.c_str()));
        Ref<GFile> f(file_for_input(text, base.get()));
        if (f) target = file_uri(f.get());
    }
    if (!alwaysEditable_) stopEditing();
    if (!target.empty()) navigateTo(target);
    if (onEditingFinished) onEditingFinished();
}

std::vector<LocationBar::Crumb> LocationBar::crumbs() const
{
    std::vector<Crumb> out;
    if (is_trash_uri(uri_)) return {{_("Trash"), "trash:///", "user-trash"}};
    if (is_recent_uri(uri_)) return {{_("Recent Files"), "recent:///", "document-open-recent"}};
    if (is_search_uri(uri_)) {
        SearchQuery q;
        SearchQuery::fromUri(uri_, q);
        return {{q.title(), uri_, "system-search"}};
    }
    Ref<GFile> f(g_file_new_for_uri(uri_.c_str()));
    std::vector<Ref<GFile>> chain;
    for (Ref<GFile> cur = f; cur; cur = Ref<GFile>(g_file_get_parent(cur.get()))) chain.push_back(cur);
    std::reverse(chain.begin(), chain.end());
    size_t start = 0;
    Crumb root;
    int pi = settings().showFullPath ? -1 : Places::instance().bestMatch(uri_);
    if (pi >= 0) {
        const Place &p = Places::instance().list()[pi];
        Ref<GFile> pf(g_file_new_for_uri(p.uri.c_str()));
        for (size_t i = 0; i < chain.size(); ++i)
            if (g_file_equal(chain[i].get(), pf.get())) {
                start = i;
                root = {p.label, file_uri(chain[i].get()), p.icon};
                break;
            }
    }
    if (root.uri.empty()) {
        start = 0;
        std::string rootPath = display_location(chain[0].get());
        root = {rootPath == "/" ? std::string("/") : rootPath, file_uri(chain[0].get()), g_file_is_native(chain[0].get()) ? "drive-harddisk" : "folder-remote"};
    }
    out.push_back(root);
    for (size_t i = start + 1; i < chain.size(); ++i) {
        char *bn = g_file_get_basename(chain[i].get());
        GFileInfo *info = g_file_query_info(chain[i].get(), G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
        std::string label = info ? str(g_file_info_get_display_name(info)) : take(bn);
        if (info) g_object_unref(info);
        if (info) g_free(bn);
        out.push_back({label, file_uri(chain[i].get()), ""});
    }
    return out;
}

GtkWidget *LocationBar::subfolderButton(const std::string &dirUri)
{
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(mb), "pan-end-symbolic");
    gtk_widget_add_css_class(mb, "flat");
    gtk_widget_add_css_class(mb, "crumb-arrow");
    gtk_widget_set_focusable(mb, FALSE);
    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
    g_object_set_data_full(G_OBJECT(pop), "beluga-uri", g_strdup(dirUri.c_str()), g_free);
    g_signal_connect(pop, "show", CB([](GtkPopover *pop, gpointer p) {
        auto *b = static_cast<LocationBar *>(p);
        std::string uri = static_cast<const char *>(g_object_get_data(G_OBJECT(pop), "beluga-uri"));
        bool hidden = b->showHidden && b->showHidden();
        std::vector<std::pair<std::string, std::string>> dirs;
        Ref<GFile> dir(g_file_new_for_uri(uri.c_str()));
        GFileEnumerator *en = g_file_enumerate_children(dir.get(), "standard::name,standard::display-name,standard::type,standard::is-hidden",
                                                        G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
        if (en) {
            GFileInfo *info;
            while ((info = g_file_enumerator_next_file(en, nullptr, nullptr)) && dirs.size() < 1000) {
                if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY && (hidden || !g_file_info_get_is_hidden(info))) {
                    Ref<GFile> c(g_file_get_child(dir.get(), g_file_info_get_name(info)));
                    dirs.emplace_back(g_file_info_get_display_name(info), file_uri(c.get()));
                }
                g_object_unref(info);
            }
            g_object_unref(en);
        }
        std::sort(dirs.begin(), dirs.end(), [](auto &a, auto &b) {
            return collate_key(a.first, 0) < collate_key(b.first, 0);
        });
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 420);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
        GtkWidget *list = gtk_list_box_new();
        gtk_widget_add_css_class(list, "navigation-sidebar");
        if (dirs.empty()) {
            GtkWidget *l = gtk_label_new(_("No subfolders"));
            gtk_widget_add_css_class(l, "dim-label");
            gtk_list_box_append(GTK_LIST_BOX(list), l);
        }
        for (auto &[name, u] : dirs) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(row), gtk_image_new_from_icon_name("folder"));
            GtkWidget *l = gtk_label_new(name.c_str());
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
            gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
            gtk_box_append(GTK_BOX(row), l);
            g_object_set_data_full(G_OBJECT(row), "beluga-uri", g_strdup(u.c_str()), g_free);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
        g_object_set_data(G_OBJECT(list), "beluga-pop", pop);
        g_signal_connect(list, "row-activated", CB([](GtkListBox *l, GtkListBoxRow *row, gpointer p) {
            auto *b = static_cast<LocationBar *>(p);
            GtkWidget *child = gtk_list_box_row_get_child(row);
            auto *u = static_cast<const char *>(g_object_get_data(G_OBJECT(child), "beluga-uri"));
            gtk_popover_popdown(GTK_POPOVER(g_object_get_data(G_OBJECT(l), "beluga-pop")));
            if (u) b->navigateTo(u);
        }), b);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
        gtk_popover_set_child(pop, scroll);
    }), this);
    return mb;
}

void LocationBar::rebuild()
{
    while (GtkWidget *c = gtk_widget_get_first_child(crumbBox_)) gtk_box_remove(GTK_BOX(crumbBox_), c);
    auto list = crumbs();
    for (size_t i = 0; i < list.size(); ++i) {
        const Crumb &c = list[i];
        GtkWidget *b = gtk_button_new();
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_add_css_class(b, "crumb");
        gtk_widget_set_focusable(b, FALSE);
        if (i + 1 == list.size()) gtk_widget_add_css_class(b, "current");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        if (!c.icon.empty()) gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(c.icon.c_str()));
        GtkWidget *l = gtk_label_new(c.label.c_str());
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
        // A minimum width, so a narrow view scrolls the path (to its end, below) instead of
        // shrinking every crumb to "…".
        glong len = g_utf8_strlen(c.label.c_str(), -1), keep = i + 1 == list.size() ? 16 : 10;
        if (len <= keep) gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_NONE);
        else gtk_label_set_width_chars(GTK_LABEL(l), (int)keep);
        gtk_box_append(GTK_BOX(box), l);
        gtk_button_set_child(GTK_BUTTON(b), box);
        g_object_set_data_full(G_OBJECT(b), "beluga-uri", g_strdup(c.uri.c_str()), g_free);
        g_signal_connect(b, "clicked", CB([](GtkButton *btn, gpointer p) {
            static_cast<LocationBar *>(p)->navigateTo(static_cast<const char *>(g_object_get_data(G_OBJECT(btn), "beluga-uri")));
        }), this);
        GtkGesture *mid = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mid), GDK_BUTTON_MIDDLE);
        g_signal_connect(mid, "released", CB([](GtkGestureClick *g, int, double, double, gpointer p) {
            auto *b = static_cast<LocationBar *>(p);
            GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
            if (b->onOpenInNewTab) b->onOpenInNewTab(static_cast<const char *>(g_object_get_data(G_OBJECT(w), "beluga-uri")));
        }), this);
        gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(mid));
        if (!is_virtual_uri(c.uri)) {
            GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GdkDragAction(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
            g_signal_connect(dt, "drop", CB([](GtkDropTarget *t, const GValue *v, double x, double y, gpointer p) -> gboolean {
                auto *b = static_cast<LocationBar *>(p);
                GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(t));
                Ref<GFile> dest(g_file_new_for_uri(static_cast<const char *>(g_object_get_data(G_OBJECT(w), "beluga-uri"))));
                if (b->onDrop) b->onDrop(dest.get(), static_cast<GdkFileList *>(g_value_get_boxed(v)),
                                         gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(t)), w, x, y);
                return TRUE;
            }), this);
            gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(dt));
        }
        gtk_box_append(GTK_BOX(crumbBox_), b);
        if (!is_virtual_uri(c.uri)) gtk_box_append(GTK_BOX(crumbBox_), subfolderButton(c.uri));
    }
}

// ---- completion -----------------------------------------------------------------------------

void LocationBar::hideCompletion()
{
    if (completionPop_) gtk_popover_popdown(GTK_POPOVER(completionPop_));
}

void LocationBar::updateCompletion()
{
    std::string text = gtk_editable_get_text(GTK_EDITABLE(entry_));
    if (text.empty() || is_virtual_uri(text)) {
        hideCompletion();
        return;
    }
    size_t slash = text.rfind('/');
    std::string dirPart = slash == std::string::npos ? std::string() : text.substr(0, slash + 1);
    std::string prefix = slash == std::string::npos ? text : text.substr(slash + 1);
    Ref<GFile> base;
    if (!is_virtual_uri(uri_)) base = Ref<GFile>(g_file_new_for_uri(uri_.c_str()));
    Ref<GFile> dir(dirPart.empty() ? (base ? Ref<GFile>::share(base.get()).release() : nullptr) : file_for_input(dirPart, base.get()));
    if (!dir) {
        hideCompletion();
        return;
    }
    std::string key = file_uri(dir.get());
    if (!dirCache_.count(key)) {
        std::vector<std::pair<std::string, bool>> names;
        GFileEnumerator *en = g_file_enumerate_children(dir.get(), "standard::name,standard::type", G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
        if (en) {
            GFileInfo *info;
            while ((info = g_file_enumerator_next_file(en, nullptr, nullptr)) && names.size() < 5000) {
                names.emplace_back(g_file_info_get_name(info), g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY);
                g_object_unref(info);
            }
            g_object_unref(en);
        }
        std::sort(names.begin(), names.end());
        if (dirCache_.size() > 20) dirCache_.clear();
        dirCache_[key] = names;
    }
    std::string pf = casefold(prefix);
    bool hidden = prefix.size() > 0 && prefix[0] == '.';
    std::vector<std::pair<std::string, bool>> matches;
    for (auto &n : dirCache_[key]) {
        if (!hidden && !n.first.empty() && n.first[0] == '.') continue;
        if (starts_with(casefold(n.first), pf) && n.first != prefix) matches.push_back(n);
        if (matches.size() >= 40) break;
    }
    if (matches.empty()) {
        hideCompletion();
        return;
    }
    if (!completionPop_) {
        completionPop_ = gtk_popover_new();
        gtk_popover_set_autohide(GTK_POPOVER(completionPop_), FALSE);
        gtk_popover_set_has_arrow(GTK_POPOVER(completionPop_), FALSE);
        gtk_popover_set_position(GTK_POPOVER(completionPop_), GTK_POS_BOTTOM);
        gtk_widget_set_halign(completionPop_, GTK_ALIGN_START);
        gtk_widget_set_parent(completionPop_, entry_);
        gtk_widget_set_can_focus(completionPop_, FALSE);
        GtkWidget *scroll = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 300);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), TRUE);
        completionList_ = gtk_list_box_new();
        gtk_widget_set_can_focus(completionList_, FALSE);
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(completionList_), GTK_SELECTION_SINGLE);
        g_signal_connect(completionList_, "row-activated", CB([](GtkListBox *, GtkListBoxRow *row, gpointer p) {
            auto *b = static_cast<LocationBar *>(p);
            auto *t = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "beluga-text"));
            b->settingText_ = true;
            gtk_editable_set_text(GTK_EDITABLE(b->entry_), t);
            gtk_editable_set_position(GTK_EDITABLE(b->entry_), -1);
            b->settingText_ = false;
            b->hideCompletion();
            gtk_widget_grab_focus(b->entry_);
            gtk_editable_set_position(GTK_EDITABLE(b->entry_), -1);
            b->updateCompletion();
        }), this);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), completionList_);
        gtk_popover_set_child(GTK_POPOVER(completionPop_), scroll);
    }
    list_box_remove_rows(GTK_LIST_BOX(completionList_));
    for (auto &[name, isDir] : matches) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(isDir ? "folder" : "text-x-generic"));
        gtk_box_append(GTK_BOX(box), gtk_label_new(name.c_str()));
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        std::string full = dirPart + name + (isDir ? "/" : "");
        g_object_set_data_full(G_OBJECT(row), "beluga-text", g_strdup(full.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(completionList_), row);
    }
    gtk_popover_popup(GTK_POPOVER(completionPop_));
}

bool LocationBar::completionKey(guint keyval)
{
    bool visible = completionPop_ && gtk_widget_get_visible(completionPop_);
    if (keyval == GDK_KEY_Tab) {
        // Complete the longest common prefix of the matches.
        if (!visible) {
            updateCompletion();
            visible = completionPop_ && gtk_widget_get_visible(completionPop_);
            if (!visible) return true;
        }
        std::vector<std::string> texts;
        for (GtkWidget *r = gtk_widget_get_first_child(completionList_); r; r = gtk_widget_get_next_sibling(r))
            if (auto *t = static_cast<const char *>(g_object_get_data(G_OBJECT(r), "beluga-text"))) texts.push_back(t);
        if (texts.empty()) return true;
        std::string common = texts[0];
        for (auto &t : texts) {
            size_t k = 0;
            while (k < common.size() && k < t.size() && common[k] == t[k]) ++k;
            common.resize(k);
        }
        if (common.size() > strlen(gtk_editable_get_text(GTK_EDITABLE(entry_)))) {
            settingText_ = true;
            gtk_editable_set_text(GTK_EDITABLE(entry_), common.c_str());
            gtk_editable_set_position(GTK_EDITABLE(entry_), -1);
            settingText_ = false;
            updateCompletion();
        }
        return true;
    }
    if (!visible) return false;
    if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Up) {
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(completionList_));
        int idx = sel ? gtk_list_box_row_get_index(sel) : -1;
        idx += keyval == GDK_KEY_Down ? 1 : -1;
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(completionList_), std::max(0, idx));
        if (row) gtk_list_box_select_row(GTK_LIST_BOX(completionList_), row);
        return true;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(completionList_));
        if (sel) {
            g_signal_emit_by_name(completionList_, "row-activated", sel);
            return true;
        }
        hideCompletion();
        return false;
    }
    if (keyval == GDK_KEY_Escape) {
        hideCompletion();
        return true;
    }
    return false;
}

} // namespace beluga
