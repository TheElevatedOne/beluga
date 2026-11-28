// SPDX-License-Identifier: GPL-2.0-or-later
#include "filechooser.hpp"

#include "archive.hpp"
#include "dialogs.hpp"
#include "fileops.hpp"
#include "gtkutil.hpp"
#include "locationbar.hpp"
#include "placespanel.hpp"
#include "view.hpp"

#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif

#include <algorithm>
#include <memory>

namespace beluga {

namespace {

// The chooser's own state, in its own file: the chooser runs in another process than Beluga's windows
// and must never write beluga.conf over their changes.
struct ChooserState {
    std::string folder;
    int width = 900, height = 600;
    int viewMode = ModeDetails;
    bool hidden = false;
};

std::string state_path() { return config_dir() + "/filechooser.conf"; }

ChooserState load_state()
{
    ChooserState s;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, state_path().c_str(), G_KEY_FILE_NONE, nullptr)) {
        const char *G = "FileChooser";
        s.folder = take(g_key_file_get_string(kf, G, "Folder", nullptr));
        if (g_key_file_has_key(kf, G, "Width", nullptr)) s.width = std::max(400, g_key_file_get_integer(kf, G, "Width", nullptr));
        if (g_key_file_has_key(kf, G, "Height", nullptr)) s.height = std::max(300, g_key_file_get_integer(kf, G, "Height", nullptr));
        if (g_key_file_has_key(kf, G, "ViewMode", nullptr)) s.viewMode = std::clamp(g_key_file_get_integer(kf, G, "ViewMode", nullptr), 0, 2);
        s.hidden = g_key_file_get_boolean(kf, G, "Hidden", nullptr);
    }
    g_key_file_free(kf);
    return s;
}

void save_state(const ChooserState &s)
{
    GKeyFile *kf = g_key_file_new();
    const char *G = "FileChooser";
    g_key_file_set_string(kf, G, "Folder", s.folder.c_str());
    g_key_file_set_integer(kf, G, "Width", s.width);
    g_key_file_set_integer(kf, G, "Height", s.height);
    g_key_file_set_integer(kf, G, "ViewMode", s.viewMode);
    g_key_file_set_boolean(kf, G, "Hidden", s.hidden);
    g_key_file_save_to_file(kf, state_path().c_str(), nullptr);
    g_key_file_free(kf);
}

struct Chooser {
    ChooserRequest req;
    std::function<void(const ChooserResult &)> done;
    bool finished = false;
    ChooserState state;
    GtkWidget *win = nullptr, *nameEntry = nullptr, *filterDrop = nullptr, *accept = nullptr;
    GtkWidget *back = nullptr, *forward = nullptr, *up = nullptr, *hidden = nullptr;
    std::vector<GtkWidget *> modeButtons;
    std::vector<GtkWidget *> choiceWidgets;
    std::unique_ptr<View> view;
    std::unique_ptr<PlacesPanel> places;
    std::unique_ptr<LocationBar> loc;
    std::vector<std::string> history;
    int index = -1;
    int filter = -1;
    std::vector<std::pair<GPatternSpec *, std::string>> rules;   // the current filter: glob (casefolded) or MIME type
    bool settingName = false, settingWidgets = false;

    ~Chooser()
    {
        clearRules();
        if (!finished && done) done({});   // closed some other way: cancelled
    }
    void clearRules()
    {
        for (auto &r : rules)
            if (r.first) g_pattern_spec_free(r.first);
        rules.clear();
    }
    bool folderMode() const { return (req.mode == ChooserRequest::Mode::Open && req.directory) || req.mode == ChooserRequest::Mode::SaveFiles; }
};

Chooser *chooser_of(GtkWidget *w) { return w ? static_cast<Chooser *>(g_object_get_data(G_OBJECT(w), "beluga-chooser")) : nullptr; }

std::string uri_of_path(const std::string &p) { return file_uri(Ref<GFile>(g_file_new_for_path(p.c_str())).get()); }

