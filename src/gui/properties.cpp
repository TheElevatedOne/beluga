// SPDX-License-Identifier: GPL-2.0-or-later
#include "properties.hpp"

#include "app.hpp"
#include "dialogs.hpp"
#include "diskusage.hpp"
#include "fileops.hpp"
#include "gtkutil.hpp"
#include "mediainfo.hpp"
#include "openwith.hpp"
#include "search.hpp"
#include "settings.hpp"
#include "thumbnails.hpp"
#include "undo.hpp"

#include <memory>
#include <thread>

namespace beluga {

namespace {

struct Totals {
    guint64 size = 0;
    int files = 0, folders = 0;
};

void walk(GFile *f, Totals &t, const std::weak_ptr<void> &alive, int depth)
{
    if (alive.expired() || depth > 64) return;
    GFileInfo *info = g_file_query_info(f, "standard::type,standard::size,standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    if (!info) return;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        if (depth > 0) t.folders++;
        GFileEnumerator *en = g_file_enumerate_children(f, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
        if (en) {
            GFileInfo *ci;
            while ((ci = g_file_enumerator_next_file(en, nullptr, nullptr))) {
                GFile *c = g_file_get_child(f, g_file_info_get_name(ci));
                walk(c, t, alive, depth + 1);
                g_object_unref(c);
                g_object_unref(ci);
            }
            g_object_unref(en);
        }
    } else {
        if (depth > 0) t.files++;
        t.size += g_file_info_get_size(info);
    }
    g_object_unref(info);
}

GtkWidget *value_label(const std::string &text)
{
    GtkWidget *l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    // Without a minimum width a wrapping label measures one character wide, and the dialog asks
    // for thousands of pixels of height at its minimum width.
    gtk_label_set_width_chars(GTK_LABEL(l), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    gtk_widget_set_hexpand(l, TRUE);
    return l;
}

struct Grid {
    GtkWidget *grid;
    int row = 0;
    GtkWidget *add(const std::string &key, GtkWidget *value)
    {
        GtkWidget *k = gtk_label_new(key.c_str());
        gtk_label_set_xalign(GTK_LABEL(k), 1);
        gtk_label_set_yalign(GTK_LABEL(k), 0);
        gtk_widget_add_css_class(k, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), k, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), value, 1, row, 1, 1);
        row++;
        return value;
    }
    GtkWidget *text(const std::string &key, const std::string &value)
    {
        if (value.empty()) return nullptr;
        return add(key, value_label(value));
    }
};

Grid new_grid()
{
    Grid g{gtk_grid_new()};
    gtk_grid_set_row_spacing(GTK_GRID(g.grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g.grid), 10);
    gtk_widget_set_margin_start(g.grid, 12);
    gtk_widget_set_margin_end(g.grid, 12);
    gtk_widget_set_margin_top(g.grid, 12);
    gtk_widget_set_margin_bottom(g.grid, 12);
    return g;
}

struct State {
    std::vector<Ref<GFile>> files;
    Ref<GFileInfo> info;               // single item
    GtkWidget *window = nullptr;
    GtkWidget *nameEntry = nullptr;
    GtkWidget *perm[3] = {nullptr, nullptr, nullptr};
    GtkWidget *exec = nullptr;
    GtkWidget *recursive = nullptr;
    guint32 mode = 0;
    bool isDir = false;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

// 0 forbidden, 1 read, 2 read+write for one class (shift 6, 3, 0).
int class_level(guint32 mode, int shift)
{
    bool r = mode & (4u << shift), w = mode & (2u << shift);
    return w && r ? 2 : r ? 1 : 0;
}

guint32 apply_level(guint32 mode, int shift, int level, bool isDir)
{
    guint32 mask = 7u << shift;
    guint32 bits = level == 2 ? 6u : level == 1 ? 4u : 0u;
    if (isDir) bits |= level > 0 ? 1u : 0u;
    else bits |= (mode >> shift) & 1u;
    return (mode & ~mask) | (bits << shift);
}

// levels[i] < 0 leaves that class alone, so only what the user changed reaches the children.
void apply_recursive(GFile *f, const int levels[3], bool execChange, bool exec)
{
    GFileInfo *info = g_file_query_info(f, "standard::type,unix::mode,standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    if (!info) return;
    bool dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    bool link = g_file_info_get_file_type(info) == G_FILE_TYPE_SYMBOLIC_LINK;
    guint32 mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    guint32 nm = mode;
    for (int i = 0; i < 3; ++i)
        if (levels[i] >= 0) nm = apply_level(nm, 6 - 3 * i, levels[i], dir);
    if (!dir && execChange) nm = exec ? (nm | (((nm & 0444) >> 2))) : (nm & ~0111u);
    if (!link && nm != mode) g_file_set_attribute_uint32(f, G_FILE_ATTRIBUTE_UNIX_MODE, nm, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    if (dir) {
        GFileEnumerator *en = g_file_enumerate_children(f, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
        if (en) {
            GFileInfo *ci;
            while ((ci = g_file_enumerator_next_file(en, nullptr, nullptr))) {
                GFile *c = g_file_get_child(f, g_file_info_get_name(ci));
                apply_recursive(c, levels, execChange, exec);
                g_object_unref(c);
                g_object_unref(ci);
            }
            g_object_unref(en);
        }
    }
    g_object_unref(info);
}

// Audio, video and image details, as in the Information panel (FFmpeg, read on a worker thread).
GtkWidget *media_page(State *st, const std::string &mime)
{
    Grid g = new_grid();
    std::string path = take(g_file_get_path(st->files[0].get()));
    if (starts_with(mime, "image/") && !path.empty()) {
        int w = 0, h = 0;
        if (gdk_pixbuf_get_file_info(path.c_str(), &w, &h) && w > 0) g.text(_("Dimensions:"), printf_str("%d × %d", w, h));
    }
    if (path.empty() || !mediainfo::available()) {
        if (g.row == 0) g.text(_("Details:"), mediainfo::available() ? _("Only files on this computer can be read.")
                                                                     : _("Beluga was built without FFmpeg."));
        return g.grid;
    }
    GtkWidget *reading = g.text(_("Details:"), _("Reading…"));
    Ref<GtkWidget> grid = Ref<GtkWidget>::share(g.grid);
    Ref<GtkWidget> placeholder = Ref<GtkWidget>::share(reading);
    int startRow = g.row - 1;
    std::weak_ptr<void> alive = st->alive;
    bool image = starts_with(mime, "image/");
    std::thread([grid, placeholder, startRow, alive, path, image] {
        auto rows = mediainfo::probe(path);
        run_on_main([grid, placeholder, startRow, alive, rows, image] {
            if (alive.expired()) return;
            GtkWidget *gr = grid.get();
            if (GtkWidget *k = gtk_grid_get_child_at(GTK_GRID(gr), 0, startRow)) gtk_grid_remove(GTK_GRID(gr), k);
            gtk_grid_remove(GTK_GRID(gr), placeholder.get());
            Grid g{gr, startRow};
            for (auto &r : rows) {
                if (image && (r.first == _("Duration:") || r.first == _("Bit rate:"))) continue;   // a picture has neither
                g.text(r.first, r.second);
            }
            if (rows.empty() && startRow == 0) g.text(_("Details:"), _("No details found."));
        });
    }).detach();
    return g.grid;
}

GtkWidget *general_page(State *st, GtkWidget *parent)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(head, 12);
    gtk_widget_set_margin_end(head, 12);
    gtk_widget_set_margin_top(head, 12);
    Grid g = new_grid();
    bool single = st->files.size() == 1;
    GFileInfo *info = st->info.get();
    GtkWidget *icon;
    if (single && info && g_file_info_get_icon(info)) icon = gtk_image_new_from_gicon(g_file_info_get_icon(info));
    else icon = gtk_image_new_from_icon_name(single ? "text-x-generic" : "edit-select-all");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
    std::string dirPath = single && st->isDir ? take(g_file_get_path(st->files[0].get())) : std::string();
    if (!dirPath.empty()) {
        // A local folder's icon can be changed (saved in its .directory file, as Dolphin does).
        std::string own = folder_icon(dirPath);
        if (GIcon *gi = icon_from_string(own)) {
            gtk_image_set_from_gicon(GTK_IMAGE(icon), gi);
            g_object_unref(gi);
        }
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "flat");
        gtk_button_set_child(GTK_BUTTON(btn), icon);
        gtk_widget_set_tooltip_text(btn, _("Change the folder's icon"));
        g_object_set_data_full(G_OBJECT(btn), "beluga-path", g_strdup(dirPath.c_str()), g_free);
        g_signal_connect(btn, "clicked", CB([](GtkButton *b) {
            std::string path = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "beluga-path"));
            Ref<GtkWidget> img = Ref<GtkWidget>::share(gtk_button_get_child(b));
            choose_icon(GTK_WIDGET(b), folder_icon(path), [path, img](const std::string &chosen) {
                GError *err = nullptr;
                if (!set_folder_icon(path, chosen, &err)) {
                    show_error(img.get(), _("Could not change the icon."), err ? err->message : "");
                    g_clear_error(&err);
                    return;
                }
                GIcon *gi = chosen.empty() ? g_themed_icon_new("folder") : icon_from_string(chosen);
                gtk_image_set_from_gicon(GTK_IMAGE(img.get()), gi);
                g_object_unref(gi);
                // The views showing the folder's parent pick up the new icon.
                std::string parent = file_uri(Ref<GFile>(g_file_get_parent(Ref<GFile>(g_file_new_for_path(path.c_str())).get())).get());
                App::instance().reloadLocation(parent);
            });
        }), nullptr);
        gtk_box_append(GTK_BOX(head), btn);
    } else {
        gtk_box_append(GTK_BOX(head), icon);
    }
    if (single && info) {
        st->nameEntry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(st->nameEntry), g_file_info_get_display_name(info));
        gtk_widget_set_hexpand(st->nameEntry, TRUE);
        gtk_widget_set_valign(st->nameEntry, GTK_ALIGN_CENTER);
        bool canRename = !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME)
                      || g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME);
        gtk_editable_set_editable(GTK_EDITABLE(st->nameEntry), canRename);
        gtk_entry_set_activates_default(GTK_ENTRY(st->nameEntry), TRUE);
        gtk_box_append(GTK_BOX(head), st->nameEntry);
    } else {
        GtkWidget *l = gtk_label_new(printf_str(_("%zu items"), st->files.size()).c_str());
        gtk_widget_add_css_class(l, "title-3");
        gtk_box_append(GTK_BOX(head), l);
    }
    gtk_box_append(GTK_BOX(box), head);

    std::string mime = info ? content_type_of(info) : std::string();
    if (single && info) {
        g.text(_("Type:"), st->isDir ? std::string(_("Folder")) : take(g_content_type_get_description(mime.c_str())) + " (" + mime + ")");
    }
    Ref<GFile> parentDir(g_file_get_parent(st->files[0].get()));
    if (parentDir) g.text(_("Location:"), display_location(parentDir.get()));
    GtkWidget *sizeLabel = g.add(_("Size:"), value_label(_("Calculating…")));
    GtkWidget *contentsLabel = nullptr;
    bool anyDir = st->isDir || !single;
    if (anyDir) contentsLabel = g.add(_("Contents:"), value_label(_("Calculating…")));
    if (single && info) {
        auto t = [&](const char *a) { return (gint64)g_file_info_get_attribute_uint64(info, a); };
        g.text(_("Created:"), format_date(t(G_FILE_ATTRIBUTE_TIME_CREATED), DateStyle::Long));
        g.text(_("Modified:"), format_date(t(G_FILE_ATTRIBUTE_TIME_MODIFIED), DateStyle::Long));
        g.text(_("Accessed:"), format_date(t(G_FILE_ATTRIBUTE_TIME_ACCESS), DateStyle::Long));
        if (g_file_info_get_is_symlink(info)) g.text(_("Points to:"), str(g_file_info_get_symlink_target(info)));
        if (!st->isDir) {
            // The program's icon and name, centred with the Change button.
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            GAppInfo *app = g_app_info_get_default_for_type(mime.c_str(), FALSE);
            if (app && g_app_info_get_icon(app)) {
                GtkWidget *img = gtk_image_new_from_gicon(g_app_info_get_icon(app));
                gtk_image_set_pixel_size(GTK_IMAGE(img), 16);
                gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
                gtk_box_append(GTK_BOX(row), img);
            }
            GtkWidget *name = value_label(app ? g_app_info_get_display_name(app) : _("No default application"));
            gtk_widget_set_valign(name, GTK_ALIGN_CENTER);
            if (app) g_object_unref(app);
            gtk_box_append(GTK_BOX(row), name);
            GtkWidget *change = gtk_button_new_with_mnemonic(_("C_hange…"));
            g_object_set_data_full(G_OBJECT(change), "beluga-mime", g_strdup(mime.c_str()), g_free);
            g_object_set_data_full(G_OBJECT(change), "beluga-file", g_object_ref(st->files[0].get()), g_object_unref);
            g_signal_connect(change, "clicked", CB([](GtkButton *b) {
                show_open_with_dialog(GTK_WIDGET(b), {Ref<GFile>::share(G_FILE(g_object_get_data(G_OBJECT(b), "beluga-file")))},
                                      static_cast<const char *>(g_object_get_data(G_OBJECT(b), "beluga-mime")));
            }), nullptr);
            gtk_widget_set_valign(change, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(row), change);
            GtkWidget *key = gtk_grid_get_child_at(GTK_GRID(g.grid), 0, g.row - 1);
            (void)key;
            g.add(_("Opens with:"), row);
            // Its label in the middle of the row, not at the top.
            if (GtkWidget *k = gtk_grid_get_child_at(GTK_GRID(g.grid), 0, g.row - 1)) gtk_label_set_yalign(GTK_LABEL(k), 0.5f);
        }
    }
    guint64 total = 0, free = 0;
    Ref<GFile> fsTarget = st->isDir ? st->files[0] : parentDir;
    if (fsTarget && filesystem_space(fsTarget.get(), total, free))
        g.text(_("Free space:"), printf_str(_("%s free of %s (%d%% used)"), format_size(free).c_str(), format_size(total).c_str(),
                                            (int)(100.0 * (total - free) / total)));
    gtk_box_append(GTK_BOX(box), g.grid);

    // Size and contents in a worker thread.
    std::weak_ptr<void> alive = st->alive;
    auto files = st->files;
    Ref<GtkWidget> sl = Ref<GtkWidget>::share(sizeLabel);
    Ref<GtkWidget> cl = contentsLabel ? Ref<GtkWidget>::share(contentsLabel) : Ref<GtkWidget>();
    std::thread([alive, files, sl, cl] {
        auto t = std::make_shared<Totals>();
        for (auto &f : files) {
            Totals one;
            // Local folders with the parallel walker; remote ones through GIO.
            std::string path = take(g_file_get_path(f.get()));
            struct stat ps;
            if (!path.empty() && g_file_is_native(f.get()) && lstat(path.c_str(), &ps) == 0 && S_ISDIR(ps.st_mode)) {
                du::Totals r = du::totals(path, true, 0, -1, [alive] { return alive.expired(); });
                one.size = r.size;
                one.files = (int)r.files;
                one.folders = (int)r.folders;
            } else {
                walk(f.get(), one, alive, 0);
            }
            t->size += one.size;
            t->files += one.files;
            t->folders += one.folders;
            GFileInfo *i = g_file_query_info(f.get(), "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
            if (i && files.size() > 1) {
                if (g_file_info_get_file_type(i) == G_FILE_TYPE_DIRECTORY) t->folders++;
                else t->files++;
            }
            if (i) g_object_unref(i);
        }
        run_on_main([alive, t, sl, cl] {
            if (alive.expired()) return;
            gtk_label_set_text(GTK_LABEL(sl.get()), printf_str("%s (%s)", format_size(t->size).c_str(),
                                                               printf_str(g_dngettext(GETTEXT_PACKAGE, "%llu byte", "%llu bytes", t->size),
                                                                          (unsigned long long)t->size).c_str()).c_str());
            if (cl) gtk_label_set_text(GTK_LABEL(cl.get()), format_files_folders(t->files, t->folders).c_str());
        });
    }).detach();
    (void)parent;
    return box;
}

