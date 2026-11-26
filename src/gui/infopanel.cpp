// SPDX-License-Identifier: GPL-2.0-or-later
#include "infopanel.hpp"

#include "dirsize.hpp"
#include "fileops.hpp"
#include "mediainfo.hpp"
#include "workqueue.hpp"
#include "gtkutil.hpp"
#include "search.hpp"
#include "settings.hpp"
#include "thumbnails.hpp"

namespace beluga {

InfoPanel::InfoPanel()
{
    alive_ = std::make_shared<int>(0);
    root_ = gtk_scrolled_window_new();
    g_object_ref_sink(root_);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(root_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(root_, "beluga-info");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 10);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    preview_ = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(preview_), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(preview_), TRUE);
    gtk_widget_set_size_request(preview_, -1, 192);
    gtk_box_append(GTK_BOX(box), preview_);
    media_ = gtk_video_new();
    gtk_widget_set_size_request(media_, -1, 160);
    gtk_widget_set_visible(media_, FALSE);
    gtk_box_append(GTK_BOX(box), media_);
    name_ = gtk_label_new(nullptr);
    gtk_label_set_wrap(GTK_LABEL(name_), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(name_), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_justify(GTK_LABEL(name_), GTK_JUSTIFY_CENTER);
    gtk_label_set_selectable(GTK_LABEL(name_), TRUE);
    gtk_widget_add_css_class(name_, "heading");
    gtk_box_append(GTK_BOX(box), name_);
    grid_ = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid_), 8);
    gtk_box_append(GTK_BOX(box), grid_);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), box);
}

InfoPanel::~InfoPanel()
{
    alive_.reset();
    if (renderSource_) g_source_remove(renderSource_);
    gtk_video_set_file(GTK_VIDEO(media_), nullptr);
    g_object_unref(root_);
}

void InfoPanel::queueRender()
{
    if (renderSource_) return;
    renderSource_ = g_timeout_add(60, [](gpointer d) -> gboolean {
        auto *p = static_cast<InfoPanel *>(d);
        p->renderSource_ = 0;
        p->render();
        return G_SOURCE_REMOVE;
    }, this);
}

void InfoPanel::showFolder(const std::string &uri)
{
    files_.clear();
    folder_ = is_virtual_uri(uri) ? Ref<GFile>() : Ref<GFile>(g_file_new_for_uri(uri.c_str()));
    virtualTitle_.clear();
    if (is_trash_uri(uri)) virtualTitle_ = _("Trash");
    else if (is_recent_uri(uri)) virtualTitle_ = _("Recent Files");
    else if (is_search_uri(uri)) {
        SearchQuery q;
        SearchQuery::fromUri(uri, q);
        virtualTitle_ = q.title();
    }
    queueRender();
}

void InfoPanel::showItems(const std::vector<BelugaItem *> &items)
{
    files_.clear();
    for (BelugaItem *it : items) files_.push_back(item_data(it).file);
    queueRender();
}

void InfoPanel::showHovered(BelugaItem *item)
{
    hovered_ = item ? item_data(item).file : Ref<GFile>();
    queueRender();
}

void InfoPanel::addRow(const std::string &key, const std::string &value)
{
    if (value.empty()) return;
    GtkWidget *k = gtk_label_new(key.c_str());
    gtk_label_set_xalign(GTK_LABEL(k), 1);
    gtk_label_set_yalign(GTK_LABEL(k), 0);
    gtk_widget_add_css_class(k, "dim-label");
    GtkWidget *v = gtk_label_new(value.c_str());
    gtk_label_set_xalign(GTK_LABEL(v), 0);
    gtk_label_set_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(v), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_widget_set_hexpand(v, TRUE);
    gtk_grid_attach(GTK_GRID(grid_), k, 0, rows_, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_), v, 1, rows_, 1, 1);
    rows_++;
}

