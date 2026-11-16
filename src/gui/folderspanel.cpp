// SPDX-License-Identifier: GPL-2.0-or-later
#include "folderspanel.hpp"

#include "dirmodel.hpp"
#include "gtkutil.hpp"
#include "settings.hpp"

namespace beluga {

namespace {

GtkFilter *folder_filter()
{
    return GTK_FILTER(gtk_custom_filter_new([](gpointer obj, gpointer) -> gboolean {
        BelugaItem *it = item_from_object(obj);
        if (!it || !item_data(it).isDir) return FALSE;
        return settings().foldersHiddenShown || !item_data(it).isHidden;
    }, nullptr, nullptr));
}

GtkSorter *folder_sorter()
{
    return GTK_SORTER(gtk_custom_sorter_new([](gconstpointer a, gconstpointer b, gpointer) -> int {
        BelugaItem *x = item_from_object((gpointer)a), *y = item_from_object((gpointer)b);
        if (!x || !y) return 0;
        int c = item_data(x).key.compare(item_data(y).key);
        return c < 0 ? -1 : c > 0 ? 1 : 0;
    }, nullptr, nullptr));
}

std::string row_uri(gpointer obj)
{
    BelugaItem *it = item_from_object(obj);
    return it ? item_data(it).uri : std::string();
}

bool is_under(const std::string &uri, const std::string &dir)
{
    if (uri == dir) return true;
    std::string d = dir;
    if (!ends_with(d, "/")) d += "/";
    return starts_with(uri, d);
}

} // namespace

GListModel *FoldersPanel::childModel(gpointer obj, gpointer)
{
    BelugaItem *it = item_from_object(obj);
    if (!it || !item_data(it).isDir) return nullptr;
    auto *lister = new DirLister;
    lister->open(item_data(it).uri);
    GtkFilterListModel *f = gtk_filter_list_model_new(G_LIST_MODEL(g_object_ref(lister->model())), folder_filter());
    GtkSortListModel *s = gtk_sort_list_model_new(G_LIST_MODEL(f), folder_sorter());
    g_object_set_data_full(G_OBJECT(s), "beluga-lister", lister, [](gpointer p) { delete static_cast<DirLister *>(p); });
    return G_LIST_MODEL(s);
}

FoldersPanel::FoldersPanel()
{
    root_ = gtk_scrolled_window_new();
    g_object_ref_sink(root_);
    gtk_widget_add_css_class(root_, "beluga-folders");
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", CB([](GtkSignalListItemFactory *, GtkListItem *li, gpointer p) {
        auto *fp = static_cast<FoldersPanel *>(p);
        GtkWidget *exp = gtk_tree_expander_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *img = gtk_image_new();
        GtkWidget *label = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(box), img);
        gtk_box_append(GTK_BOX(box), label);
        gtk_tree_expander_set_child(GTK_TREE_EXPANDER(exp), box);
        g_object_set_data(G_OBJECT(exp), "beluga-img", img);
        g_object_set_data(G_OBJECT(exp), "beluga-label", label);
        GtkGesture *mid = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mid), GDK_BUTTON_MIDDLE);
        g_signal_connect(mid, "released", CB([](GtkGestureClick *g, int, double, double, gpointer p2) {
            auto *fp2 = static_cast<FoldersPanel *>(p2);
            auto *u = static_cast<const char *>(g_object_get_data(G_OBJECT(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g))), "beluga-uri"));
            if (u && fp2->onOpenInNewTab) fp2->onOpenInNewTab(u);
        }), fp);
        gtk_widget_add_controller(exp, GTK_EVENT_CONTROLLER(mid));
        GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GdkDragAction(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
        g_signal_connect(dt, "drop", CB([](GtkDropTarget *t, const GValue *v, double x, double y, gpointer p2) -> gboolean {
            auto *fp2 = static_cast<FoldersPanel *>(p2);
            GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(t));
            auto *u = static_cast<const char *>(g_object_get_data(G_OBJECT(w), "beluga-uri"));
            if (!u || !fp2->onDrop) return FALSE;
            Ref<GFile> dest(g_file_new_for_uri(u));
            fp2->onDrop(dest.get(), static_cast<GdkFileList *>(g_value_get_boxed(v)),
                        gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(t)), w, x, y);
            return TRUE;
        }), fp);
        gtk_widget_add_controller(exp, GTK_EVENT_CONTROLLER(dt));
        gtk_list_item_set_child(li, exp);
    }), this);
    g_signal_connect(factory, "bind", CB([](GtkSignalListItemFactory *, GtkListItem *li, gpointer) {
        GtkWidget *exp = gtk_list_item_get_child(li);
        GtkTreeListRow *row = GTK_TREE_LIST_ROW(gtk_list_item_get_item(li));
        gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(exp), row);
        BelugaItem *it = item_from_object(row);
        if (!it) return;
        const ItemData &d = item_data(it);
        gtk_image_set_from_gicon(GTK_IMAGE(g_object_get_data(G_OBJECT(exp), "beluga-img")), d.icon.get());
        std::string name = d.name;
        if (gtk_tree_list_row_get_depth(row) == 0) name = display_location(d.file.get());
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(exp), "beluga-label")), name.c_str());
        g_object_set_data_full(G_OBJECT(exp), "beluga-uri", g_strdup(d.uri.c_str()), g_free);
    }), this);
    list_ = gtk_list_view_new(nullptr, factory);
    gtk_widget_add_css_class(list_, "navigation-sidebar");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(root_), list_);
    settingsListener_ = add_settings_listener([this] {
        std::string u = uri_;
        rootUri_.clear();
        if (!u.empty()) setUri(u);
    });
}

