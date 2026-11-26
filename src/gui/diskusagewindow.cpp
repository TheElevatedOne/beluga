// SPDX-License-Identifier: GPL-2.0-or-later
#include "diskusagewindow.hpp"

#include "app.hpp"
#include "diskusage.hpp"
#include "fileops.hpp"
#include "gtkutil.hpp"
#include "mainwindow.hpp"
#include "settings.hpp"
#include "view.hpp"
#include "viewcontainer.hpp"

#include <algorithm>
#include <memory>
#include <thread>

// A list item for one node of the tree. The tree itself belongs to the window's state.
G_BEGIN_DECLS
G_DECLARE_FINAL_TYPE(BelugaDuItem, beluga_du_item, BELUGA, DU_ITEM, GObject)
G_END_DECLS

struct _BelugaDuItem {
    GObject parent_instance;
    beluga::du::Node *node;
};

G_DEFINE_FINAL_TYPE(BelugaDuItem, beluga_du_item, G_TYPE_OBJECT)

static void beluga_du_item_class_init(BelugaDuItemClass *) {}
static void beluga_du_item_init(BelugaDuItem *self) { self->node = nullptr; }

namespace beluga {

namespace {

enum Column { ColName, ColSize, ColShare, ColFiles };

struct DuWin {
    GtkWidget *win = nullptr, *pathLabel = nullptr, *totalLabel = nullptr, *status = nullptr, *spinner = nullptr;
    GtkWidget *apparent = nullptr, *oneFs = nullptr, *stop = nullptr, *view = nullptr;
    std::string path;
    MainWindow *mw = nullptr;
    std::weak_ptr<void> mwAlive;
    std::shared_ptr<du::Node> tree;          // the whole scan (maybe of a folder above this one)
    du::Node *root = nullptr;                // the folder shown, inside tree
    std::shared_ptr<du::Scanner> scanner;   // the running scan; shared with its thread
    bool busy = false;
    bool rootApparent = false;              // how the shown tree was counted
    guint timer = 0;
    gint64 started = 0;
    std::shared_ptr<void> life = std::make_shared<char>(0);

