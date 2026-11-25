// SPDX-License-Identifier: GPL-2.0-or-later
#include "placespanel.hpp"

#include "devices.hpp"
#include "dialogs.hpp"
#include "fileops.hpp"
#include "gtkutil.hpp"
#include "places.hpp"
#include "settings.hpp"
#include "trash.hpp"

#include <algorithm>

namespace beluga {

namespace {

struct RowData {
    std::string key;        // "place:<index>" or "device:<id>"
    std::string uri;        // target folder ("" for unmounted devices)
    std::string section;
    int placeIndex = -1;
    std::string deviceId;
};

RowData *row_data(GtkListBoxRow *row)
{
    return row ? static_cast<RowData *>(g_object_get_data(G_OBJECT(row), "beluga-row")) : nullptr;
}

std::string uri_for_path(const std::string &p) { return take(g_filename_to_uri(p.c_str(), nullptr, nullptr)); }

} // namespace

PlacesPanel::PlacesPanel()
{
    root_ = gtk_scrolled_window_new();
    g_object_ref_sink(root_);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(root_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(root_, "beluga-places");
    list_ = gtk_list_box_new();
    gtk_widget_add_css_class(list_, "navigation-sidebar");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), list_);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list_), [](GtkListBoxRow *row, GtkListBoxRow *before, gpointer) {
        RowData *d = row_data(row), *b = row_data(before);
        if (!d || (b && b->section == d->section)) {
            gtk_list_box_row_set_header(row, nullptr);
            return;
        }
        GtkWidget *l = gtk_label_new(d->section.c_str());
        gtk_widget_add_css_class(l, "places-header");
        gtk_widget_add_css_class(l, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_list_box_row_set_header(row, l);
    }, nullptr, nullptr);
    g_signal_connect(list_, "row-activated", CB([](GtkListBox *, GtkListBoxRow *row, gpointer p) {
        auto *pp = static_cast<PlacesPanel *>(p);
        if (!pp->rebuilding_) pp->activateRow(row, false);
    }), this);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", CB([](GtkGestureClick *g, int, double x, double y, gpointer p) {
        auto *pp = static_cast<PlacesPanel *>(p);
        guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
        GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(pp->list_), (int)y);
        if (b == GDK_BUTTON_MIDDLE && row) {
            pp->activateRow(row, true);
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        } else if (b == GDK_BUTTON_SECONDARY) {
            pp->showMenu(row, x, y);
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
        }
    }), this);
    gtk_widget_add_controller(list_, GTK_EVENT_CONTROLLER(click));

    // Files dropped on an entry go there; folders dropped elsewhere become places.
    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GdkDragAction(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
    g_signal_connect(dt, "drop", CB([](GtkDropTarget *t, const GValue *v, double x, double y, gpointer p) -> gboolean {
        auto *pp = static_cast<PlacesPanel *>(p);
        auto *files = static_cast<GdkFileList *>(g_value_get_boxed(v));
        GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(pp->list_), (int)y);
        RowData *d = row_data(row);
        GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(t));
        if (d && !d->uri.empty() && !is_search_uri(d->uri) && !is_recent_uri(d->uri)) {
            if (is_trash_uri(d->uri)) {
                if (pp->onDrop) pp->onDrop(nullptr, files, mods, pp->list_, x, y);
            } else {
                Ref<GFile> dest(g_file_new_for_uri(d->uri.c_str()));
                if (pp->onDrop) pp->onDrop(dest.get(), files, mods, pp->list_, x, y);
            }
            return TRUE;
        }
        GSList *l = gdk_file_list_get_files(files);
        for (GSList *i = l; i; i = i->next) {
            GFile *f = G_FILE(i->data);
            if (g_file_query_file_type(f, G_FILE_QUERY_INFO_NONE, nullptr) != G_FILE_TYPE_DIRECTORY) continue;
            std::string u = file_uri(f);
            if (Places::instance().indexOf(u) >= 0) continue;
            Places::instance().add({take(g_file_get_basename(f)), u, Places::iconForUri(u), false});
        }
        g_slist_free(l);
        return TRUE;
    }), this);
    gtk_widget_add_controller(list_, GTK_EVENT_CONTROLLER(dt));

    // Reordering places by dragging rows.
    GtkDropTarget *rt = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
    g_signal_connect(rt, "drop", CB([](GtkDropTarget *, const GValue *v, double, double y, gpointer p) -> gboolean {
        auto *pp = static_cast<PlacesPanel *>(p);
        std::string s = g_value_get_string(v) ? g_value_get_string(v) : "";
        if (!starts_with(s, "beluga-place:")) return FALSE;
        int from = std::atoi(s.c_str() + 10);
        GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(pp->list_), (int)y);
        RowData *d = row_data(row);
        int to = d && d->placeIndex >= 0 ? d->placeIndex : (int)Places::instance().list().size();
        if (d && d->placeIndex >= 0 && from < to) to++;
        Places::instance().move(from, to);
        return TRUE;
    }), this);
    gtk_widget_add_controller(list_, GTK_EVENT_CONTROLLER(rt));

    placesListener_ = Places::instance().addListener([this] { rebuild(); });
    devicesListener_ = DeviceManager::instance().addListener([this] { rebuild(); });
    settingsListener_ = add_settings_listener([this] { rebuild(); });
    DeviceManager::instance().init();
    rebuild();
}