GtkWidget *permissions_page(State *st)
{
    Grid g = new_grid();
    GFileInfo *info = st->info.get();
    st->mode = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_MODE);
    const char *fileOpts[] = {_("Forbidden"), _("Can View"), _("Can View & Modify"), nullptr};
    const char *dirOpts[] = {_("Forbidden"), _("Can View Content"), _("Can View & Modify Content"), nullptr};
    const char *titles[] = {_("Owner:"), _("Group:"), _("Others:")};
    bool own = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_UNIX_UID) == getuid() || getuid() == 0;
    for (int i = 0; i < 3; ++i) {
        st->perm[i] = gtk_drop_down_new_from_strings(st->isDir ? dirOpts : fileOpts);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(st->perm[i]), class_level(st->mode, 6 - 3 * i));
        gtk_widget_set_sensitive(st->perm[i], own);
        g.add(titles[i], st->perm[i]);
    }
    if (!st->isDir) {
        st->exec = gtk_check_button_new_with_mnemonic(_("Is _executable"));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(st->exec), (st->mode & 0100) != 0);
        gtk_widget_set_sensitive(st->exec, own);
        g.add("", st->exec);
    } else {
        st->recursive = gtk_check_button_new_with_mnemonic(_("Apply changes to all _subfolders and their contents"));
        gtk_widget_set_sensitive(st->recursive, own);
        g.add("", st->recursive);
    }
    g.text(_("User:"), str(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_USER)));
    g.text(_("Group:"), str(g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_OWNER_GROUP)));
    g.text(_("Mode:"), format_permissions(st->mode, 2));
    return g.grid;
}