FoldersPanel::~FoldersPanel()
{
    remove_settings_listener(settingsListener_);
    if (expandSource_) g_source_remove(expandSource_);
    gtk_list_view_set_model(GTK_LIST_VIEW(list_), nullptr);
    g_clear_object(&sel_);
    g_clear_object(&tree_);
    g_object_unref(root_);
}

void FoldersPanel::setRoot(const std::string &rootUri)
{
    if (rootUri == rootUri_) return;
    rootUri_ = rootUri;
    gtk_list_view_set_model(GTK_LIST_VIEW(list_), nullptr);
    g_clear_object(&sel_);
    g_clear_object(&tree_);
    GListStore *store = g_list_store_new(BELUGA_TYPE_ITEM);
    Ref<GFile> f(g_file_new_for_uri(rootUri.c_str()));
    Ref<GFileInfo> info(g_file_query_info(f.get(), "standard::*", G_FILE_QUERY_INFO_NONE, nullptr, nullptr));
    if (info) {
        BelugaItem *it = item_new(f.get(), info.get());
        g_list_store_append(store, it);
        g_object_unref(it);
    }
    tree_ = gtk_tree_list_model_new(G_LIST_MODEL(store), FALSE, FALSE, childModel, this, nullptr);
    sel_ = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(tree_)));
    gtk_single_selection_set_autoselect(sel_, FALSE);
    gtk_single_selection_set_can_unselect(sel_, TRUE);
    g_signal_connect_swapped(sel_, "notify::selected", CB([](FoldersPanel *fp) {
        if (fp->syncing_) return;
        gpointer obj = gtk_single_selection_get_selected_item(fp->sel_);
        std::string u = row_uri(obj);
        if (!u.empty() && u != fp->uri_ && fp->onNavigate) fp->onNavigate(u);
    }), this);
    gtk_list_view_set_model(GTK_LIST_VIEW(list_), GTK_SELECTION_MODEL(sel_));
    if (GtkTreeListRow *row = gtk_tree_list_model_get_row(tree_, 0)) {
        gtk_tree_list_row_set_expanded(row, TRUE);
        g_object_unref(row);
    }
}

void FoldersPanel::setUri(const std::string &uri)
{
    uri_ = uri;
    std::string home = file_uri(Ref<GFile>(g_file_new_for_path(g_get_home_dir())).get());
    std::string root;
    if (settings().foldersLimitToHome && is_under(uri, home)) {
        root = home;
    } else {
        Ref<GFile> f(g_file_new_for_uri(uri.c_str()));
        Ref<GFile> top = f;
        for (Ref<GFile> p(g_file_get_parent(top.get())); p; p = Ref<GFile>(g_file_get_parent(top.get()))) top = p;
        root = file_uri(top.get());
    }
    setRoot(root);
    expandTo(uri);
}

void FoldersPanel::expandTo(const std::string &uri)
{
    if (expandSource_) g_source_remove(expandSource_);
    struct Ctx {
        FoldersPanel *p;
        std::string target;
        int ticks;
    };
    auto *ctx = new Ctx{this, uri, 0};
    auto step = [](gpointer data) -> gboolean {
        auto *c = static_cast<Ctx *>(data);
        FoldersPanel *p = c->p;
        if (!p->tree_ || ++c->ticks > 60) {
            p->expandSource_ = 0;
            return G_SOURCE_REMOVE;
        }
        guint n = g_list_model_get_n_items(G_LIST_MODEL(p->tree_));
        int best = -1;
        size_t bestLen = 0;
        for (guint i = 0; i < n; ++i) {
            GtkTreeListRow *row = gtk_tree_list_model_get_row(p->tree_, i);
            std::string u = row_uri(row);
            if (!u.empty() && is_under(c->target, u) && u.size() >= bestLen) {
                best = (int)i;
                bestLen = u.size();
            }
            g_object_unref(row);
        }
        if (best < 0) {
            p->expandSource_ = 0;
            return G_SOURCE_REMOVE;
        }
        GtkTreeListRow *row = gtk_tree_list_model_get_row(p->tree_, best);
        std::string u = row_uri(row);
        std::string t = c->target;
        while (t.size() > 1 && t.back() == '/') t.pop_back();
        std::string uu = u;
        while (uu.size() > 1 && uu.back() == '/') uu.pop_back();
        bool done = uu == t;
        if (!done && !gtk_tree_list_row_get_expanded(row)) gtk_tree_list_row_set_expanded(row, TRUE);
        g_object_unref(row);
        if (done) {
            p->syncing_ = true;
            gtk_single_selection_set_selected(p->sel_, best);
            p->syncing_ = false;
            if (settings().foldersAutoScroll) gtk_list_view_scroll_to(GTK_LIST_VIEW(p->list_), best, GTK_LIST_SCROLL_NONE, nullptr);
            p->expandSource_ = 0;
            return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    };
    // The context is freed when the source ends, including when expandTo() replaces it.
    if (step(ctx) == G_SOURCE_CONTINUE)
        expandSource_ = g_timeout_add_full(G_PRIORITY_DEFAULT, 80, step, ctx, [](gpointer d) { delete static_cast<Ctx *>(d); });
    else
        delete ctx;
}

} // namespace beluga