    ~DuWin()
    {
        if (scanner) scanner->cancel();
        if (timer) g_source_remove(timer);
    }
};

DuWin *state_of(GtkWidget *w) { return w ? static_cast<DuWin *>(g_object_get_data(G_OBJECT(w), "beluga-du")) : nullptr; }

BelugaDuItem *item_new(du::Node *n)
{
    auto *i = BELUGA_DU_ITEM(g_object_new(beluga_du_item_get_type(), nullptr));
    i->node = n;
    return i;
}

GListModel *children_of(gpointer item, gpointer)
{
    du::Node *n = BELUGA_DU_ITEM(item)->node;
    if (!n->dir || n->children.empty()) return nullptr;
    GListStore *s = g_list_store_new(beluga_du_item_get_type());
    for (auto &c : n->children) {
        BelugaDuItem *i = item_new(c.get());
        g_list_store_append(s, i);
        g_object_unref(i);
    }
    return G_LIST_MODEL(s);
}

du::Node *node_of(GtkListItem *li)
{
    auto *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(li));
    if (!row) return nullptr;
    gpointer item = gtk_tree_list_row_get_item(row);
    du::Node *n = item ? BELUGA_DU_ITEM(item)->node : nullptr;
    if (item) g_object_unref(item);
    return n;
}

std::string count_text(guint64 n) { return printf_str("%llu", (unsigned long long)n); }

std::string name_text(const du::Node *n)
{
    if (n->rest())
        return printf_str(g_dngettext(GETTEXT_PACKAGE, "%s smaller file", "%s smaller files", (gulong)n->files),
                          count_text(n->files).c_str());
    std::string s = n->name;   // the full path for the top folder
    if (n->otherFileSystem && n->children.empty()) s += " " + std::string(_("(another drive, not counted)"));
    return s;
}

GIcon *icon_for(const du::Node *n)
{
    if (n->rest()) {
        const char *names[] = {"view-more-horizontal-symbolic", "view-more-symbolic", "text-x-generic", nullptr};
        return g_themed_icon_new_from_names((char **)names, -1);
    }
    if (n->dir) {
        if (n->otherFileSystem) return g_themed_icon_new("drive-harddisk");
        if (n->unreadable) {
            const char *names[] = {"folder-locked", "folder", nullptr};
            return g_themed_icon_new_from_names((char **)names, -1);
        }
        return g_themed_icon_new("folder");
    }
    char *ct = g_content_type_guess(n->name.c_str(), nullptr, 0, nullptr);
    GIcon *i = g_content_type_get_icon(ct);
    g_free(ct);
    return i;
}

GtkListItemFactory *column_factory(Column col, DuWin *d)
{
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_object_set_data(G_OBJECT(f), "beluga-col", GINT_TO_POINTER(col));
    g_signal_connect(f, "setup", CB([](GtkSignalListItemFactory *f, GtkListItem *li, gpointer) {
        int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(f), "beluga-col"));
        if (col == ColName) {
            GtkWidget *exp = gtk_tree_expander_new();
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_box_append(GTK_BOX(box), gtk_image_new());
            GtkWidget *l = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
            gtk_box_append(GTK_BOX(box), l);
            gtk_tree_expander_set_child(GTK_TREE_EXPANDER(exp), box);
            gtk_list_item_set_child(li, exp);
        } else if (col == ColShare) {
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *bar = gtk_progress_bar_new();
            gtk_widget_set_hexpand(bar, TRUE);
            gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(box), bar);
            GtkWidget *l = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(l), 1);
            gtk_label_set_width_chars(GTK_LABEL(l), 7);
            gtk_widget_add_css_class(l, "numeric");
            gtk_box_append(GTK_BOX(box), l);
            gtk_list_item_set_child(li, box);
        } else {
            GtkWidget *l = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(l), 1);
            gtk_widget_add_css_class(l, "numeric");
            gtk_list_item_set_child(li, l);
        }
    }), nullptr);
    g_signal_connect(f, "bind", CB([](GtkSignalListItemFactory *f, GtkListItem *li, gpointer p) {
        auto *d = static_cast<DuWin *>(p);
        int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(f), "beluga-col"));
        du::Node *n = node_of(li);
        if (!n || !d->root) return;
        GtkWidget *child = gtk_list_item_get_child(li);
        if (col == ColName) {
            gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(child), GTK_TREE_LIST_ROW(gtk_list_item_get_item(li)));
            GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(child));
            GtkWidget *img = gtk_widget_get_first_child(box);
            GIcon *icon = icon_for(n);
            gtk_image_set_from_gicon(GTK_IMAGE(img), icon);
            g_object_unref(icon);
            GtkWidget *l = gtk_widget_get_next_sibling(img);
            gtk_label_set_text(GTK_LABEL(l), n == d->root ? d->path.c_str() : name_text(n).c_str());
            std::string tip = n->rest() ? std::string() : n->path();
            if (n->unreadable) tip += "\n" + std::string(_("Some of its contents could not be read."));
            gtk_widget_set_tooltip_text(box, tip.empty() ? nullptr : tip.c_str());
        } else if (col == ColSize) {
            gtk_label_set_text(GTK_LABEL(child), format_size(n->size).c_str());
        } else if (col == ColShare) {
            double frac = d->root->size ? (double)n->size / (double)d->root->size : 0;
            GtkWidget *bar = gtk_widget_get_first_child(child);
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), frac);
            gtk_label_set_text(GTK_LABEL(gtk_widget_get_next_sibling(bar)), printf_str("%.1f %%", 100.0 * frac).c_str());
        } else {
            gtk_label_set_text(GTK_LABEL(child), n->dir || n->rest() ? count_text(n->files).c_str() : "");
        }
    }), d);
    return f;
}

void add_column(DuWin *d, const char *title, Column col, bool expand, int width)
{
    GtkColumnViewColumn *c = gtk_column_view_column_new(title, column_factory(col, d));
    gtk_column_view_column_set_expand(c, expand);
    gtk_column_view_column_set_resizable(c, TRUE);
    if (width > 0) gtk_column_view_column_set_fixed_width(c, width);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(d->view), c);
    g_object_unref(c);
}