PlacesPanel::~PlacesPanel()
{
    Places::instance().removeListener(placesListener_);
    DeviceManager::instance().removeListener(devicesListener_);
    remove_settings_listener(settingsListener_);
    // A context menu that is still open hangs off root_.
    for (GtkWidget *c = gtk_widget_get_first_child(root_), *next; c; c = next) {
        next = gtk_widget_get_next_sibling(c);
        if (GTK_IS_POPOVER(c)) gtk_widget_unparent(c);
    }
    g_object_unref(root_);
}

void PlacesPanel::focus() { gtk_widget_grab_focus(list_); }

void PlacesPanel::rebuild()
{
    rebuilding_ = true;
    list_box_remove_rows(GTK_LIST_BOX(list_));
    const Settings &s = settings();
    auto hiddenSection = [&](PlaceSection sec) {
        return std::find(s.hiddenPlaceSections.begin(), s.hiddenPlaceSections.end(), section_id(sec)) != s.hiddenPlaceSections.end();
    };
    int iconSize = s.placesIconSize > 0 ? s.placesIconSize : 22;

    auto makeRow = [&](RowData *d, const std::string &label, const std::string &icon, bool dim, GtkWidget *extra, GtkWidget *trailing) {
        GtkWidget *row = gtk_list_box_row_new();
        g_object_set_data_full(G_OBJECT(row), "beluga-row", d, [](gpointer p) { delete static_cast<RowData *>(p); });
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GIcon *gi = g_themed_icon_new_with_default_fallbacks(icon.c_str());
        GtkWidget *img = gtk_image_new_from_gicon(gi);
        g_object_unref(gi);
        gtk_image_set_pixel_size(GTK_IMAGE(img), iconSize);
        gtk_box_append(GTK_BOX(box), img);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(texts, TRUE);
        GtkWidget *l = gtk_label_new(label.c_str());
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(texts), l);
        if (extra) gtk_box_append(GTK_BOX(texts), extra);
        gtk_box_append(GTK_BOX(box), texts);
        if (trailing) gtk_box_append(GTK_BOX(box), trailing);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        if (dim) gtk_widget_add_css_class(row, "dim-label");
        gtk_widget_set_tooltip_text(row, d->uri.empty() ? label.c_str() : display_location(d->uri).c_str());
        if (d->placeIndex >= 0) {
            GtkDragSource *src = gtk_drag_source_new();
            gtk_drag_source_set_actions(src, GDK_ACTION_MOVE);
            std::string payload = "beluga-place:" + std::to_string(d->placeIndex);
            GValue v = G_VALUE_INIT;
            g_value_init(&v, G_TYPE_STRING);
            g_value_set_string(&v, payload.c_str());
            GdkContentProvider *content = gdk_content_provider_new_for_value(&v);
            gtk_drag_source_set_content(src, content);
            g_object_unref(content);
            g_value_unset(&v);
            gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(src));
        }
        gtk_list_box_append(GTK_LIST_BOX(list_), row);
        return row;
    };

    PlaceSection order[] = {PlaceSection::Places, PlaceSection::Remote, PlaceSection::Recent, PlaceSection::Search};
    const auto &places = Places::instance().list();
    bool trashFull = !trash::is_empty();
    for (PlaceSection sec : order) {
        if (hiddenSection(sec)) continue;
        for (size_t i = 0; i < places.size(); ++i) {
            const Place &p = places[i];
            if (section_for_uri(p.uri) != sec) continue;
            if (p.hidden && !s.showHiddenPlaces) continue;
            auto *d = new RowData{"place:" + std::to_string(i), p.uri, section_title(sec), (int)i, {}};
            std::string icon = p.icon;
            if (is_trash_uri(p.uri)) icon = trashFull ? "user-trash-full" : "user-trash";
            makeRow(d, p.label, icon, p.hidden, nullptr, nullptr);
        }
    }
    if (!hiddenSection(PlaceSection::Devices)) {
        for (auto &dev : DeviceManager::instance().devices()) {
            auto *d = new RowData{"device:" + dev.id, dev.mountPath.empty() ? std::string() : uri_for_path(dev.mountPath),
                                  section_title(PlaceSection::Devices), -1, dev.id};
            GtkWidget *bar = nullptr;
            if (!dev.mountPath.empty()) {
                guint64 total = 0, free = 0;
                Ref<GFile> f(g_file_new_for_path(dev.mountPath.c_str()));
                if (filesystem_space(f.get(), total, free)) {
                    bar = gtk_level_bar_new_for_interval(0, 1);
                    gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), 1.0 - (double)free / total);
                    gtk_widget_add_css_class(bar, "places-usage");
                    gtk_widget_set_tooltip_text(bar, printf_str(_("%s free of %s"), format_size(free).c_str(), format_size(total).c_str()).c_str());
                }
            }
            GtkWidget *eject = nullptr;
            if (dev.removable && (!dev.mountPath.empty() || dev.canEject || dev.canPowerOff)) {
                eject = gtk_button_new_from_icon_name("media-eject");
                gtk_widget_add_css_class(eject, "flat");
                gtk_widget_set_tooltip_text(eject, _("Safely Remove"));
                gtk_widget_set_valign(eject, GTK_ALIGN_CENTER);
                g_object_set_data_full(G_OBJECT(eject), "beluga-id", g_strdup(dev.id.c_str()), g_free);
                g_signal_connect(eject, "clicked", CB([](GtkButton *b, gpointer p) {
                    auto *pp = static_cast<PlacesPanel *>(p);
                    pp->menuKey_ = std::string("device:") + static_cast<const char *>(g_object_get_data(G_OBJECT(b), "beluga-id"));
                    pp->runRowAction("eject");
                }), this);
            }
            makeRow(d, dev.label, dev.icon, false, bar, eject);
        }
    }
    rebuilding_ = false;
    setCurrentUri(current_);
}