GtkWidget *checksums_page(State *st)
{
    Grid g = new_grid();
    struct Algo { const char *name; GChecksumType type; };
    static const Algo algos[] = {{"MD5", G_CHECKSUM_MD5}, {"SHA1", G_CHECKSUM_SHA1}, {"SHA256", G_CHECKSUM_SHA256}, {"SHA512", G_CHECKSUM_SHA512}};
    for (const Algo &a : algos) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *val = value_label("");
        gtk_label_set_wrap_mode(GTK_LABEL(val), PANGO_WRAP_CHAR);
        gtk_widget_add_css_class(val, "monospace");
        GtkWidget *calc = gtk_button_new_with_label(_("Calculate"));
        gtk_box_append(GTK_BOX(row), calc);
        gtk_box_append(GTK_BOX(row), val);
        g_object_set_data(G_OBJECT(calc), "beluga-val", val);
        g_object_set_data(G_OBJECT(calc), "beluga-type", GINT_TO_POINTER((int)a.type));
        g_object_set_data_full(G_OBJECT(calc), "beluga-file", g_object_ref(st->files[0].get()), g_object_unref);
        g_object_set_data(G_OBJECT(calc), "beluga-state", st);
        g_signal_connect(calc, "clicked", CB([](GtkButton *b) {
            auto *st = static_cast<State *>(g_object_get_data(G_OBJECT(b), "beluga-state"));
            Ref<GtkWidget> val = Ref<GtkWidget>::share(GTK_WIDGET(g_object_get_data(G_OBJECT(b), "beluga-val")));
            auto type = (GChecksumType)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "beluga-type"));
            Ref<GFile> f = Ref<GFile>::share(G_FILE(g_object_get_data(G_OBJECT(b), "beluga-file")));
            gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
            gtk_label_set_text(GTK_LABEL(val.get()), _("Calculating…"));
            std::weak_ptr<void> alive = st->alive;
            std::thread([alive, val, type, f] {
                GChecksum *cs = g_checksum_new(type);
                GFileInputStream *in = g_file_read(f.get(), nullptr, nullptr);
                std::string result;
                if (in) {
                    std::vector<guchar> buf(1 << 16);
                    gssize n;
                    while (!alive.expired() && (n = g_input_stream_read(G_INPUT_STREAM(in), buf.data(), buf.size(), nullptr, nullptr)) > 0)
                        g_checksum_update(cs, buf.data(), n);
                    g_object_unref(in);
                    result = g_checksum_get_string(cs);
                } else {
                    result = _("Could not read the file.");
                }
                g_checksum_free(cs);
                run_on_main([alive, val, result] {
                    if (!alive.expired()) gtk_label_set_text(GTK_LABEL(val.get()), result.c_str());
                });
            }).detach();
        }), nullptr);
        g.add(a.name, row);
    }
    GtkWidget *verify = gtk_entry_new();
    g_object_set(verify, "placeholder-text", _("Paste a checksum to compare"), nullptr);
    GtkWidget *result = value_label("");
    g.add(_("Verify:"), verify);
    g.add("", result);
    g_object_set_data(G_OBJECT(verify), "beluga-result", result);
    g_object_set_data(G_OBJECT(verify), "beluga-grid", g.grid);
    g_signal_connect(verify, "changed", CB([](GtkEditable *e) {
        std::string want = casefold(trim(gtk_editable_get_text(e)));
        auto *res = GTK_LABEL(g_object_get_data(G_OBJECT(e), "beluga-result"));
        auto *grid = GTK_WIDGET(g_object_get_data(G_OBJECT(e), "beluga-grid"));
        if (want.empty()) {
            gtk_label_set_text(res, "");
            return;
        }
        bool match = false;
        for (int r = 0; r < 4; ++r) {
            GtkWidget *row = gtk_grid_get_child_at(GTK_GRID(grid), 1, r);
            GtkWidget *val = row ? gtk_widget_get_last_child(row) : nullptr;
            if (val && GTK_IS_LABEL(val) && casefold(gtk_label_get_text(GTK_LABEL(val))) == want) match = true;
        }
        gtk_label_set_text(res, match ? _("The checksums match.") : _("No calculated checksum matches. Calculate the matching type first."));
    }), nullptr);
    return g.grid;
}