void InfoPanel::setPreview(GFile *file, GFileInfo *info, const std::string &mime, gint64 mtime, guint64 size)
{
    int scale = gtk_widget_get_scale_factor(root_);
    Ref<GIcon> icon = info && g_file_info_get_icon(info) ? Ref<GIcon>::share(g_file_info_get_icon(info))
                                                           : Ref<GIcon>(g_themed_icon_new("text-x-generic"));
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(root_));
    GtkIconPaintable *ip = gtk_icon_theme_lookup_by_gicon(theme, icon.get(), 128, scale, GTK_TEXT_DIR_NONE, GtkIconLookupFlags(0));
    gtk_picture_set_paintable(GTK_PICTURE(preview_), GDK_PAINTABLE(ip));
    g_object_unref(ip);
    if (!settings().infoPreviews || !file || (info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)) return;
    thumbs::Request r;
    r.uri = file_uri(file);
    r.path = take(g_file_get_path(file));
    r.mime = mime;
    r.mtime = mtime;
    r.size = size;
    r.pixels = 256 * scale;
    if (!thumbs::supported(mime, !r.path.empty()) || !thumbs::allowed(r)) return;
    if (GdkPixbuf *p = thumbs::cached(r)) {
        GdkTexture *t = texture_from_pixbuf(p);
        gtk_picture_set_paintable(GTK_PICTURE(preview_), GDK_PAINTABLE(t));
        g_object_unref(t);
        g_object_unref(p);
        return;
    }
    std::string uri = r.uri;
    thumbs::request(r, alive_, [this, uri](GdkPixbuf *p) {
        Ref<GFile> shown = hovered_ ? hovered_ : (files_.size() == 1 ? files_[0] : folder_);
        if (!p || !shown || file_uri(shown.get()) != uri) return;
        GdkTexture *t = texture_from_pixbuf(p);
        gtk_picture_set_paintable(GTK_PICTURE(preview_), GDK_PAINTABLE(t));
        g_object_unref(t);
    });
}