void PlacesPanel::setCurrentUri(const std::string &uri)
{
    current_ = uri;
    rebuilding_ = true;
    GtkListBoxRow *best = nullptr;
    size_t bestLen = 0;
    for (GtkWidget *c = gtk_widget_get_first_child(list_); c; c = gtk_widget_get_next_sibling(c)) {
        if (!GTK_IS_LIST_BOX_ROW(c)) continue;
        RowData *d = row_data(GTK_LIST_BOX_ROW(c));
        if (!d || d->uri.empty()) continue;
        std::string u = d->uri;
        while (u.size() > 1 && u.back() == '/' && !ends_with(u, ":///")) u.pop_back();
        bool match = uri == u || uri == d->uri || (!is_virtual_uri(u) && starts_with(uri, u) && uri.size() > u.size() && uri[u.size()] == '/');
        if (match && u.size() > bestLen) {
            best = GTK_LIST_BOX_ROW(c);
            bestLen = u.size();
        }
    }
    if (best) gtk_list_box_select_row(GTK_LIST_BOX(list_), best);
    else gtk_list_box_unselect_all(GTK_LIST_BOX(list_));
    rebuilding_ = false;
}

void PlacesPanel::activateRow(GtkListBoxRow *row, bool newTab)
{
    RowData *d = row_data(row);
    if (!d) return;
    if (!d->deviceId.empty() && d->uri.empty()) {
        std::string id = d->deviceId;
        DeviceManager::instance().mount(id, [this, newTab](bool ok, const std::string &result) {
            if (!ok) {
                if (onError) onError(printf_str(_("Could not mount the device: %s"), result.c_str()));
                return;
            }
            std::string u = uri_for_path(result);
            if (newTab && onOpenInNewTab) onOpenInNewTab(u);
            else if (onNavigate) onNavigate(u);
        });
        return;
    }
    if (newTab && onOpenInNewTab) onOpenInNewTab(d->uri);
    else if (onNavigate) onNavigate(d->uri);
}