bool is_archive_item(const ItemData &d) { return !d.isDir && archive::can_browse(d.mime, d.name); }

void finish(Chooser *c, ChooserResult r)
{
    if (c->finished) return;
    c->finished = true;
    r.filter = c->filter;
    for (size_t i = 0; i < c->req.choices.size() && i < c->choiceWidgets.size(); ++i) {
        const auto &ch = c->req.choices[i];
        GtkWidget *w = c->choiceWidgets[i];
        if (ch.options.empty()) {
            r.choices.push_back({ch.id, gtk_check_button_get_active(GTK_CHECK_BUTTON(w)) ? "true" : "false"});
        } else {
            guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(w));
            if (sel < ch.options.size()) r.choices.push_back({ch.id, ch.options[sel].first});
        }
    }
    // Remembered for next time.
    if (!c->view->isVirtual()) c->state.folder = take(g_file_get_path(c->view->locationFile().get()));
    c->state.viewMode = c->view->props().viewMode;
    c->state.hidden = c->view->props().hiddenFilesShown;
    gtk_window_get_default_size(GTK_WINDOW(c->win), &c->state.width, &c->state.height);
    if (gtk_widget_get_width(c->win) > 0) {
        c->state.width = gtk_widget_get_width(c->win);
        c->state.height = gtk_widget_get_height(c->win);
    }
    save_state(c->state);
    auto cb = std::move(c->done);
    c->done = nullptr;
    if (cb) cb(r);
    gtk_window_destroy(GTK_WINDOW(c->win));
}

void update_buttons(Chooser *c)
{
    gtk_widget_set_sensitive(c->back, c->index > 0);
    gtk_widget_set_sensitive(c->forward, c->index + 1 < (int)c->history.size());
    gtk_widget_set_sensitive(c->up, !c->view->isVirtual() && Ref<GFile>(g_file_get_parent(c->view->locationFile().get())));
    c->settingWidgets = true;
    int mode = c->view->props().viewMode;
    for (size_t i = 0; i < c->modeButtons.size(); ++i)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->modeButtons[i]), (int)i == mode);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->hidden), c->view->props().hiddenFilesShown);
    c->settingWidgets = false;
}

void open_location(Chooser *c, const std::string &uri)
{
    c->view->setLocation(uri);
    // The chooser's own view mode and hidden files, not saved for the folder.
    ViewProps p = c->view->props();
    p.viewMode = c->state.viewMode;
    p.hiddenFilesShown = c->state.hidden;
    if (p.viewMode != c->view->props().viewMode) p.zoomLevel = -1;
    c->view->applyProps(p, false);
    c->loc->setUri(uri);
    c->places->setCurrentUri(uri);
    update_buttons(c);
}

void navigate(Chooser *c, const std::string &uri, bool addHistory = true)
{
    if (uri.empty() || archive::is_archive_uri(uri)) return;
    if (addHistory) {
        if (c->index + 1 < (int)c->history.size()) c->history.erase(c->history.begin() + c->index + 1, c->history.end());
        c->history.push_back(uri);
        c->index = (int)c->history.size() - 1;
    }
    open_location(c, uri);
}

void apply_filter(Chooser *c, int index)
{
    c->filter = index;
    c->clearRules();
    if (index >= 0 && index < (int)c->req.filters.size())
        for (auto &[type, rule] : c->req.filters[index].rules)
            c->rules.push_back({type == 0 ? g_pattern_spec_new(casefold(rule).c_str()) : nullptr, rule});
    bool folders = c->folderMode();
    c->view->setItemFilter([c, folders](BelugaItem *it) {
        const ItemData &d = item_data(it);
        if (d.isDir) return true;
        if (folders) return false;
        if (c->rules.empty()) return true;
        std::string name = casefold(d.name);
        for (auto &[spec, rule] : c->rules) {
            if (spec ? (bool)g_pattern_spec_match_string(spec, name.c_str()) : (d.mime == rule || g_content_type_is_a(d.mime.c_str(), rule.c_str())))
                return true;
        }
        return false;
    });
}