void InfoPanel::render()
{
    while (GtkWidget *c = gtk_widget_get_first_child(grid_)) gtk_grid_remove(GTK_GRID(grid_), c);
    rows_ = 0;
    gtk_video_set_file(GTK_VIDEO(media_), nullptr);
    gtk_widget_set_visible(media_, FALSE);
    alive_ = std::make_shared<int>(0);

    std::vector<Ref<GFile>> shown;
    if (hovered_ && settings().infoShowHovered) shown.push_back(hovered_);
    else if (!files_.empty()) shown = files_;
    else if (folder_) shown.push_back(folder_);
    if (shown.empty()) {
        gtk_label_set_text(GTK_LABEL(name_), virtualTitle_.c_str());
        gtk_picture_set_paintable(GTK_PICTURE(preview_), nullptr);
        if (!virtualTitle_.empty()) {
            GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(root_));
            const char *icon = virtualTitle_ == _("Trash") ? "user-trash" : virtualTitle_ == _("Recent Files") ? "document-open-recent" : "system-search";
            GtkIconPaintable *ip = gtk_icon_theme_lookup_icon(theme, icon, nullptr, 128, gtk_widget_get_scale_factor(root_),
                                                              GTK_TEXT_DIR_NONE, GtkIconLookupFlags(0));
            gtk_picture_set_paintable(GTK_PICTURE(preview_), GDK_PAINTABLE(ip));
            g_object_unref(ip);
        }
        return;
    }
    if (shown.size() > 1) {
        int files = 0, folders = 0;
        guint64 total = 0;
        for (auto &f : shown) {
            GFileInfo *info = g_file_query_info(f.get(), "standard::type,standard::size", G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
            if (!info) continue;
            if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) folders++;
            else {
                files++;
                total += g_file_info_get_size(info);
            }
            g_object_unref(info);
        }
        gtk_label_set_text(GTK_LABEL(name_), printf_str(_("%zu items selected"), shown.size()).c_str());
        GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(root_));
        GtkIconPaintable *ip = gtk_icon_theme_lookup_icon(theme, "edit-select-all", nullptr, 128, gtk_widget_get_scale_factor(root_),
                                                          GTK_TEXT_DIR_NONE, GtkIconLookupFlags(0));
        gtk_picture_set_paintable(GTK_PICTURE(preview_), GDK_PAINTABLE(ip));
        g_object_unref(ip);
        addRow(_("Contents:"), format_files_folders(files, folders));
        if (files) addRow(_("Size:"), format_size(total));
        return;
    }
    GFile *f = shown[0].get();
    GFileInfo *info = g_file_query_info(f, kItemAttributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    if (!info) {
        gtk_label_set_text(GTK_LABEL(name_), display_location(f).c_str());
        return;
    }
    const char *dn = g_file_info_get_display_name(info);
    gtk_label_set_text(GTK_LABEL(name_), dn ? dn : "");
    bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    std::string mime = content_type_of(info);
    auto t64 = [&](const char *a) { return (gint64)g_file_info_get_attribute_uint64(info, a); };
    gint64 mtime = t64(G_FILE_ATTRIBUTE_TIME_MODIFIED);
    guint64 size = g_file_info_get_size(info);
    setPreview(f, info, mime, mtime, size);
    DateStyle ds = settings().infoLongDates ? DateStyle::Long : DateStyle::Short;
    addRow(_("Type:"), dir ? std::string(_("Folder")) : take(g_content_type_get_description(mime.c_str())));
    if (dir) {
        addRow(_("Contains:"), _("Counting…"));
        GtkWidget *valueLabel = gtk_grid_get_child_at(GTK_GRID(grid_), 1, rows_ - 1);
        Ref<GtkWidget> keep = Ref<GtkWidget>::share(valueLabel);
        dirsize::request(file_uri(f), false, true, settings().recursiveDirSizeLimit, alive_, [keep](int count, guint64 bytes) {
            std::string t = count < 0 ? std::string(_("Unknown")) : format_items(count) + ", " + format_size(bytes);
            gtk_label_set_text(GTK_LABEL(keep.get()), t.c_str());
        });
    } else {
        addRow(_("Size:"), format_size(size));
    }
    addRow(_("Modified:"), format_date(mtime, ds));
    addRow(_("Created:"), format_date(t64(G_FILE_ATTRIBUTE_TIME_CREATED), ds));
    addRow(_("Accessed:"), format_date(t64(G_FILE_ATTRIBUTE_TIME_ACCESS), ds));
    Ref<GFile> parent(g_file_get_parent(f));
    if (parent) addRow(_("Location:"), display_location(parent.get()));
    if (g_file_info_get_is_symlink(info)) addRow(_("Points to:"), str(g_file_info_get_symlink_target(info)));
    if (g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_UNIX_MODE))
        addRow(_("Permissions:"), format_permissions(g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE), settings().permissionsFormat));
    addRow(_("Owner:"), str(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER)));
    addRow(_("Group:"), str(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP)));
    char *path = g_file_get_path(f);
    if (path && starts_with(mime, "image/")) {
        int w = 0, h = 0;
        if (gdk_pixbuf_get_file_info(path, &w, &h) && w > 0) addRow(_("Dimensions:"), printf_str("%d × %d", w, h));
    }
    if (path && !dir && (starts_with(mime, "audio/") || starts_with(mime, "video/"))) {
        gtk_video_set_autoplay(GTK_VIDEO(media_), settings().infoAutoPlay);
        gtk_video_set_file(GTK_VIDEO(media_), f);
        gtk_widget_set_visible(media_, TRUE);
        if (mediainfo::available()) {
            // What ffprobe shows, read with FFmpeg on a worker thread (newest file first; stale ones are skipped).
            static WorkQueue *probes = new WorkQueue(1, true);
            std::weak_ptr<int> alive = alive_;
            std::string file = path;
            probes->push([this, alive, file] {
                if (alive.expired()) return;
                auto rows = mediainfo::probe(file);
                run_on_main([this, alive, rows] {
                    if (alive.expired()) return;
                    for (auto &r : rows) addRow(r.first, r.second);
                });
            });
        } else if (GtkMediaStream *stream = gtk_video_get_media_stream(GTK_VIDEO(media_))) {
            addRow(_("Duration:"), "…");
            GtkWidget *value = gtk_grid_get_child_at(GTK_GRID(grid_), 1, rows_ - 1);
            auto update = [](GtkMediaStream *st, GParamSpec *, gpointer label) {
                gint64 d = gtk_media_stream_get_duration(st);
                if (d > 0) gtk_label_set_text(GTK_LABEL(label), format_duration(d / 1e6).c_str());
            };
            g_signal_connect_object(stream, "notify::duration", G_CALLBACK(+update), value, GConnectFlags(0));
            update(stream, nullptr, value);
        }
    }
    g_free(path);
    g_object_unref(info);
}

} // namespace beluga