void show_tree(DuWin *d)
{
    GListStore *top = g_list_store_new(beluga_du_item_get_type());
    BelugaDuItem *ri = item_new(d->root);
    g_list_store_append(top, ri);
    g_object_unref(ri);
    GtkTreeListModel *tree = gtk_tree_list_model_new(G_LIST_MODEL(top), FALSE, FALSE, children_of, nullptr, nullptr);
    GtkSingleSelection *sel = gtk_single_selection_new(G_LIST_MODEL(tree));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(d->view), GTK_SELECTION_MODEL(sel));
    g_object_unref(sel);
    if (GtkTreeListRow *r = gtk_tree_list_model_get_row(tree, 0)) {
        gtk_tree_list_row_set_expanded(r, TRUE);
        g_object_unref(r);
    }
}

// Shows a scan (or the folder inside it that this window is for).
void show_result(DuWin *d, std::shared_ptr<du::Node> tree, du::Node *node, bool apparent)
{
    gtk_column_view_set_model(GTK_COLUMN_VIEW(d->view), nullptr);   // items point into the old tree
    d->tree = std::move(tree);
    d->root = node;
    d->rootApparent = apparent;
    show_tree(d);
    gtk_label_set_text(GTK_LABEL(d->totalLabel),
                       printf_str(apparent ? _("%s in files") : _("%s on disk"), format_size(d->root->size).c_str()).c_str());
}

std::string counts_text(const du::Node *n)
{
    return printf_str(_("%s files and %s folders"), count_text(n->files).c_str(), count_text(n->folders).c_str());
}

void set_busy(DuWin *d, bool on)
{
    d->busy = on;
    gtk_widget_set_visible(d->spinner, on);
    if (on) gtk_spinner_start(GTK_SPINNER(d->spinner));
    else gtk_spinner_stop(GTK_SPINNER(d->spinner));
    gtk_widget_set_sensitive(d->stop, on);
    if (!on && d->timer) {
        g_source_remove(d->timer);
        d->timer = 0;
    }
}

void update_progress(DuWin *d)
{
    if (!d->scanner) return;
    gtk_label_set_text(GTK_LABEL(d->status),
                       printf_str(_("Reading… %s in %s files and %s folders"), format_size(d->scanner->bytes).c_str(),
                                  count_text(d->scanner->files).c_str(), count_text(d->scanner->folders).c_str()).c_str());
}

void start_scan(DuWin *d)
{
    if (d->scanner) d->scanner->cancel();
    auto sc = std::make_shared<du::Scanner>();
    d->scanner = sc;
    du::Options o;
    o.apparentSize = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->apparent));
    o.oneFileSystem = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->oneFs));
    d->started = g_get_monotonic_time();
    set_busy(d, true);
    update_progress(d);
    if (!d->timer)
        d->timer = g_timeout_add(150, [](gpointer p) -> gboolean {
            update_progress(static_cast<DuWin *>(p));
            return G_SOURCE_CONTINUE;
        }, d);
    std::weak_ptr<void> alive = d->life;
    std::string path = d->path;
    std::thread([sc, path, o, alive, d] {
        auto result = std::make_shared<std::shared_ptr<du::Node>>(sc->run(path, o));
        gint64 when = g_get_real_time() / G_USEC_PER_SEC;
        if (*result) du::remember_tree(*result, o, when);
        run_on_main([sc, result, alive, d, o] {
            if (alive.expired() || d->scanner != sc) return;   // closed, or a newer scan replaced this one
            set_busy(d, false);
            if (!*result) {
                gtk_label_set_text(GTK_LABEL(d->status), sc->cancelled() ? _("Stopped.") : _("This folder could not be read."));
                return;
            }
            du::Node *top = result->get();
            show_result(d, *result, top, o.apparentSize);
            double secs = (g_get_monotonic_time() - d->started) / 1e6;
            std::string s = printf_str(_("%s, read in %.1f s"), counts_text(top).c_str(), secs);
            if (sc->errors)
                s += printf_str(g_dngettext(GETTEXT_PACKAGE, ". %s folder could not be read.", ". %s folders could not be read.",
                                            (gulong)sc->errors.load()),
                                count_text(sc->errors).c_str());
            gtk_label_set_text(GTK_LABEL(d->status), s.c_str());
        });
    }).detach();
}