std::string quote_names(const std::vector<std::string> &names)
{
    if (names.size() == 1) return names[0];
    std::string s;
    for (auto &n : names) {
        if (!s.empty()) s += ' ';
        s += '"' + n + '"';
    }
    return s;
}

// The names typed in the name field: one name, or several in quotes.
std::vector<std::string> parse_names(const std::string &text)
{
    std::vector<std::string> out;
    if (text.find('"') == std::string::npos) {
        if (!trim(text).empty()) out.push_back(trim(text));
        return out;
    }
    std::string cur;
    bool in = false;
    for (char ch : text) {
        if (ch == '"') {
            if (in && !cur.empty()) out.push_back(cur);
            cur.clear();
            in = !in;
        } else if (in) {
            cur += ch;
        }
    }
    return out;
}

void selection_changed(Chooser *c)
{
    if (c->req.mode == ChooserRequest::Mode::SaveFiles) return;
    std::vector<std::string> names;
    for (BelugaItem *it : c->view->selectedItems()) {
        const ItemData &d = item_data(it);
        bool wanted = c->req.mode == ChooserRequest::Mode::Open && c->req.directory ? d.isDir : !d.isDir;
        if (wanted) names.push_back(d.name);
    }
    if (names.empty()) return;   // keep what was typed
    c->settingName = true;
    gtk_editable_set_text(GTK_EDITABLE(c->nameEntry), quote_names(names).c_str());
    c->settingName = false;
}

void confirm_overwrite(Chooser *c, const std::vector<std::string> &uris)
{
    std::vector<std::string> existing;
    for (auto &u : uris) {
        Ref<GFile> f(g_file_new_for_uri(u.c_str()));
        if (g_file_query_exists(f.get(), nullptr)) existing.push_back(take(g_file_get_basename(f.get())));
    }
    ChooserResult r;
    r.accepted = true;
    r.uris = uris;
    if (existing.empty()) {
        finish(c, r);
        return;
    }
    std::string msg = existing.size() == 1 ? printf_str(_("“%s” already exists. Replace it?"), existing[0].c_str())
                                           : printf_str(_("%zu of these files already exist. Replace them?"), existing.size());
    ask(c->win, msg, _("The program you are saving from will write over the existing contents."), {_("Cancel"), _("Replace")}, 0, 0,
        [win = Ref<GtkWidget>::share(c->win), r](int answer) {
            if (answer != 1) return;
            if (Chooser *c2 = chooser_of(win.get())) finish(c2, r);
        });
}