void apply(State *st, GtkWidget *parent)
{
    if (st->nameEntry && st->info) {
        std::string newName = trim(gtk_editable_get_text(GTK_EDITABLE(st->nameEntry)));
        std::string old = g_file_info_get_display_name(st->info.get());
        if (!newName.empty() && newName != old) {
            Ref<GFile> renamed;
            GError *err = nullptr;
            std::string oldUri = file_uri(st->files[0].get());
            if (rename_file(st->files[0].get(), newName, renamed, &err)) {
                UndoCommand c;
                c.type = UndoCommand::Rename;
                c.pairs.emplace_back(oldUri, file_uri(renamed.get()));
                UndoManager::instance().push(c);
                st->files[0] = renamed;
            } else {
                show_error(parent, printf_str(_("Could not rename “%s”."), old.c_str()), err->message);
                g_error_free(err);
            }
        }
    }
    if (st->perm[0]) {
        int levels[3];
        for (int i = 0; i < 3; ++i) levels[i] = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(st->perm[i]));
        bool recursive = st->recursive && gtk_check_button_get_active(GTK_CHECK_BUTTON(st->recursive));
        guint32 nm = st->mode;
        for (int i = 0; i < 3; ++i)
            if (levels[i] != class_level(st->mode, 6 - 3 * i)) nm = apply_level(nm, 6 - 3 * i, levels[i], st->isDir);
        if (st->exec) {
            bool on = gtk_check_button_get_active(GTK_CHECK_BUTTON(st->exec));
            nm = on ? (nm | ((nm & 0444) >> 2)) : (nm & ~0111u);
        }
        GError *err = nullptr;
        if (nm != st->mode && !g_file_set_attribute_uint32(st->files[0].get(), G_FILE_ATTRIBUTE_UNIX_MODE, nm, G_FILE_QUERY_INFO_NONE, nullptr, &err)) {
            show_error(parent, _("Could not change the permissions."), err->message);
            g_error_free(err);
        } else if (recursive) {
            for (int i = 0; i < 3; ++i)
                if (levels[i] == class_level(st->mode, 6 - 3 * i)) levels[i] = -1;
            Ref<GFile> f = st->files[0];
            if (levels[0] >= 0 || levels[1] >= 0 || levels[2] >= 0)
                std::thread([f, levels0 = levels[0], levels1 = levels[1], levels2 = levels[2]] {
                int lv[3] = {levels0, levels1, levels2};
                apply_recursive(f.get(), lv, false, false);
            }).detach();
        }
    }
}

} // namespace