void PlacesPanel::showMenu(GtkListBoxRow *row, double x, double y)
{
    RowData *d = row_data(row);
    menuKey_ = d ? d->key : std::string();
    GSimpleActionGroup *group = g_simple_action_group_new();
    static const char *names[] = {"open-new-tab", "open-new-window", "edit", "remove", "hide", "show-all", "hide-section",
                                  "add", "mount", "unmount", "eject", "empty-trash", "properties", nullptr};
    for (int i = 0; names[i]; ++i) {
        GSimpleAction *a = g_simple_action_new(names[i], nullptr);
        g_object_set_data_full(G_OBJECT(a), "beluga-name", g_strdup(names[i]), g_free);
        g_signal_connect(a, "activate", CB([](GSimpleAction *a, GVariant *, gpointer p) {
            static_cast<PlacesPanel *>(p)->runRowAction(static_cast<const char *>(g_object_get_data(G_OBJECT(a), "beluga-name")));
        }), this);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(a));
        g_object_unref(a);
    }
    gtk_widget_insert_action_group(root_, "places", G_ACTION_GROUP(group));
    g_object_unref(group);

    GMenu *m = g_menu_new();
    auto section = [&]() {
        GMenu *s = g_menu_new();
        g_menu_append_section(m, nullptr, G_MENU_MODEL(s));
        g_object_unref(s);
        return s;
    };
    if (d) {
        const Device *dev = d->deviceId.empty() ? nullptr : DeviceManager::instance().find(d->deviceId);
        GMenu *s = section();
        if (!d->uri.empty()) {
            g_menu_append(s, _("Open in New Tab"), "places.open-new-tab");
            g_menu_append(s, _("Open in New Window"), "places.open-new-window");
        }
        if (is_trash_uri(d->uri)) g_menu_append(section(), _("Empty Trash"), "places.empty-trash");
        if (dev) {
            GMenu *ds = section();
            if (dev->mountPath.empty()) g_menu_append(ds, _("Mount"), "places.mount");
            else if (dev->mountPath != "/") g_menu_append(ds, _("Unmount"), "places.unmount");
            if (dev->removable || dev->canEject || dev->canPowerOff) g_menu_append(ds, _("Safely Remove"), "places.eject");
        }
        if (d->placeIndex >= 0) {
            GMenu *ps = section();
            g_menu_append(ps, _("Edit…"), "places.edit");
            g_menu_append(ps, _("Remove"), "places.remove");
            const Place &p = Places::instance().list()[d->placeIndex];
            g_menu_append(ps, p.hidden ? _("Show") : _("Hide"), "places.hide");
        }
        g_menu_append(section(), _("Hide Section"), "places.hide-section");
        if (!d->uri.empty() && !is_virtual_uri(d->uri)) g_menu_append(section(), _("Properties"), "places.properties");
    }
    GMenu *s = section();
    g_menu_append(s, _("Add Entry…"), "places.add");
    g_menu_append(s, settings().showHiddenPlaces || !settings().hiddenPlaceSections.empty() ? _("Hide Hidden Entries") : _("Show All Entries"),
                  "places.show-all");
    // The menu hangs off the scrolled window, not the list box: GtkListBox can't remove a child that
    // isn't a row, so rebuilding or disposing it with the menu attached loops forever.
    graphene_point_t in = GRAPHENE_POINT_INIT((float)x, (float)y), pt = in;
    if (!gtk_widget_compute_point(list_, root_, &in, &pt)) pt = in;
    popup_menu(root_, G_MENU_MODEL(m), pt.x, pt.y);
    g_object_unref(m);
}