void do_accept(Chooser *c)
{
    std::string text = trim(gtk_editable_get_text(GTK_EDITABLE(c->nameEntry)));
    Ref<GFile> folder = c->view->locationFile();
    using Mode = ChooserRequest::Mode;
    // A path typed in the name field: a folder opens, a file is taken.
    if (!text.empty() && (text.find('/') != std::string::npos || text[0] == '~') && text.find('"') == std::string::npos) {
        Ref<GFile> f(file_for_input(text, folder.get()));
        if (f && g_file_query_file_type(f.get(), G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY && !c->folderMode()) {
            navigate(c, file_uri(f.get()));
            gtk_editable_set_text(GTK_EDITABLE(c->nameEntry), "");
            return;
        }
        if (f && c->req.mode == Mode::Save) {
            confirm_overwrite(c, {file_uri(f.get())});
            return;
        }
        if (f && g_file_query_exists(f.get(), nullptr)) {
            finish(c, {true, {file_uri(f.get())}, {}, -1});
            return;
        }
    }
    auto selected = c->view->selectedItems();
    switch (c->req.mode) {
    case Mode::Open: {
        std::vector<std::string> uris;
        if (c->req.directory) {
            for (BelugaItem *it : selected)
                if (item_data(it).isDir) uris.push_back(item_data(it).uri);
            if (uris.empty() && folder) uris.push_back(file_uri(folder.get()));
        } else {
            std::vector<BelugaItem *> files, dirs;
            for (BelugaItem *it : selected) (item_data(it).isDir ? dirs : files).push_back(it);
            if (files.empty() && dirs.size() == 1) {
                navigate(c, item_data(dirs[0]).uri);
                return;
            }
            for (BelugaItem *it : files) uris.push_back(item_data(it).uri);
            // Names typed into the field, in the folder shown.
            if (uris.empty() && folder)
                for (auto &n : parse_names(text)) {
                    Ref<GFile> f(g_file_get_child(folder.get(), n.c_str()));
                    if (g_file_query_file_type(f.get(), G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY) {
                        navigate(c, file_uri(f.get()));
                        return;
                    }
                    if (g_file_query_exists(f.get(), nullptr)) uris.push_back(file_uri(f.get()));
                }
        }
        if (uris.empty()) return;
        if (!c->req.multiple && uris.size() > 1) uris.resize(1);
        finish(c, {true, uris, {}, -1});
        return;
    }
    case Mode::Save: {
        if (!folder || c->view->isVirtual()) {
            show_error(c->win, _("Choose a folder to save in."));
            return;
        }
        if (text.empty()) {
            gtk_widget_grab_focus(c->nameEntry);
            return;
        }
        Ref<GFile> target(g_file_get_child_for_display_name(folder.get(), text.c_str(), nullptr));
        if (!target) return;
        if (g_file_query_file_type(target.get(), G_FILE_QUERY_INFO_NONE, nullptr) == G_FILE_TYPE_DIRECTORY) {
            navigate(c, file_uri(target.get()));
            gtk_editable_set_text(GTK_EDITABLE(c->nameEntry), "");
            return;
        }
        confirm_overwrite(c, {file_uri(target.get())});
        return;
    }
    case Mode::SaveFiles: {
        Ref<GFile> dir = folder;
        if (selected.size() == 1 && item_data(selected[0]).isDir) dir = item_data(selected[0]).file;
        if (!dir || c->view->isVirtual()) {
            show_error(c->win, _("Choose a folder to save in."));
            return;
        }
        std::vector<std::string> uris;
        for (auto &n : c->req.files) uris.push_back(file_uri(Ref<GFile>(g_file_get_child(dir.get(), n.c_str())).get()));
        confirm_overwrite(c, uris);
        return;
    }
    }
}

// A themed icon from the first name the theme has (the same names as Beluga's tool bar).
GtkWidget *icon_of(const char *const *names)
{
    GIcon *i = g_themed_icon_new_from_names((char **)names, -1);
    GtkWidget *img = gtk_image_new_from_gicon(i);
    g_object_unref(i);
    return img;
}

GtkWidget *tool_button(const char *const *icons, const char *tooltip)
{
    GtkWidget *b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(b), icon_of(icons));
    gtk_widget_add_css_class(b, "flat");
    gtk_widget_set_tooltip_text(b, tooltip);
    return b;
}

void set_parent(GtkWidget *win, const std::string &parent)
{
#ifdef GDK_WINDOWING_WAYLAND
    if (!starts_with(parent, "wayland:")) return;
    GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(win));
    if (s && GDK_IS_WAYLAND_TOPLEVEL(s)) gdk_wayland_toplevel_set_transient_for_exported(GDK_WAYLAND_TOPLEVEL(s), parent.substr(8).c_str());
#else
    (void)win;
    (void)parent;
#endif
}

} // namespace