void show_properties_dialog(GtkWidget *parent, const std::vector<Ref<GFile>> &files)
{
    if (files.empty()) return;
    auto *st = new State;
    st->files = files;
    if (files.size() == 1) {
        st->info = Ref<GFileInfo>(g_file_query_info(files[0].get(), kItemAttributes, G_FILE_QUERY_INFO_NONE, nullptr, nullptr));
        st->isDir = st->info && g_file_info_get_file_type(st->info.get()) == G_FILE_TYPE_DIRECTORY;
    }
    GtkWidget *win = gtk_window_new();
    st->window = win;
    g_object_set_data_full(G_OBJECT(win), "beluga-state", st, [](gpointer p) { delete static_cast<State *>(p); });
    std::string title = files.size() == 1 ? printf_str(_("Properties for %s"), take(g_file_get_basename(files[0].get())).c_str())
                                          : std::string(_("Properties"));
    gtk_window_set_title(GTK_WINDOW(win), title.c_str());
    if (GtkWindow *pw = window_of(parent)) gtk_window_set_transient_for(GTK_WINDOW(win), pw);
    gtk_window_set_default_size(GTK_WINDOW(win), 480, -1);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(win), box);
    GtkWidget *nb = gtk_notebook_new();
    gtk_widget_set_vexpand(nb, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), general_page(st, parent), gtk_label_new(_("General")));
    if (st->info && g_file_info_has_attribute(st->info.get(), G_FILE_ATTRIBUTE_UNIX_MODE))
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), permissions_page(st), gtk_label_new(_("Permissions")));
    if (st->info && g_file_info_get_file_type(st->info.get()) == G_FILE_TYPE_REGULAR) {
        std::string mime = content_type_of(st->info.get());
        if (st->files.size() == 1 && (starts_with(mime, "audio/") || starts_with(mime, "video/") || starts_with(mime, "image/")))
            gtk_notebook_append_page(GTK_NOTEBOOK(nb), media_page(st, mime), gtk_label_new(_("Details")));
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), checksums_page(st), gtk_label_new(_("Checksums")));
    }
    // Scrolled, so a long name or many checksums never make the dialog taller than the screen.
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 700);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), nb);
    gtk_box_append(GTK_BOX(box), scroll);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_widget_set_margin_end(buttons, 12);
    GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
    GtkWidget *ok = gtk_button_new_with_mnemonic(_("_OK"));
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), cancel);
    gtk_box_append(GTK_BOX(buttons), ok);
    gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_default_widget(GTK_WINDOW(win), ok);
    g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_destroy), win);
    g_object_set_data_full(G_OBJECT(ok), "beluga-parent", parent ? g_object_ref(parent) : nullptr, parent ? g_object_unref : nullptr);
    g_signal_connect(ok, "clicked", CB([](GtkButton *b, gpointer w) {
        auto *st = static_cast<State *>(g_object_get_data(G_OBJECT(w), "beluga-state"));
        apply(st, GTK_WIDGET(g_object_get_data(G_OBJECT(b), "beluga-parent")));
        gtk_window_destroy(GTK_WINDOW(w));
    }), win);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer w) -> gboolean {
        if (keyval == GDK_KEY_Escape) {
            gtk_window_destroy(GTK_WINDOW(w));
            return TRUE;
        }
        return FALSE;
    }), win);
    gtk_widget_add_controller(win, key);
    gtk_window_present(GTK_WINDOW(win));
}

} // namespace beluga