void open_node(DuWin *d, du::Node *n)
{
    if (!n) return;
    bool file = !n->dir;
    std::string target = n->rest() ? n->parent->path() : n->path();
    std::string uri = file_uri(Ref<GFile>(g_file_new_for_path(target.c_str())).get());
    if (d->mwAlive.expired()) {
        App::instance().openLocations({uri}, file && !n->rest(), false, false);
        return;
    }
    ViewContainer *c = d->mw->activeContainer();
    if (!c) return;
    if (file && !n->rest()) {
        Ref<GFile> f(g_file_new_for_path(target.c_str()));
        Ref<GFile> parent(g_file_get_parent(f.get()));
        c->navigate(file_uri(parent.get()));
        if (c->view()) c->view()->selectUris({uri});
    } else {
        c->navigate(uri);
    }
    d->mw->present();
}

} // namespace

GtkWidget *show_disk_usage(MainWindow *mw, const std::string &path)
{
    auto *d = new DuWin;
    d->path = path;
    d->mw = mw;
    if (mw) d->mwAlive = mw->lifeToken();
    GtkWidget *win = gtk_window_new();
    d->win = win;
    g_object_set_data_full(G_OBJECT(win), "beluga-du", d, [](gpointer p) { delete static_cast<DuWin *>(p); });
    gtk_window_set_title(GTK_WINDOW(win), printf_str(_("Disk Usage – %s"), basename_of(path).c_str()).c_str());
    if (mw) gtk_window_set_transient_for(GTK_WINDOW(win), mw->window());
    gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 880, 600);
    gtk_widget_add_css_class(win, "beluga-dialog");

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("folder");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_box_append(GTK_BOX(head), icon);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(texts, TRUE);
    d->pathLabel = gtk_label_new(path.c_str());
    gtk_label_set_xalign(GTK_LABEL(d->pathLabel), 0);
    gtk_label_set_ellipsize(GTK_LABEL(d->pathLabel), PANGO_ELLIPSIZE_START);
    gtk_widget_add_css_class(d->pathLabel, "heading");
    gtk_box_append(GTK_BOX(texts), d->pathLabel);
    d->totalLabel = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(d->totalLabel), 0);
    gtk_box_append(GTK_BOX(texts), d->totalLabel);
    gtk_box_append(GTK_BOX(head), texts);
    gtk_box_append(GTK_BOX(box), head);

    GtkWidget *opts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    d->apparent = gtk_check_button_new_with_mnemonic(_("_Apparent size"));
    gtk_widget_set_tooltip_text(d->apparent, _("Add up the file sizes instead of the disk space the files take up."));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->apparent), settings().duApparentSize);
    d->oneFs = gtk_check_button_new_with_mnemonic(_("_Stay on this drive"));
    gtk_widget_set_tooltip_text(d->oneFs, _("Don't count other drives that are mounted inside this folder."));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->oneFs), settings().duOneFileSystem);
    gtk_box_append(GTK_BOX(opts), d->apparent);
    gtk_box_append(GTK_BOX(opts), d->oneFs);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(opts), spacer);
    GtkWidget *rescan = gtk_button_new_with_mnemonic(_("_Rescan"));
    d->stop = gtk_button_new_with_mnemonic(_("S_top"));
    gtk_box_append(GTK_BOX(opts), rescan);
    gtk_box_append(GTK_BOX(opts), d->stop);
    gtk_box_append(GTK_BOX(box), opts);

    GtkWidget *statusRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d->spinner = gtk_spinner_new();
    gtk_box_append(GTK_BOX(statusRow), d->spinner);
    d->status = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(d->status), 0);
    gtk_label_set_wrap(GTK_LABEL(d->status), TRUE);
    gtk_widget_add_css_class(d->status, "dim-label");
    gtk_box_append(GTK_BOX(statusRow), d->status);
    gtk_box_append(GTK_BOX(box), statusRow);

    d->view = gtk_column_view_new(nullptr);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(d->view), FALSE);
    add_column(d, _("Name"), ColName, true, -1);
    add_column(d, _("Size"), ColSize, false, 110);
    add_column(d, _("Share"), ColShare, false, 260);   // themes give a progress bar ~150 px at least
    add_column(d, _("Files"), ColFiles, false, 90);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), d->view);
    gtk_box_append(GTK_BOX(box), scroll);

    g_signal_connect(d->view, "activate", CB([](GtkColumnView *cv, guint pos, gpointer p) {
        auto *row = GTK_TREE_LIST_ROW(g_list_model_get_item(G_LIST_MODEL(gtk_column_view_get_model(cv)), pos));
        if (!row) return;
        gpointer item = gtk_tree_list_row_get_item(row);
        if (item) {
            open_node(static_cast<DuWin *>(p), BELUGA_DU_ITEM(item)->node);
            g_object_unref(item);
        }
        g_object_unref(row);
    }), d);
    g_signal_connect(rescan, "clicked", CB([](GtkButton *, gpointer p) { start_scan(static_cast<DuWin *>(p)); }), d);
    g_signal_connect(d->stop, "clicked", CB([](GtkButton *, gpointer p) {
        auto *d = static_cast<DuWin *>(p);
        if (d->scanner) d->scanner->cancel();
    }), d);
    auto optionChanged = CB([](GtkCheckButton *, gpointer p) {
        auto *d = static_cast<DuWin *>(p);
        settings().duApparentSize = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->apparent));
        settings().duOneFileSystem = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->oneFs));
        settings().save();
        start_scan(d);
    });
    g_signal_connect(d->apparent, "toggled", optionChanged, d);
    g_signal_connect(d->oneFs, "toggled", optionChanged, d);
    // The widgets are gone when "destroy" runs: stop the scan and its progress timer, so nothing
    // that arrives later touches them.
    g_signal_connect(win, "destroy", CB([](GtkWidget *w, gpointer) {
        if (DuWin *d = state_of(w)) {
            if (d->scanner) d->scanner->cancel();
            d->scanner.reset();
            if (d->timer) g_source_remove(d->timer);
            d->timer = 0;
            d->life.reset();
        }
    }), nullptr);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer w) -> gboolean {
        if (keyval != GDK_KEY_Escape) return FALSE;
        gtk_window_destroy(GTK_WINDOW(w));
        return TRUE;
    }), win);
    gtk_widget_add_controller(win, key);

    gtk_window_present(GTK_WINDOW(win));
    // The last scan of this folder (or of one above it) shows at once; it's read again only when old.
    du::Options o;
    o.apparentSize = settings().duApparentSize;
    o.oneFileSystem = settings().duOneFileSystem;
    du::CachedTree ct;
    if (du::cached_tree(path, o, ct)) {
        show_result(d, ct.tree, ct.node, o.apparentSize);
        gint64 age = std::max<gint64>(0, g_get_real_time() / G_USEC_PER_SEC - ct.when);
        std::string when = age < 60 ? std::string(_("read less than a minute ago"))
                         : age < 3600 ? printf_str(g_dngettext(GETTEXT_PACKAGE, "read %d minute ago", "read %d minutes ago", (gulong)(age / 60)), (int)(age / 60))
                                      : printf_str(g_dngettext(GETTEXT_PACKAGE, "read %d hour ago", "read %d hours ago", (gulong)(age / 3600)), (int)(age / 3600));
        gtk_label_set_text(GTK_LABEL(d->status), printf_str(_("%s, %s. Rescan to read it again."), counts_text(ct.node).c_str(), when.c_str()).c_str());
        if (age >= du::kFreshSeconds) start_scan(d);
    } else {
        start_scan(d);
    }
    return win;
}

bool disk_usage_busy(GtkWidget *window)
{
    DuWin *d = state_of(window);
    return d && d->busy;
}

guint disk_usage_rows(GtkWidget *window)
{
    DuWin *d = state_of(window);
    GtkSelectionModel *m = d ? gtk_column_view_get_model(GTK_COLUMN_VIEW(d->view)) : nullptr;
    return m ? g_list_model_get_n_items(G_LIST_MODEL(m)) : 0;
}

} // namespace beluga