GtkWidget *show_file_chooser(GtkApplication *app, const ChooserRequest &req, std::function<void(const ChooserResult &)> done)
{
    auto *c = new Chooser;
    c->req = req;
    c->done = std::move(done);
    c->state = load_state();
    using Mode = ChooserRequest::Mode;

    GtkWidget *win = gtk_window_new();
    c->win = win;
    if (app) gtk_window_set_application(GTK_WINDOW(win), app);
    g_object_set_data_full(G_OBJECT(win), "beluga-chooser", c, [](gpointer p) { delete static_cast<Chooser *>(p); });
    std::string title = req.title;
    if (title.empty())
        title = req.mode == Mode::Open ? (req.directory ? _("Select Folder") : req.multiple ? _("Open Files") : _("Open File"))
                                       : req.mode == Mode::Save ? _("Save File") : _("Save Files");
    gtk_window_set_title(GTK_WINDOW(win), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(win), c->state.width, c->state.height);
    gtk_widget_add_css_class(win, "beluga-window");
    gtk_widget_add_css_class(win, "beluga-file-chooser");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), box);

    // Tool bar: history, the location, view modes, hidden files, new folder.
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(bar, "toolbar");
    gtk_widget_add_css_class(bar, "beluga-toolbar");
    static const char *const back[] = {"go-previous", "go-previous-symbolic", nullptr};
    static const char *const fwd[] = {"go-next", "go-next-symbolic", nullptr};
    static const char *const upIcon[] = {"go-up", "go-up-symbolic", nullptr};
    static const char *const iconsIcon[] = {"view-list-icons", "view-grid-symbolic", nullptr};
    static const char *const compactIcon[] = {"view-list-details", "view-list-compact-symbolic", "view-list-symbolic", nullptr};
    static const char *const detailsIcon[] = {"view-list-tree", "view-list-symbolic", nullptr};
    static const char *const hiddenIcon[] = {"view-hidden", "view-reveal-symbolic", "view-conceal-symbolic", nullptr};
    static const char *const newFolderIcon[] = {"folder-new", "folder-new-symbolic", nullptr};
    c->back = tool_button(back, _("Back"));
    c->forward = tool_button(fwd, _("Forward"));
    c->up = tool_button(upIcon, _("Up"));
    for (GtkWidget *b : {c->back, c->forward, c->up}) gtk_box_append(GTK_BOX(bar), b);
    c->loc = std::make_unique<LocationBar>();
    gtk_widget_set_hexpand(c->loc->widget(), TRUE);
    gtk_box_append(GTK_BOX(bar), c->loc->widget());
    struct { const char *const *icons; const char *tip; } modes[] = {{iconsIcon, N_("Icons")}, {compactIcon, N_("Compact")}, {detailsIcon, N_("Details")}};
    for (int i = 0; i < 3; ++i) {
        GtkWidget *t = gtk_toggle_button_new();
        gtk_button_set_child(GTK_BUTTON(t), icon_of(modes[i].icons));
        gtk_widget_add_css_class(t, "flat");
        gtk_widget_set_tooltip_text(t, _(modes[i].tip));
        g_object_set_data(G_OBJECT(t), "beluga-mode", GINT_TO_POINTER(i));
        g_signal_connect(t, "toggled", CB([](GtkToggleButton *t, gpointer p) {
            auto *c = static_cast<Chooser *>(p);
            if (c->settingWidgets || !gtk_toggle_button_get_active(t)) return;
            c->state.viewMode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "beluga-mode"));
            ViewProps pr = c->view->props();
            pr.viewMode = c->state.viewMode;
            pr.zoomLevel = -1;
            c->view->applyProps(pr, false);
            update_buttons(c);
        }), c);
        c->modeButtons.push_back(t);
        gtk_box_append(GTK_BOX(bar), t);
    }
    c->hidden = gtk_toggle_button_new();
    gtk_button_set_child(GTK_BUTTON(c->hidden), icon_of(hiddenIcon));
    gtk_widget_add_css_class(c->hidden, "flat");
    gtk_widget_set_tooltip_text(c->hidden, _("Show Hidden Files"));
    gtk_box_append(GTK_BOX(bar), c->hidden);
    GtkWidget *newFolder = tool_button(newFolderIcon, _("Create Folder"));
    gtk_box_append(GTK_BOX(bar), newFolder);
    gtk_box_append(GTK_BOX(box), bar);

    // Places | view
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);
    c->places = std::make_unique<PlacesPanel>();
    gtk_widget_set_size_request(c->places->widget(), 140, -1);
    gtk_paned_set_start_child(GTK_PANED(paned), c->places->widget());
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_position(GTK_PANED(paned), 180);
    c->view = std::make_unique<View>();
    c->view->setSaveProps(false);
    gtk_paned_set_end_child(GTK_PANED(paned), c->view->widget());
    gtk_box_append(GTK_BOX(box), paned);

    // Name, file type, the program's options, buttons.
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 10);
    gtk_widget_set_margin_end(grid, 10);
    gtk_widget_set_margin_top(grid, 8);
    gtk_widget_set_margin_bottom(grid, 10);
    int row = 0;
    auto label = [&](const char *text, GtkWidget *target) {
        GtkWidget *l = gtk_label_new_with_mnemonic(text);
        gtk_label_set_xalign(GTK_LABEL(l), 1);
        gtk_label_set_mnemonic_widget(GTK_LABEL(l), target);
        gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    };
    c->nameEntry = gtk_entry_new();
    gtk_widget_set_hexpand(c->nameEntry, TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(c->nameEntry), TRUE);
    if (req.mode != Mode::SaveFiles) {
        label(req.mode == Mode::Open && req.directory ? _("_Folder:") : _("_Name:"), c->nameEntry);
        gtk_grid_attach(GTK_GRID(grid), c->nameEntry, 1, row++, 1, 1);
    }
    if (!req.filters.empty() && !c->folderMode()) {
        std::vector<const char *> names;
        for (auto &f : req.filters) names.push_back(f.name.c_str());
        names.push_back(nullptr);
        c->filterDrop = gtk_drop_down_new_from_strings(names.data());
        gtk_widget_set_hexpand(c->filterDrop, TRUE);
        label(_("_Type:"), c->filterDrop);
        gtk_grid_attach(GTK_GRID(grid), c->filterDrop, 1, row++, 1, 1);
    }
    for (auto &ch : req.choices) {
        GtkWidget *w;
        if (ch.options.empty()) {
            w = gtk_check_button_new_with_mnemonic(ch.label.c_str());
            gtk_check_button_set_active(GTK_CHECK_BUTTON(w), ch.selected == "true");
            gtk_grid_attach(GTK_GRID(grid), w, 1, row++, 1, 1);
        } else {
            std::vector<const char *> labels;
            guint sel = 0;
            for (size_t i = 0; i < ch.options.size(); ++i) {
                labels.push_back(ch.options[i].second.c_str());
                if (ch.options[i].first == ch.selected) sel = (guint)i;
            }
            labels.push_back(nullptr);
            w = gtk_drop_down_new_from_strings(labels.data());
            gtk_drop_down_set_selected(GTK_DROP_DOWN(w), sel);
            label((ch.label + ":").c_str(), w);
            gtk_grid_attach(GTK_GRID(grid), w, 1, row++, 1, 1);
        }
        c->choiceWidgets.push_back(w);
    }
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
    std::string acceptText = req.acceptLabel;
    if (acceptText.empty())
        acceptText = req.mode == Mode::Open ? (req.directory ? _("_Select") : _("_Open")) : _("_Save");
    c->accept = gtk_button_new_with_mnemonic(acceptText.c_str());
    gtk_widget_add_css_class(c->accept, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), c->accept);
    gtk_grid_attach(GTK_GRID(grid), buttons, 0, row, 2, 1);
    gtk_box_append(GTK_BOX(box), grid);
    gtk_window_set_default_widget(GTK_WINDOW(win), c->accept);

    // Behaviour.
    c->loc->onNavigate = [c](const std::string &u) { navigate(c, u); };
    c->loc->showHidden = [c] { return c->view->props().hiddenFilesShown; };
    c->places->onNavigate = [c](const std::string &u) { navigate(c, u); };
    c->view->onSelectionChanged = [c] { selection_changed(c); };
    c->view->onActivate = [c](const std::vector<BelugaItem *> &items, GdkModifierType) {
        if (items.size() == 1 && item_data(items[0]).isDir) {
            navigate(c, item_data(items[0]).uri);
            return;
        }
        // Files (archives too: programs can't read inside them).
        if (c->req.mode == ChooserRequest::Mode::Open && !c->req.directory) do_accept(c);
        else if (c->req.mode == ChooserRequest::Mode::Save) {
            for (BelugaItem *it : items)
                if (!item_data(it).isDir || is_archive_item(item_data(it))) {
                    gtk_editable_set_text(GTK_EDITABLE(c->nameEntry), item_data(it).name.c_str());
                    do_accept(c);
                    return;
                }
        }
    };
    c->view->onEscape = [c] { finish(c, {}); };
    g_signal_connect_swapped(c->back, "clicked", CB([](Chooser *c) {
        if (c->index <= 0) return;
        c->index--;
        open_location(c, c->history[c->index]);
    }), c);
    g_signal_connect_swapped(c->forward, "clicked", CB([](Chooser *c) {
        if (c->index + 1 >= (int)c->history.size()) return;
        c->index++;
        open_location(c, c->history[c->index]);
    }), c);
    g_signal_connect_swapped(c->up, "clicked", CB([](Chooser *c) {
        Ref<GFile> f = c->view->locationFile();
        Ref<GFile> parent(f ? g_file_get_parent(f.get()) : nullptr);
        if (!parent) return;
        std::string from = c->view->location();
        navigate(c, file_uri(parent.get()));
        c->view->selectAndShow(from);
    }), c);
    g_signal_connect(c->hidden, "toggled", CB([](GtkToggleButton *t, gpointer p) {
        auto *c = static_cast<Chooser *>(p);
        if (c->settingWidgets) return;
        c->state.hidden = gtk_toggle_button_get_active(t);
        ViewProps pr = c->view->props();
        pr.hiddenFilesShown = c->state.hidden;
        c->view->applyProps(pr, false);
    }), c);
    g_signal_connect_swapped(newFolder, "clicked", CB([](Chooser *c) {
        Ref<GFile> dir = c->view->locationFile();
        if (!dir || c->view->isVirtual()) return;
        ask_name(c->win, _("New Folder"), printf_str(_("Create new folder in:\n%s"), display_location(dir.get()).c_str()),
                 unique_name(dir.get(), _("New Folder")), -1, _("C_reate"),
                 [win = Ref<GtkWidget>::share(c->win), dir](const std::string &name) {
                     Chooser *c2 = chooser_of(win.get());
                     if (!c2) return;
                     Ref<GFile> created;
                     GError *err = nullptr;
                     if (!make_directory(dir.get(), trim(name), created, &err)) {
                         show_error(win.get(), _("Could not create the folder."), err->message);
                         g_error_free(err);
                         return;
                     }
                     c2->view->selectAndShow(file_uri(created.get()));
                 });
    }), c);
    if (c->filterDrop)
        g_signal_connect(c->filterDrop, "notify::selected", CB([](GtkDropDown *dd, GParamSpec *, gpointer p) {
            apply_filter(static_cast<Chooser *>(p), (int)gtk_drop_down_get_selected(dd));
        }), c);
    g_signal_connect_swapped(cancel, "clicked", CB([](Chooser *c) { finish(c, {}); }), c);
    g_signal_connect_swapped(c->accept, "clicked", CB([](Chooser *c) { do_accept(c); }), c);
    g_signal_connect(win, "close-request", CB([](GtkWindow *w, gpointer) -> gboolean {
        if (Chooser *c = chooser_of(GTK_WIDGET(w))) finish(c, {});
        return TRUE;   // finish() destroyed it
    }), nullptr);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType mods, gpointer p) -> gboolean {
        auto *c = static_cast<Chooser *>(p);
        if (keyval == GDK_KEY_Escape) {
            finish(c, {});
            return TRUE;
        }
        if ((mods & GDK_ALT_MASK) && keyval == GDK_KEY_Up) {
            g_signal_emit_by_name(c->up, "clicked");
            return TRUE;
        }
        if ((mods & GDK_CONTROL_MASK) && keyval == GDK_KEY_h) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c->hidden), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(c->hidden)));
            return TRUE;
        }
        if ((mods & GDK_CONTROL_MASK) && keyval == GDK_KEY_l) {
            c->loc->startEditing(true);
            return TRUE;
        }
        return FALSE;
    }), c);
    gtk_widget_add_controller(win, key);
    std::string parent = req.parentWindow;
    g_signal_connect_data(win, "realize", CB([](GtkWidget *w, gpointer p) { set_parent(w, static_cast<const char *>(p)); }),
                          g_strdup(parent.c_str()), [](gpointer p, GClosure *) { g_free(p); }, GConnectFlags(G_CONNECT_AFTER));

    // Where to start: the program's folder or file, else the last folder used, else home.
    std::string start = req.currentFolder;
    if (start.empty() && !req.currentFile.empty()) start = dirname_of(req.currentFile);
    if (start.empty() || !g_file_test(start.c_str(), G_FILE_TEST_IS_DIR)) start = c->state.folder;
    if (start.empty() || !g_file_test(start.c_str(), G_FILE_TEST_IS_DIR)) start = home_dir();
    apply_filter(c, req.filters.empty() ? -1 : std::clamp(req.currentFilter, 0, (int)req.filters.size() - 1));
    if (c->filterDrop && c->filter >= 0) gtk_drop_down_set_selected(GTK_DROP_DOWN(c->filterDrop), (guint)c->filter);
    navigate(c, uri_of_path(start));
    if (req.mode == Mode::Save) {
        std::string name = !req.currentName.empty() ? req.currentName : basename_of(req.currentFile);
        if (!req.currentFile.empty() || !name.empty()) gtk_editable_set_text(GTK_EDITABLE(c->nameEntry), name.c_str());
        if (!req.currentFile.empty()) c->view->selectUris({uri_of_path(req.currentFile)});
    }
    gtk_window_present(GTK_WINDOW(win));
    if (req.mode == Mode::Save) {
        gtk_widget_grab_focus(c->nameEntry);
        std::string name = gtk_editable_get_text(GTK_EDITABLE(c->nameEntry));
        int stem = (int)g_utf8_strlen(split_extension(name).first.c_str(), -1);
        gtk_editable_select_region(GTK_EDITABLE(c->nameEntry), 0, stem);
    } else {
        c->view->focus();
    }
    return win;
}

void file_chooser_close(GtkWidget *chooser)
{
    if (Chooser *c = chooser_of(chooser)) finish(c, {});
}

View *file_chooser_view(GtkWidget *chooser)
{
    Chooser *c = chooser_of(chooser);
    return c ? c->view.get() : nullptr;
}

void file_chooser_set_name(GtkWidget *chooser, const std::string &name)
{
    if (Chooser *c = chooser_of(chooser)) gtk_editable_set_text(GTK_EDITABLE(c->nameEntry), name.c_str());
}

void file_chooser_navigate(GtkWidget *chooser, const std::string &uri)
{
    if (Chooser *c = chooser_of(chooser)) navigate(c, uri);
}

void file_chooser_set_filter(GtkWidget *chooser, int index)
{
    Chooser *c = chooser_of(chooser);
    if (!c) return;
    if (c->filterDrop) gtk_drop_down_set_selected(GTK_DROP_DOWN(c->filterDrop), (guint)index);
    else apply_filter(c, index);
}

void file_chooser_accept(GtkWidget *chooser)
{
    if (Chooser *c = chooser_of(chooser)) do_accept(c);
}

} // namespace beluga
