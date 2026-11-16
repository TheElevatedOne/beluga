// SPDX-License-Identifier: GPL-2.0-or-later
#include "searchbar.hpp"

#include "gtkutil.hpp"
#include "settings.hpp"

namespace beluga {

static GtkWidget *close_button()
{
    GtkWidget *b = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, _("Close"));
    return b;
}

// ---- filter bar ----------------------------------------------------------------------------

FilterBar::FilterBar()
{
    revealer_ = gtk_revealer_new();
    g_object_ref_sink(revealer_);
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer_), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer_), 120);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(box, "beluga-filterbar");
    GtkWidget *l = gtk_label_new(_("Filter:"));
    gtk_box_append(GTK_BOX(box), l);
    entry_ = gtk_search_entry_new();
    gtk_widget_set_hexpand(entry_, TRUE);
    g_object_set(entry_, "placeholder-text", _("Filter by name (wildcards like *.txt work)"), nullptr);
    gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(entry_), 80);
    gtk_box_append(GTK_BOX(box), entry_);
    lock_ = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(lock_), "object-locked-symbolic");
    gtk_widget_add_css_class(lock_, "flat");
    gtk_widget_set_tooltip_text(lock_, _("Keep the filter when changing folders"));
    gtk_box_append(GTK_BOX(box), lock_);
    GtkWidget *close = close_button();
    gtk_box_append(GTK_BOX(box), close);
    gtk_revealer_set_child(GTK_REVEALER(revealer_), box);

    g_signal_connect(entry_, "search-changed", CB([](GtkSearchEntry *e, gpointer p) {
        auto *f = static_cast<FilterBar *>(p);
        if (f->onChanged) f->onChanged(gtk_editable_get_text(GTK_EDITABLE(e)));
    }), this);
    g_signal_connect(entry_, "stop-search", CB([](GtkSearchEntry *, gpointer p) {
        auto *f = static_cast<FilterBar *>(p);
        if (f->onClose) f->onClose();
    }), this);
    g_signal_connect(entry_, "activate", CB([](GtkSearchEntry *, gpointer p) {
        auto *f = static_cast<FilterBar *>(p);
        if (f->onFocusView) f->onFocusView();
    }), this);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer p) -> gboolean {
        auto *f = static_cast<FilterBar *>(p);
        if (keyval == GDK_KEY_Down && f->onFocusView) {
            f->onFocusView();
            return TRUE;
        }
        return FALSE;
    }), this);
    gtk_widget_add_controller(entry_, key);
    g_signal_connect_swapped(close, "clicked", CB([](FilterBar *f) { if (f->onClose) f->onClose(); }), this);
}

FilterBar::~FilterBar() { g_object_unref(revealer_); }

void FilterBar::setVisible(bool on) { gtk_revealer_set_reveal_child(GTK_REVEALER(revealer_), on); }
bool FilterBar::visible() const { return gtk_revealer_get_reveal_child(GTK_REVEALER(revealer_)); }
void FilterBar::focus() { gtk_widget_grab_focus(entry_); }
void FilterBar::clear() { gtk_editable_set_text(GTK_EDITABLE(entry_), ""); }
std::string FilterBar::text() const { return gtk_editable_get_text(GTK_EDITABLE(entry_)); }
bool FilterBar::locked() const { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(lock_)); }

// ---- search bar ------------------------------------------------------------------------------

SearchBar::SearchBar()
{
    revealer_ = gtk_revealer_new();
    g_object_ref_sink(revealer_);
    gtk_revealer_set_transition_duration(GTK_REVEALER(revealer_), 120);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(outer, "beluga-searchbar");
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    entry_ = gtk_search_entry_new();
    gtk_widget_set_hexpand(entry_, TRUE);
    g_object_set(entry_, "placeholder-text", _("Search…"), nullptr);
    gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(entry_), 400);
    gtk_box_append(GTK_BOX(row1), entry_);
    GtkWidget *close = close_button();
    gtk_box_append(GTK_BOX(row1), close);
    gtk_box_append(GTK_BOX(outer), row1);

    // Options wrap onto more lines in narrow (split) views.
    GtkWidget *row2 = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(row2), GTK_SELECTION_NONE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(row2), 6);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(row2), 4);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(row2), 8);
    const char *where[] = {_("In This Folder"), _("In Your Files"), _("Everywhere"), nullptr};
    where_ = gtk_drop_down_new_from_strings(where);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(where_), settings().searchEverywhere ? 1 : 0);
    const char *what[] = {_("File Names"), _("File Names and Contents"), nullptr};
    what_ = gtk_drop_down_new_from_strings(what);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(what_), settings().searchContent ? 1 : 0);
    const char *types[] = {_("All Files"), _("Folders"), _("Documents"), _("Images"), _("Audio"), _("Videos"), _("Archives"), nullptr};
    type_ = gtk_drop_down_new_from_strings(types);
    const char *when[] = {_("Any Time"), _("Today"), _("Last 7 Days"), _("Last 30 Days"), _("Last Year"), nullptr};
    when_ = gtk_drop_down_new_from_strings(when);
    hidden_ = gtk_check_button_new_with_label(_("Hidden files"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(hidden_), settings().searchHidden);
    for (GtkWidget *w : {where_, what_, type_, when_, hidden_}) {
        gtk_flow_box_append(GTK_FLOW_BOX(row2), w);
        gtk_widget_set_focusable(gtk_widget_get_parent(w), FALSE);
    }
    gtk_box_append(GTK_BOX(outer), row2);
    gtk_revealer_set_child(GTK_REVEALER(revealer_), outer);

    g_signal_connect_swapped(entry_, "search-changed", CB([](SearchBar *s) { s->schedule(); }), this);
    g_signal_connect_swapped(entry_, "activate", CB([](SearchBar *s) {
        s->run();
        if (s->onFocusView) s->onFocusView();
    }), this);
    g_signal_connect_swapped(entry_, "stop-search", CB([](SearchBar *s) { if (s->onClose) s->onClose(); }), this);
    for (GtkWidget *w : {where_, what_, type_, when_})
        g_signal_connect_swapped(w, "notify::selected", CB([](SearchBar *s) { s->schedule(); }), this);
    g_signal_connect_swapped(hidden_, "toggled", CB([](SearchBar *s) { s->schedule(); }), this);
    g_signal_connect_swapped(close, "clicked", CB([](SearchBar *s) { if (s->onClose) s->onClose(); }), this);
}