void PlacesPanel::runMenuAction(int placeIndex, const std::string &action)
{
    for (GtkWidget *c = gtk_widget_get_first_child(list_); c; c = gtk_widget_get_next_sibling(c)) {
        if (!GTK_IS_LIST_BOX_ROW(c)) continue;
        RowData *d = row_data(GTK_LIST_BOX_ROW(c));
        if (!d || d->placeIndex != placeIndex) continue;
        graphene_point_t in = GRAPHENE_POINT_INIT(4, 4), pt = GRAPHENE_POINT_INIT(0, 0);
        if (!gtk_widget_compute_point(c, list_, &in, &pt)) pt = GRAPHENE_POINT_INIT(0, 0);
        showMenu(GTK_LIST_BOX_ROW(c), pt.x, pt.y);
        runRowAction(action);
        return;
    }
}

void PlacesPanel::runRowAction(const std::string &action)
{
    RowData *d = nullptr;
    for (GtkWidget *c = gtk_widget_get_first_child(list_); c; c = gtk_widget_get_next_sibling(c))
        if (GTK_IS_LIST_BOX_ROW(c))
            if (RowData *r = row_data(GTK_LIST_BOX_ROW(c)); r && r->key == menuKey_) d = r;
    Settings &s = settings();
    if (action == "add") {
        Place p;
        p.uri = current_.empty() || is_virtual_uri(current_) ? file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get()) : current_;
        p.icon = Places::iconForUri(p.uri);
        p.label = basename_of(display_location(p.uri));
        edit_place_dialog(root_, p, true, [](const Place &np) { Places::instance().add(np); });
        return;
    }
    if (action == "show-all") {
        bool showing = s.showHiddenPlaces || !s.hiddenPlaceSections.empty();
        s.showHiddenPlaces = !showing;
        if (showing) {
            s.showHiddenPlaces = false;
        } else {
            s.hiddenPlaceSections.clear();
        }
        s.save();
        notify_settings_changed();
        return;
    }
    if (!d) return;
    std::string uri = d->uri;
    std::string id = d->deviceId;
    int index = d->placeIndex;
    std::string sectionTitle = d->section;
    if (action == "open-new-tab" && onOpenInNewTab) onOpenInNewTab(uri);
    else if (action == "open-new-window" && onOpenInNewWindow) onOpenInNewWindow(uri);
    else if (action == "properties" && onProperties) onProperties(uri);
    else if (action == "empty-trash") {
        auto run = [] { FileJob::emptyTrash()->start(); };
        if (s.confirmEmptyTrash)
            ask(root_, _("Empty the trash?"), _("All items in the trash will be deleted permanently."), {_("Cancel"), _("Empty Trash")}, 1, 0,
                [run](int r) { if (r == 1) run(); });
        else run();
    } else if (action == "edit" && index >= 0) {
        edit_place_dialog(root_, Places::instance().list()[index], false, [index](const Place &np) { Places::instance().update(index, np); });
    } else if (action == "remove" && index >= 0) {
        Places::instance().remove(index);
    } else if (action == "hide" && index >= 0) {
        Places::instance().setHidden(index, !Places::instance().list()[index].hidden);
    } else if (action == "hide-section") {
        for (PlaceSection sec : {PlaceSection::Places, PlaceSection::Remote, PlaceSection::Recent, PlaceSection::Search, PlaceSection::Devices})
            if (sectionTitle == section_title(sec)) s.hiddenPlaceSections.push_back(section_id(sec));
        s.save();
        notify_settings_changed();
    } else if (action == "mount") {
        DeviceManager::instance().mount(id, [this](bool ok, const std::string &r) {
            if (!ok && onError) onError(printf_str(_("Could not mount the device: %s"), r.c_str()));
        });
    } else if (action == "unmount" || action == "eject") {
        bool eject = action == "eject";
        auto done = [this, eject](bool ok, const std::string &r) {
            if (!ok && onError) onError(printf_str(eject ? _("Could not safely remove the device: %s") : _("Could not unmount the device: %s"), r.c_str()));
        };
        // Leave the device first if a view shows it.
        if (!uri.empty() && starts_with(current_, uri) && onNavigate) onNavigate(settings().homeUrl);
        if (eject) DeviceManager::instance().eject(id, done);
        else DeviceManager::instance().unmount(id, done);
    }
}

} // namespace beluga