SearchBar::~SearchBar()
{
    if (timer_) g_source_remove(timer_);
    g_object_unref(revealer_);
}

void SearchBar::setVisible(bool on) { gtk_revealer_set_reveal_child(GTK_REVEALER(revealer_), on); }
bool SearchBar::visible() const { return gtk_revealer_get_reveal_child(GTK_REVEALER(revealer_)); }

void SearchBar::focus()
{
    gtk_widget_grab_focus(entry_);
    gtk_editable_select_region(GTK_EDITABLE(entry_), 0, -1);
}

void SearchBar::setFolder(const std::string &uri)
{
    if (!is_virtual_uri(uri)) folder_ = uri;
}

void SearchBar::showQuery(const SearchQuery &q)
{
    updating_ = true;
    gtk_editable_set_text(GTK_EDITABLE(entry_), q.text.c_str());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(what_), q.content ? 1 : 0);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(hidden_), q.includeHidden);
    static const char *types[] = {"", "folders", "documents", "images", "audio", "videos", "archives"};
    for (int i = 0; i < 7; ++i)
        if (q.type == types[i]) gtk_drop_down_set_selected(GTK_DROP_DOWN(type_), i);
    int days[] = {0, 1, 7, 30, 365};
    for (int i = 0; i < 5; ++i)
        if (q.modifiedWithinDays == days[i]) gtk_drop_down_set_selected(GTK_DROP_DOWN(when_), i);
    std::string home = file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get());
    if (q.rootUri == "file:///") gtk_drop_down_set_selected(GTK_DROP_DOWN(where_), 2);
    else if (q.rootUri == home && folder_ != home) gtk_drop_down_set_selected(GTK_DROP_DOWN(where_), 1);
    else {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(where_), 0);
        folder_ = q.rootUri;
    }
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    updating_ = false;
}

void SearchBar::schedule()
{
    if (updating_) return;
    if (timer_) g_source_remove(timer_);
    timer_ = g_timeout_add(250, [](gpointer p) -> gboolean {
        auto *s = static_cast<SearchBar *>(p);
        s->timer_ = 0;
        s->run();
        return G_SOURCE_REMOVE;
    }, this);
}

void SearchBar::run()
{
    if (timer_) g_source_remove(timer_);
    timer_ = 0;
    SearchQuery q;
    q.text = trim(gtk_editable_get_text(GTK_EDITABLE(entry_)));
    guint where = gtk_drop_down_get_selected(GTK_DROP_DOWN(where_));
    std::string home = file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get());
    q.rootUri = where == 2 ? "file:///" : where == 1 ? home : (folder_.empty() ? home : folder_);
    q.content = gtk_drop_down_get_selected(GTK_DROP_DOWN(what_)) == 1;
    q.includeHidden = gtk_check_button_get_active(GTK_CHECK_BUTTON(hidden_));
    static const char *types[] = {"", "folders", "documents", "images", "audio", "videos", "archives"};
    guint t = gtk_drop_down_get_selected(GTK_DROP_DOWN(type_));
    q.type = t < 7 ? types[t] : "";
    int days[] = {0, 1, 7, 30, 365};
    guint d = gtk_drop_down_get_selected(GTK_DROP_DOWN(when_));
    q.modifiedWithinDays = d < 5 ? days[d] : 0;
    Settings &s = settings();
    s.searchEverywhere = where == 1;
    s.searchContent = q.content;
    s.searchHidden = q.includeHidden;
    if (q.text.empty() && q.type.empty() && q.modifiedWithinDays == 0) return;
    if (onSearch) onSearch(q.toUri());
}

} // namespace beluga
