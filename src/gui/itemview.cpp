// SPDX-License-Identifier: GPL-2.0-or-later
#include "itemview.hpp"

#include "gtkutil.hpp"
#include "settings.hpp"
#include "viewprops.hpp"

#include <algorithm>
#include <cmath>

// ---- GObject wrapper ---------------------------------------------------------------

G_BEGIN_DECLS
#define BELUGA_TYPE_ITEM_VIEW (beluga_item_view_get_type())
G_DECLARE_FINAL_TYPE(BelugaItemView, beluga_item_view, BELUGA, ITEM_VIEW, GtkWidget)
G_END_DECLS

struct _BelugaItemView {
    GtkWidget parent;
    beluga::ItemView *v;
};

G_DEFINE_FINAL_TYPE_WITH_CODE(BelugaItemView, beluga_item_view, GTK_TYPE_WIDGET,
                              G_IMPLEMENT_INTERFACE(GTK_TYPE_SCROLLABLE, nullptr))

enum { PROP_0, PROP_HADJ, PROP_VADJ, PROP_HPOL, PROP_VPOL };

static void div_set_property(GObject *obj, guint id, const GValue *value, GParamSpec *pspec)
{
    beluga::ItemView *v = BELUGA_ITEM_VIEW(obj)->v;
    switch (id) {
    case PROP_HADJ: v->setAdjustment(true, GTK_ADJUSTMENT(g_value_get_object(value))); break;
    case PROP_VADJ: v->setAdjustment(false, GTK_ADJUSTMENT(g_value_get_object(value))); break;
    case PROP_HPOL: v->hpolicy_ = g_value_get_enum(value); break;
    case PROP_VPOL: v->vpolicy_ = g_value_get_enum(value); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void div_get_property(GObject *obj, guint id, GValue *value, GParamSpec *pspec)
{
    beluga::ItemView *v = BELUGA_ITEM_VIEW(obj)->v;
    switch (id) {
    case PROP_HADJ: g_value_set_object(value, v->hadj_); break;
    case PROP_VADJ: g_value_set_object(value, v->vadj_); break;
    case PROP_HPOL: g_value_set_enum(value, v->hpolicy_); break;
    case PROP_VPOL: g_value_set_enum(value, v->vpolicy_); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, id, pspec);
    }
}

static void div_measure(GtkWidget *w, GtkOrientation o, int forSize, int *min, int *nat, int *minb, int *natb)
{
    BELUGA_ITEM_VIEW(w)->v->measure(o, forSize, min, nat);
    *minb = *natb = -1;
}

static void div_size_allocate(GtkWidget *w, int width, int height, int)
{
    BELUGA_ITEM_VIEW(w)->v->allocate(width, height);
}

static void div_snapshot(GtkWidget *w, GtkSnapshot *s) { BELUGA_ITEM_VIEW(w)->v->snapshot(s); }

static void div_dispose(GObject *obj)
{
    auto *self = BELUGA_ITEM_VIEW(obj);
    if (self->v) self->v->dispose();
    G_OBJECT_CLASS(beluga_item_view_parent_class)->dispose(obj);
}

static void div_finalize(GObject *obj)
{
    auto *self = BELUGA_ITEM_VIEW(obj);
    delete self->v;
    self->v = nullptr;
    G_OBJECT_CLASS(beluga_item_view_parent_class)->finalize(obj);
}

static void beluga_item_view_class_init(BelugaItemViewClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
    oc->set_property = div_set_property;
    oc->get_property = div_get_property;
    oc->dispose = div_dispose;
    oc->finalize = div_finalize;
    wc->measure = div_measure;
    wc->size_allocate = div_size_allocate;
    wc->snapshot = div_snapshot;
    g_object_class_override_property(oc, PROP_HADJ, "hadjustment");
    g_object_class_override_property(oc, PROP_VADJ, "vadjustment");
    g_object_class_override_property(oc, PROP_HPOL, "hscroll-policy");
    g_object_class_override_property(oc, PROP_VPOL, "vscroll-policy");
    gtk_widget_class_set_css_name(wc, "beluga-view");
    gtk_widget_class_set_accessible_role(wc, GTK_ACCESSIBLE_ROLE_GRID);
}

static void beluga_item_view_init(BelugaItemView *self) { self->v = new beluga::ItemView(GTK_WIDGET(self)); }

namespace beluga {

GtkWidget *item_view_new() { return GTK_WIDGET(g_object_new(BELUGA_TYPE_ITEM_VIEW, nullptr)); }
ItemView *item_view_get(GtkWidget *w) { return BELUGA_ITEM_VIEW(w)->v; }

namespace {
constexpr double kGroupGap = 10;
int text_width_for_index(int idx)
{
    static const int w[] = {72, 96, 128, 176};
    return w[std::clamp(idx, 0, 3)];
}
int compact_limit(int idx)
{
    static const int w[] = {420, 120, 200, 300};
    return w[std::clamp(idx, 0, 3)];
}
GdkModifierType current_mods(GtkEventController *c) { return gtk_event_controller_get_current_event_state(c); }
} // namespace

ItemView::ItemView(GtkWidget *w) : w_(w)
{
    gtk_widget_set_focusable(w_, TRUE);
    gtk_widget_set_overflow(w_, GTK_OVERFLOW_HIDDEN);
    gtk_widget_add_css_class(w_, "view");
    gtk_widget_set_has_tooltip(w_, TRUE);
    setupControllers();
}

ItemView::~ItemView()
{
    if (rbBase_) gtk_bitset_unref(rbBase_);
}

void ItemView::dispose()
{
    if (disposed_) return;
    disposed_ = true;
    stopAutoscroll();
    if (springSource_) g_source_remove(springSource_);
    springSource_ = 0;
    setModel(nullptr);
    for (Cell *c : cells_) {
        unbindCell(c);
        gtk_widget_unparent(c->root);
        delete c;
    }
    cells_.clear();
    for (auto &g : groupHeaders_) gtk_widget_unparent(g.root);
    groupHeaders_.clear();
    if (header_) gtk_widget_unparent(header_);
    header_ = nullptr;
    if (rubber_) gtk_widget_unparent(rubber_);
    rubber_ = nullptr;
    if (renameEntry_) gtk_widget_unparent(renameEntry_);
    renameEntry_ = nullptr;
    // Context menus and the drop menu that are still open when the view goes away.
    for (GtkWidget *c = gtk_widget_get_first_child(w_); c;) {
        GtkWidget *next = gtk_widget_get_next_sibling(c);
        gtk_widget_unparent(c);
        c = next;
    }
    for (GtkAdjustment **a : {&hadj_, &vadj_})
        if (*a) {
            g_signal_handlers_disconnect_by_data(*a, this);
            g_object_unref(*a);
            *a = nullptr;
        }
    if (fontCss_) {
        gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(fontCss_));
        g_object_unref(fontCss_);
        fontCss_ = nullptr;
    }
    g_clear_weak_pointer(&current_);
    g_clear_weak_pointer(&anchor_);
    g_clear_weak_pointer(&hover_);
    g_clear_weak_pointer(&renameItem_);
}

void ItemView::setAdjustment(bool horizontal, GtkAdjustment *adj)
{
    GtkAdjustment *&slot = horizontal ? hadj_ : vadj_;
    if (adj && adj == slot) return;
    if (slot) {
        g_signal_handlers_disconnect_by_data(slot, this);
        g_object_unref(slot);
    }
    if (!adj) adj = gtk_adjustment_new(0, 0, 0, 0, 0, 0);
    slot = GTK_ADJUSTMENT(g_object_ref_sink(adj));
    g_signal_connect_swapped(slot, "value-changed", CB([](ItemView *v) {
        if (v->rubberActive_) v->updateRubberband();
        gtk_widget_queue_allocate(v->w_);
    }), this);
}

// ---- model ---------------------------------------------------------------------------

void ItemView::setModel(GtkSelectionModel *model)
{
    if (sel_ == model) return;
    for (Cell *c : cells_) {
        unbindCell(c);
        gtk_widget_set_child_visible(c->root, FALSE);
    }
    if (sel_) {
        g_signal_handlers_disconnect_by_data(sel_, this);
        g_object_unref(sel_);
    }
    sel_ = model ? GTK_SELECTION_MODEL(g_object_ref(model)) : nullptr;
    if (sel_) {
        g_signal_connect(sel_, "items-changed", G_CALLBACK(onItemsChanged), this);
        g_signal_connect(sel_, "selection-changed", G_CALLBACK(onSelectionChanged), this);
        if (GTK_IS_SECTION_MODEL(sel_))
            g_signal_connect_swapped(sel_, "sections-changed", CB([](ItemView *v) {
                v->sectionsDirty_ = v->layoutDirty_ = true;
                gtk_widget_queue_allocate(v->w_);
            }), this);
    }
    g_clear_weak_pointer(&current_);
    g_clear_weak_pointer(&anchor_);
    currentPos_ = -1;
    relayout();
}

void ItemView::onItemsChanged(GListModel *, guint, guint, guint, gpointer self)
{
    auto *v = static_cast<ItemView *>(self);
    v->layoutDirty_ = v->sectionsDirty_ = true;
    v->currentPos_ = -1;
    gtk_widget_queue_allocate(v->w_);
}

void ItemView::onSelectionChanged(GtkSelectionModel *, guint, guint, gpointer self)
{
    auto *v = static_cast<ItemView *>(self);
    for (Cell *c : v->cells_)
        if (c->used && c->item) v->updateCellState(c);
}

void ItemView::onItemSignal(BelugaItem *item, gpointer self)
{
    auto *v = static_cast<ItemView *>(self);
    auto it = v->bound_.find(item);
    if (it == v->bound_.end()) return;
    Cell *c = it->second;
    v->bindCell(c, c->pos, c->rowObj, item);
    if (v->mode_ == 1) v->layoutDirty_ = true;
    gtk_widget_queue_allocate(v->w_);
}

int ItemView::count() const { return sel_ ? (int)g_list_model_get_n_items(G_LIST_MODEL(sel_)) : 0; }

// ---- configuration ---------------------------------------------------------------------

void ItemView::setMode(int mode)
{
    if (mode == mode_ && !cells_.empty()) return;
    mode_ = mode;
    for (Cell *c : cells_) {
        unbindCell(c);
        gtk_widget_unparent(c->root);
        delete c;
    }
    cells_.clear();
    bound_.clear();
    rebuildHeader();
    relayout();
}

void ItemView::setIconSize(int px)
{
    if (px == iconSize_) return;
    iconSize_ = px;
    refreshAll();
    relayout();
}

void ItemView::setRoles(const std::vector<std::string> &roles)
{
    if (roles == roles_ && !columns_.empty()) return;
    roles_ = roles;
    for (Cell *c : cells_) {
        unbindCell(c);
        gtk_widget_unparent(c->root);
        delete c;
    }
    cells_.clear();
    bound_.clear();
    rebuildHeader();
    relayout();
}

void ItemView::setColumnWidths(const std::map<std::string, int> &w)
{
    savedWidths_ = w;
    rebuildHeader();
    relayout();
}

std::map<std::string, int> ItemView::columnWidths() const { return savedWidths_; }

void ItemView::setSortIndicator(const std::string &role, bool descending)
{
    sortRole_ = role;
    sortDesc_ = descending;
    updateHeaderLabels();
}

void ItemView::setPreviews(bool on)
{
    if (on == previews_) return;
    previews_ = on;
    refreshAll();
}

void ItemView::setHeaderVisible(bool on)
{
    if (on == headerVisible_) return;
    headerVisible_ = on;
    rebuildHeader();
    relayout();
}

void ItemView::setGrouped(bool on)
{
    grouped_ = on;
    relayout();
}

void ItemView::setSelectionMode(bool on)
{
    selectionMode_ = on;
    refreshAll();
}

void ItemView::setFont(const std::string &font)
{
    std::string cls = printf_str("belugaview-%p", (void *)this);
    GdkDisplay *display = gdk_display_get_default();
    if (fontCss_) {
        gtk_style_context_remove_provider_for_display(display, GTK_STYLE_PROVIDER(fontCss_));
        g_object_unref(fontCss_);
        fontCss_ = nullptr;
    }
    gtk_widget_remove_css_class(w_, cls.c_str());
    if (!font.empty()) {
        PangoFontDescription *d = pango_font_description_from_string(font.c_str());
        std::string css = "." + cls + " {";
        if (pango_font_description_get_family(d))
            css += printf_str(" font-family: \"%s\";", pango_font_description_get_family(d));
        if (pango_font_description_get_size(d) > 0)
            css += printf_str(" font-size: %gpt;", pango_font_description_get_size(d) / (double)PANGO_SCALE);
        css += printf_str(" font-weight: %d;", (int)pango_font_description_get_weight(d));
        if (pango_font_description_get_style(d) == PANGO_STYLE_ITALIC) css += " font-style: italic;";
        css += " }";
        pango_font_description_free(d);
        fontCss_ = gtk_css_provider_new();
        gtk_css_provider_load_from_string(fontCss_, css.c_str());
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(fontCss_), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        gtk_widget_add_css_class(w_, cls.c_str());
    }
    relayout();
}

void ItemView::relayout()
{
    layoutDirty_ = sectionsDirty_ = true;
    gtk_widget_queue_allocate(w_);
}

void ItemView::refreshAll()
{
    for (Cell *c : cells_)
        if (c->used && c->item) bindCell(c, c->pos, c->rowObj, c->item);
    gtk_widget_queue_allocate(w_);
}

// ---- layout ----------------------------------------------------------------------------

double ItemView::lineHeight() const
{
    PangoContext *ctx = gtk_widget_get_pango_context(w_);
    PangoFontMetrics *m = pango_context_get_metrics(ctx, nullptr, nullptr);
    double h = pango_font_metrics_get_height(m) / (double)PANGO_SCALE;
    if (h <= 0) h = (pango_font_metrics_get_ascent(m) + pango_font_metrics_get_descent(m)) / (double)PANGO_SCALE;
    pango_font_metrics_unref(m);
    return std::ceil(std::max(h, 10.0));
}

double ItemView::headerHeight() const { return mode_ == 2 && headerVisible_ ? std::ceil(lineH_ + 14) : 0; }

int ItemView::itemWidthIcons() const
{
    int text = text_width_for_index(settings().iconsTextWidth);
    int pad = settings().iconsCompactSpacing ? 4 : 10;
    return std::max(iconSize_ + 2 * pad, text + pad);
}

int ItemView::compactColumnWidth(int first, int cnt)
{
    int limit = compact_limit(settings().compactMaxTextWidth);
    PangoLayout *layout = nullptr;
    int maxW = 40;
    for (int i = first; i < first + cnt; ++i) {
        BelugaItem *it = item_at(G_LIST_MODEL(sel_), i);
        if (!it) continue;
        ItemData &d = item_data(it);
        if (d.nameWidth < 0) {
            if (!layout) layout = gtk_widget_create_pango_layout(w_, nullptr);
            pango_layout_set_text(layout, d.name.c_str(), -1);
            int w, h;
            pango_layout_get_pixel_size(layout, &w, &h);
            d.nameWidth = w;
        }
        maxW = std::max(maxW, std::min(d.nameWidth, limit));
        if (maxW >= limit) break;
    }
    if (layout) g_object_unref(layout);
    return iconSize_ + 6 + maxW + 18;
}

void ItemView::computeSections()
{
    sections_.clear();
    int n = count();
    if (grouped_ && sel_ && GTK_IS_SECTION_MODEL(sel_) && n > 0) {
        int pos = 0;
        while (pos < n) {
            guint s = 0, e = 0;
            gtk_section_model_get_section(GTK_SECTION_MODEL(sel_), pos, &s, &e);
            if (e <= (guint)pos) e = pos + 1;
            Section sec{(int)s, (int)e, groupLabel ? groupLabel((int)s) : std::string()};
            sections_.push_back(sec);
            pos = (int)e;
        }
    } else {
        sections_.push_back({0, n, {}});
    }
    sectionsDirty_ = false;
}

void ItemView::computeLayout(int width, int height)
{
    lineH_ = lineHeight();
    if (sectionsDirty_) computeSections();
    lines_.clear();
    bool groups = grouped_ && sections_.size() >= 1 && count() > 0;
    double gh = std::ceil(lineH_ + 18);
    int extras = (int)roles_.size();
    if (mode_ == 0) {
        itemW_ = itemWidthIcons();
        int lines = settings().iconsMaxLines <= 0 ? 6 : settings().iconsMaxLines;
        double pad = settings().iconsCompactSpacing ? 4 : 8;
        itemH_ = std::ceil(pad + iconSize_ + 4 + lines * lineH_ + extras * lineH_ + pad);
        double avail = std::max(1.0, width - 2 * margin_);
        cols_ = std::max(1, (int)(avail / itemW_));
        gapX_ = cols_ > 1 || avail > itemW_ ? (avail - cols_ * itemW_) / cols_ : 0;
        double y = margin_;
        for (size_t si = 0; si < sections_.size(); ++si) {
            const Section &s = sections_[si];
            if (groups) {
                lines_.push_back({y, gh, s.start, 0, (int)si, true});
                y += gh;
            }
            for (int i = s.start; i < s.end; i += cols_) {
                lines_.push_back({y, itemH_, i, std::min(cols_, s.end - i), (int)si, false});
                y += itemH_;
            }
            if (groups) y += kGroupGap;
        }
        contentW_ = width;
        contentH_ = y + margin_;
    } else if (mode_ == 2) {
        columns_.clear();
        columns_.push_back({"text", 0});
        for (auto &r : roles_) {
            auto it = savedWidths_.find(r);
            const RoleInfo *ri = role_info(r);
            columns_.push_back({r, it != savedWidths_.end() ? it->second : (ri ? ri->defaultWidth : 120)});
        }
        double others = 0;
        for (size_t i = 1; i < columns_.size(); ++i) others += columns_[i].width;
        auto nw = savedWidths_.find("text");
        int nameW = nw != savedWidths_.end() ? nw->second : 300;
        columns_[0].width = std::max(nameW, (int)(width - others - 2));
        rowH_ = std::ceil(std::max((double)iconSize_, lineH_) + 8);
        double y = headerHeight();
        for (size_t si = 0; si < sections_.size(); ++si) {
            const Section &s = sections_[si];
            if (groups) {
                lines_.push_back({y, gh, s.start, 0, (int)si, true});
                y += gh;
            }
            for (int i = s.start; i < s.end; ++i) {
                lines_.push_back({y, rowH_, i, 1, (int)si, false});
                y += rowH_;
            }
        }
        contentW_ = 0;
        for (auto &c : columns_) contentW_ += c.width;
        contentH_ = y + 2;
        ensureHeaderCells();
    } else {
        rowH_ = std::ceil(std::max((double)iconSize_, lineH_ * (1 + extras)) + 8);
        double top = margin_ + (groups ? gh : 0);
        double avail = std::max(1.0, height - top - margin_);
        rows_ = std::max(1, (int)(avail / rowH_));
        double x = margin_;
        for (size_t si = 0; si < sections_.size(); ++si) {
            Section &s = sections_[si];
            s.off = x;
            for (int i = s.start; i < s.end; i += rows_) {
                int cnt = std::min(rows_, s.end - i);
                double w = compactColumnWidth(i, cnt);
                lines_.push_back({x, w, i, cnt, (int)si, false});
                x += w;
            }
            s.ext = x - s.off;
            if (groups) x += kGroupGap;
        }
        contentW_ = x + margin_;
        contentH_ = height;
    }
}

int ItemView::lineOf(int pos) const
{
    if (lines_.empty()) return -1;
    int lo = 0, hi = (int)lines_.size() - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (lines_[mid].first <= pos) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return -1;
    while (best < (int)lines_.size() && lines_[best].header) best++;
    if (best >= (int)lines_.size()) return -1;
    const Line &l = lines_[best];
    return pos >= l.first && pos < l.first + l.count ? best : -1;
}

bool ItemView::contentRect(int pos, double &x, double &y, double &w, double &h) const
{
    int li = lineOf(pos);
    if (li < 0) return false;
    const Line &l = lines_[li];
    int idx = pos - l.first;
    if (mode_ == 0) {
        x = margin_ + idx * (itemW_ + gapX_) + gapX_ / 2;
        y = l.off;
        w = itemW_;
        h = itemH_;
    } else if (mode_ == 2) {
        x = 0;
        y = l.off;
        w = std::max(contentW_, (double)gtk_widget_get_width(w_));
        h = rowH_;
    } else {
        bool groups = grouped_ && count() > 0;
        double top = margin_ + (groups ? std::ceil(lineH_ + 18) : 0);
        x = l.off;
        y = top + idx * rowH_;
        w = l.ext;
        h = rowH_;
    }
    return true;
}

void ItemView::toWidget(double &x, double &y) const
{
    x -= hadj_ ? gtk_adjustment_get_value(hadj_) : 0;
    y -= vadj_ ? gtk_adjustment_get_value(vadj_) : 0;
}

void ItemView::toContent(double &x, double &y) const
{
    x += hadj_ ? gtk_adjustment_get_value(hadj_) : 0;
    y += vadj_ ? gtk_adjustment_get_value(vadj_) : 0;
}

bool ItemView::itemRect(int pos, GdkRectangle &r) const
{
    double x, y, w, h;
    if (!contentRect(pos, x, y, w, h)) return false;
    toWidget(x, y);
    r = {(int)x, (int)y, (int)w, (int)h};
    return true;
}

int ItemView::itemAt(double wx, double wy) const
{
    if (lines_.empty()) return -1;
    if (mode_ == 2 && wy < headerHeight()) return -1;
    double x = wx, y = wy;
    toContent(x, y);
    double key = vertical() ? y : x;
    int lo = 0, hi = (int)lines_.size() - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const Line &l = lines_[mid];
        if (key < l.off) hi = mid - 1;
        else if (key >= l.off + l.ext) lo = mid + 1;
        else {
            found = mid;
            break;
        }
    }
    if (found < 0 || lines_[found].header) return -1;
    const Line &l = lines_[found];
    if (mode_ == 0) {
        double cx = x - margin_ - gapX_ / 2;
        if (cx < 0) return -1;
        int col = (int)(cx / (itemW_ + gapX_));
        if (col >= l.count) return -1;
        if (cx - col * (itemW_ + gapX_) > itemW_) return -1;
        return l.first + col;
    }
    if (mode_ == 2) {
        if (x > std::max(contentW_, (double)gtk_widget_get_width(w_))) return -1;
        return l.first;
    }
    bool groups = grouped_ && count() > 0;
    double top = margin_ + (groups ? std::ceil(lineH_ + 18) : 0);
    if (y < top) return -1;
    int row = (int)((y - top) / rowH_);
    return row < l.count ? l.first + row : -1;
}

bool ItemView::contentHit(int pos, double wx, double wy) const
{
    if (pos < 0) return false;
    if (mode_ == 0) return true;
    double x, y, w, h;
    if (!contentRect(pos, x, y, w, h)) return false;
    double cx = wx, cy = wy;
    toContent(cx, cy);
    BelugaItem *it = item_at(G_LIST_MODEL(sel_), pos);
    int nameW = it && item_data(it).nameWidth >= 0 ? item_data(it).nameWidth : 120;
    double start = x;
    if (mode_ == 2) {
        gpointer obj = g_list_model_get_item(G_LIST_MODEL(sel_), pos);
        if (GTK_IS_TREE_LIST_ROW(obj)) start += 16.0 * (gtk_tree_list_row_get_depth(GTK_TREE_LIST_ROW(obj))) + (tree_ ? 18 : 0);
        if (obj) g_object_unref(obj);
        return cx < start + iconSize_ + 8 + std::min((double)nameW + 12, (double)columns_[0].width);
    }
    return cx < start + iconSize_ + 8 + nameW + 12;
}

void ItemView::measure(GtkOrientation, int, int *min, int *nat)
{
    *min = 0;
    *nat = 200;
}

// ---- cells -------------------------------------------------------------------------------

ItemView::Cell *ItemView::takeCell()
{
    for (Cell *c : cells_)
        if (!c->used && !c->item) {
            c->used = true;
            return c;
        }
    Cell *c = new Cell;
    buildCell(c);
    cells_.push_back(c);
    c->used = true;
    return c;
}

static GtkWidget *make_label(bool name)
{
    GtkWidget *l = gtk_label_new(nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(l), settings().elidingMode == 1 && name ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_single_line_mode(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, FALSE);
    return l;
}

void ItemView::buildCell(Cell *c)
{
    c->mode = mode_;
    c->columns = roles_.size();
    auto iconStack = [&]() {
        GtkWidget *ov = gtk_overlay_new();
        c->icon = gtk_image_new();
        gtk_widget_add_css_class(c->icon, "item-icon");
        gtk_overlay_set_child(GTK_OVERLAY(ov), c->icon);
        c->emblem = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(c->emblem), 16);
        gtk_widget_set_halign(c->emblem, GTK_ALIGN_END);
        gtk_widget_set_valign(c->emblem, GTK_ALIGN_END);
        gtk_widget_set_can_target(c->emblem, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), c->emblem);
        c->toggle = gtk_check_button_new();
        gtk_widget_add_css_class(c->toggle, "selection-toggle");
        gtk_widget_set_halign(c->toggle, GTK_ALIGN_START);
        gtk_widget_set_valign(c->toggle, GTK_ALIGN_START);
        gtk_widget_set_can_target(c->toggle, FALSE);
        gtk_widget_set_can_focus(c->toggle, FALSE);
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), c->toggle);
        gtk_widget_set_can_target(ov, FALSE);
        return ov;
    };
    if (mode_ == 0) {
        c->root = GTK_WIDGET(g_object_new(GTK_TYPE_BOX, "orientation", GTK_ORIENTATION_VERTICAL, "spacing", 4,
                                          "accessible-role", GTK_ACCESSIBLE_ROLE_GRID_CELL, nullptr));
        gtk_widget_add_css_class(c->root, "icons");
        GtkWidget *ov = iconStack();
        gtk_widget_set_halign(ov, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(ov, settings().iconsCompactSpacing ? 4 : 8);
        gtk_box_append(GTK_BOX(c->root), ov);
        c->name = gtk_label_new(nullptr);
        gtk_label_set_wrap(GTK_LABEL(c->name), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(c->name), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_justify(GTK_LABEL(c->name), GTK_JUSTIFY_CENTER);
        gtk_label_set_xalign(GTK_LABEL(c->name), 0.5f);
        int lines = settings().iconsMaxLines <= 0 ? 6 : settings().iconsMaxLines;
        gtk_label_set_lines(GTK_LABEL(c->name), lines);
        gtk_label_set_ellipsize(GTK_LABEL(c->name), settings().elidingMode == 1 ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_END);
        gtk_label_set_natural_wrap_mode(GTK_LABEL(c->name), GTK_NATURAL_WRAP_NONE);
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_insert_hyphens_new(FALSE));   // break names without "-"
        gtk_label_set_attributes(GTK_LABEL(c->name), attrs);
        pango_attr_list_unref(attrs);
        gtk_widget_set_margin_start(c->name, 2);
        gtk_widget_set_margin_end(c->name, 2);
        gtk_box_append(GTK_BOX(c->root), c->name);
        for (size_t i = 0; i < roles_.size(); ++i) {
            GtkWidget *l = make_label(false);
            gtk_label_set_xalign(GTK_LABEL(l), 0.5f);
            gtk_widget_add_css_class(l, "dim-label");
            gtk_box_append(GTK_BOX(c->root), l);
            c->extras.push_back(l);
        }
        c->highlight = c->root;
    } else if (mode_ == 1) {
        c->root = GTK_WIDGET(g_object_new(GTK_TYPE_BOX, "orientation", GTK_ORIENTATION_HORIZONTAL, "spacing", 6,
                                          "accessible-role", GTK_ACCESSIBLE_ROLE_GRID_CELL, nullptr));
        gtk_widget_add_css_class(c->root, "compact");
        GtkWidget *ov = iconStack();
        gtk_widget_set_valign(ov, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_start(ov, 4);
        gtk_box_append(GTK_BOX(c->root), ov);
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(texts, TRUE);
        c->name = make_label(true);
        gtk_box_append(GTK_BOX(texts), c->name);
        for (size_t i = 0; i < roles_.size(); ++i) {
            GtkWidget *l = make_label(false);
            gtk_widget_add_css_class(l, "dim-label");
            gtk_box_append(GTK_BOX(texts), l);
            c->extras.push_back(l);
        }
        gtk_box_append(GTK_BOX(c->root), texts);
        c->highlight = c->root;
    } else {
        c->root = GTK_WIDGET(g_object_new(GTK_TYPE_BOX, "orientation", GTK_ORIENTATION_HORIZONTAL, "spacing", 0,
                                          "accessible-role", GTK_ACCESSIBLE_ROLE_ROW, nullptr));
        gtk_widget_add_css_class(c->root, "details");
        GtkWidget *nameBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_add_css_class(nameBox, "name-cell");
        c->indent = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_append(GTK_BOX(nameBox), c->indent);
        c->expander = gtk_image_new();
        gtk_image_set_pixel_size(GTK_IMAGE(c->expander), 16);
        gtk_widget_set_size_request(c->expander, 16, -1);
        gtk_box_append(GTK_BOX(nameBox), c->expander);
        GtkWidget *ov = iconStack();
        gtk_widget_set_valign(ov, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(nameBox), ov);
        c->name = make_label(true);
        gtk_widget_set_hexpand(c->name, TRUE);
        gtk_box_append(GTK_BOX(nameBox), c->name);
        gtk_box_append(GTK_BOX(c->root), nameBox);
        for (size_t i = 0; i < roles_.size(); ++i) {
            GtkWidget *l = make_label(false);
            const RoleInfo *ri = role_info(roles_[i]);
            if (ri && ri->rightAligned) gtk_label_set_xalign(GTK_LABEL(l), 1.0f);
            gtk_widget_set_margin_start(l, 6);
            gtk_widget_set_margin_end(l, 6);
            gtk_widget_add_css_class(l, "dim-label");
            gtk_box_append(GTK_BOX(c->root), l);
            c->extras.push_back(l);
        }
        c->highlight = settings().detailsHighlightEntireRow ? c->root : nameBox;
    }
    gtk_widget_add_css_class(c->root, "beluga-item");
    gtk_widget_insert_after(c->root, w_, nullptr);   // below headers and rubber band
    gtk_widget_set_child_visible(c->root, FALSE);
}

void ItemView::unbindCell(Cell *c)
{
    if (c->item) {
        if (c->handler) g_signal_handler_disconnect(c->item, c->handler);
        auto it = bound_.find(c->item);
        if (it != bound_.end() && it->second == c) bound_.erase(it);
        g_object_unref(c->item);
    }
    if (c->rowObj) g_object_unref(c->rowObj);
    c->item = nullptr;
    c->rowObj = nullptr;
    c->handler = 0;
    c->pos = -1;
}

void ItemView::bindCell(Cell *c, int pos, gpointer obj, BelugaItem *item)
{
    if (c->item != item) {
        unbindCell(c);
        c->item = BELUGA_ITEM(g_object_ref(item));
        c->rowObj = obj ? g_object_ref(obj) : nullptr;
        c->handler = g_signal_connect(item, "changed", G_CALLBACK(onItemSignal), this);
        bound_[item] = c;
    } else if (obj != c->rowObj) {
        if (c->rowObj) g_object_unref(c->rowObj);
        c->rowObj = obj ? g_object_ref(obj) : nullptr;
    }
    c->pos = pos;
    ItemData &d = item_data(item);
    int scale = gtk_widget_get_scale_factor(w_);
    if (previews_ && d.thumb) gtk_image_set_from_paintable(GTK_IMAGE(c->icon), d.thumb.get());
    else gtk_image_set_from_gicon(GTK_IMAGE(c->icon), d.icon.get());
    gtk_image_set_pixel_size(GTK_IMAGE(c->icon), iconSize_);
    if (previews_ && !d.isDir && !d.thumbPending && !d.thumbFailed && d.thumbPixels < iconSize_ * scale && onNeedPreview)
        onNeedPreview(item);
    gtk_label_set_text(GTK_LABEL(c->name), d.name.c_str());
    for (size_t i = 0; i < c->extras.size() && i < roles_.size(); ++i) {
        const std::string &role = roles_[i];
        if (role == "size" && d.isDir && d.dirCount == -2 && !d.dirPending && onNeedDirSize) onNeedDirSize(item);
        gtk_label_set_text(GTK_LABEL(c->extras[i]), item_role_text(item, role).c_str());
    }
    const char *emblem = nullptr;
    if (d.isLink) emblem = "emblem-symbolic-link";
    else if (!d.readable) emblem = "emblem-unreadable";
    if (emblem) gtk_image_set_from_icon_name(GTK_IMAGE(c->emblem), emblem);
    gtk_widget_set_visible(c->emblem, emblem != nullptr);
    if (d.cut) gtk_widget_add_css_class(c->root, "cut");
    else gtk_widget_remove_css_class(c->root, "cut");
    if (d.isHidden) gtk_widget_add_css_class(c->root, "hidden-item");
    else gtk_widget_remove_css_class(c->root, "hidden-item");
    if (c->mode == 2) {
        int depth = obj && GTK_IS_TREE_LIST_ROW(obj) ? (int)gtk_tree_list_row_get_depth(GTK_TREE_LIST_ROW(obj)) : 0;
        gtk_widget_set_size_request(c->indent, 16 * depth + 4, -1);
        bool expandable = obj && GTK_IS_TREE_LIST_ROW(obj) && gtk_tree_list_row_is_expandable(GTK_TREE_LIST_ROW(obj));
        gtk_widget_set_visible(c->expander, tree_);
        if (expandable)
            gtk_image_set_from_icon_name(GTK_IMAGE(c->expander), gtk_tree_list_row_get_expanded(GTK_TREE_LIST_ROW(obj)) ? "pan-down-symbolic" : "pan-end-symbolic");
        else
            gtk_image_clear(GTK_IMAGE(c->expander));
        if (!columns_.empty()) {
            GtkWidget *nameBox = gtk_widget_get_first_child(c->root);
            gtk_widget_set_size_request(nameBox, columns_[0].width, -1);
            for (size_t i = 0; i < c->extras.size() && i + 1 < columns_.size(); ++i)
                gtk_widget_set_size_request(c->extras[i], std::max(0, columns_[i + 1].width - 12), -1);
        }
    }
    gtk_accessible_update_property(GTK_ACCESSIBLE(c->root), GTK_ACCESSIBLE_PROPERTY_LABEL, d.name.c_str(), -1);
    updateCellState(c);
}

void ItemView::updateCellState(Cell *c)
{
    if (!c->item || !sel_) return;
    bool selected = c->pos >= 0 && c->pos < count() && gtk_selection_model_is_selected(sel_, c->pos);
    if (selected) gtk_widget_set_state_flags(c->highlight, GTK_STATE_FLAG_SELECTED, FALSE);
    else gtk_widget_unset_state_flags(c->highlight, GTK_STATE_FLAG_SELECTED);
    bool isCurrent = c->item == current_ && gtk_widget_has_focus(w_);
    if (isCurrent) gtk_widget_add_css_class(c->root, "current");
    else gtk_widget_remove_css_class(c->root, "current");
    if (c->pos == dropPos_ && dropPos_ >= 0) gtk_widget_set_state_flags(c->highlight, GTK_STATE_FLAG_DROP_ACTIVE, FALSE);
    else gtk_widget_unset_state_flags(c->highlight, GTK_STATE_FLAG_DROP_ACTIVE);
    bool hovered = c->item == hover_;
    bool showToggle = selectionMode_ || (settings().showSelectionToggle && (hovered || selected) && c->mode != 2);
    if (c->mode == 2) showToggle = selectionMode_;
    gtk_widget_set_visible(c->toggle, showToggle);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c->toggle), selected);
}

// ---- header (details) -----------------------------------------------------------------

void ItemView::rebuildHeader()
{
    if (header_) {
        gtk_widget_unparent(header_);
        header_ = nullptr;
        headerCells_.clear();
    }
    if (mode_ != 2 || !headerVisible_) return;
    header_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(header_, "beluga-header");
    gtk_widget_set_can_target(header_, FALSE);
    gtk_widget_insert_before(header_, w_, rubber_);
    std::vector<std::string> roles = {"text"};
    roles.insert(roles.end(), roles_.begin(), roles_.end());
    for (auto &r : roles) {
        GtkWidget *b = gtk_button_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *l = gtk_label_new(role_title(r).c_str());
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
        gtk_widget_set_hexpand(l, TRUE);
        GtkWidget *arrow = gtk_image_new();
        gtk_box_append(GTK_BOX(box), l);
        gtk_box_append(GTK_BOX(box), arrow);
        gtk_button_set_child(GTK_BUTTON(b), box);
        gtk_widget_set_can_focus(b, FALSE);
        gtk_widget_add_css_class(b, "header-cell");
        g_object_set_data_full(G_OBJECT(b), "role", g_strdup(r.c_str()), g_free);
        g_object_set_data(G_OBJECT(b), "arrow", arrow);
        gtk_box_append(GTK_BOX(header_), b);
        headerCells_.push_back(b);
    }
    updateHeaderLabels();
}

void ItemView::ensureHeaderCells()
{
    if (!header_) rebuildHeader();
    if (!header_) return;
    for (size_t i = 0; i < headerCells_.size() && i < columns_.size(); ++i)
        gtk_widget_set_size_request(headerCells_[i], columns_[i].width, -1);
}

void ItemView::updateHeaderLabels()
{
    for (GtkWidget *b : headerCells_) {
        auto *role = static_cast<const char *>(g_object_get_data(G_OBJECT(b), "role"));
        auto *arrow = GTK_WIDGET(g_object_get_data(G_OBJECT(b), "arrow"));
        if (role && sortRole_ == role) gtk_image_set_from_icon_name(GTK_IMAGE(arrow), sortDesc_ ? "pan-down-symbolic" : "pan-up-symbolic");
        else gtk_image_clear(GTK_IMAGE(arrow));
    }
}

int ItemView::columnBoundaryAt(double x) const
{
    double hv = hadj_ ? gtk_adjustment_get_value(hadj_) : 0;
    double cum = -hv;
    for (size_t i = 0; i < columns_.size(); ++i) {
        cum += columns_[i].width;
        if (std::fabs(x - cum) <= 5) return (int)i;
    }
    return -1;
}

// ---- allocation -------------------------------------------------------------------------

void ItemView::allocate(int width, int height)
{
    if (disposed_) return;
    allocCount++;
    if (width != lastW_ || height != lastH_) layoutDirty_ = true;
    if (layoutDirty_) {
        computeLayout(width, height);
        layoutDirty_ = false;
        lastW_ = width;
        lastH_ = height;
    }
    double vUpper = vertical() ? std::max(contentH_, (double)height) : height;
    double hUpper = mode_ == 1 ? std::max(contentW_, (double)width) : mode_ == 2 ? std::max(contentW_, (double)width) : width;
    if (vadj_) {
        double v = std::clamp(gtk_adjustment_get_value(vadj_), 0.0, std::max(0.0, vUpper - height));
        gtk_adjustment_configure(vadj_, v, 0, vUpper, std::max(lineH_ * 3, 24.0), height * 0.9, height);
    }
    if (hadj_) {
        double v = std::clamp(gtk_adjustment_get_value(hadj_), 0.0, std::max(0.0, hUpper - width));
        double step = mode_ == 1 ? std::max(rowH_, 32.0) * 2 : 24.0;
        gtk_adjustment_configure(hadj_, v, 0, hUpper, step, width * 0.9, width);
    }
    double hv = hadj_ ? gtk_adjustment_get_value(hadj_) : 0;
    double vv = vadj_ ? gtk_adjustment_get_value(vadj_) : 0;

    for (Cell *c : cells_) c->used = false;
    for (auto &g : groupHeaders_) g.used = false;

    int n = count();
    if (currentPos_ >= n) currentPos_ = -1;
    auto useGroupHeader = [&](int section, const GtkAllocation &a) {
        GroupHeader *gh = nullptr;
        for (auto &g : groupHeaders_)
            if (!g.used && g.section == section) gh = &g;
        if (!gh)
            for (auto &g : groupHeaders_)
                if (!g.used) {
                    gh = &g;
                    break;
                }
        if (!gh) {
            GroupHeader g;
            g.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_add_css_class(g.root, "group-header");
            g.label = gtk_label_new(nullptr);
            gtk_label_set_xalign(GTK_LABEL(g.label), 0.0f);
            gtk_label_set_ellipsize(GTK_LABEL(g.label), PANGO_ELLIPSIZE_END);
            gtk_box_append(GTK_BOX(g.root), g.label);
            gtk_box_append(GTK_BOX(g.root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            gtk_widget_set_can_target(g.root, FALSE);
            gtk_widget_insert_after(g.root, w_, nullptr);
            groupHeaders_.push_back(g);
            gh = &groupHeaders_.back();
        }
        gh->used = true;
        if (gh->section != section) {
            gh->section = section;
            gtk_label_set_text(GTK_LABEL(gh->label), sections_[section].label.c_str());
        }
        gtk_widget_set_child_visible(gh->root, TRUE);
        int m, nn;
        gtk_widget_measure(gh->root, GTK_ORIENTATION_HORIZONTAL, -1, &m, &nn, nullptr, nullptr);
        gtk_widget_measure(gh->root, GTK_ORIENTATION_VERTICAL, a.width, &m, &nn, nullptr, nullptr);
        gtk_widget_size_allocate(gh->root, &a, -1);
    };

    auto placeItem = [&](int pos) {
        double x, y, w, h;
        if (!contentRect(pos, x, y, w, h)) return;
        gpointer obj = g_list_model_get_item(G_LIST_MODEL(sel_), pos);
        BelugaItem *item = item_from_object(obj);
        if (!item) {
            if (obj) g_object_unref(obj);
            return;
        }
        Cell *c;
        auto it = bound_.find(item);
        if (it != bound_.end() && !it->second->used && it->second->mode == mode_) {
            c = it->second;
            c->used = true;
            if (c->pos != pos || c->rowObj != obj) bindCell(c, pos, obj, item);
            else updateCellState(c);
        } else {
            c = takeCell();
            bindCell(c, pos, obj, item);
        }
        if (item == current_) currentPos_ = pos;
        g_object_unref(obj);
        gtk_widget_set_child_visible(c->root, TRUE);
        GtkAllocation a = {(int)std::floor(x - hv), (int)std::floor(y - vv), (int)std::ceil(w), (int)std::ceil(h)};
        if (mode_ == 2) a.width = (int)std::max(contentW_, (double)width);
        int m, nn;
        gtk_widget_measure(c->root, GTK_ORIENTATION_HORIZONTAL, -1, &m, &nn, nullptr, nullptr);
        if (mode_ != 0) a.width = std::max(a.width, m);
        gtk_widget_measure(c->root, GTK_ORIENTATION_VERTICAL, a.width, &m, &nn, nullptr, nullptr);
        gtk_widget_size_allocate(c->root, &a, -1);
    };

    double gh = std::ceil(lineH_ + 18);
    if (n > 0) {
        if (vertical()) {
            double top = vv + (mode_ == 2 ? headerHeight() : 0), bottom = vv + height;
            // First line that ends below the top edge.
            int lo = 0, hi = (int)lines_.size() - 1, first = (int)lines_.size();
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                if (lines_[mid].off + lines_[mid].ext > top) {
                    first = mid;
                    hi = mid - 1;
                } else {
                    lo = mid + 1;
                }
            }
            for (int li = first; li < (int)lines_.size() && lines_[li].off < bottom; ++li) {
                const Line &l = lines_[li];
                if (l.header) {
                    GtkAllocation a = {(int)(margin_ - hv), (int)(l.off - vv), (int)(std::max(contentW_, (double)width) - 2 * margin_), (int)l.ext};
                    useGroupHeader(l.section, a);
                    continue;
                }
                for (int p = l.first; p < l.first + l.count; ++p) placeItem(p);
            }
        } else {
            for (auto &l : lines_) {
                if (l.off + l.ext < hv || l.off > hv + width) continue;
                for (int p = l.first; p < l.first + l.count; ++p) placeItem(p);
            }
            if (grouped_)
                for (size_t si = 0; si < sections_.size(); ++si) {
                    const Section &s = sections_[si];
                    if (s.off + s.ext < hv || s.off > hv + width) continue;
                    GtkAllocation a = {(int)(s.off - hv), (int)margin_, (int)std::max(s.ext, 60.0), (int)gh};
                    useGroupHeader((int)si, a);
                }
        }
    }

    for (Cell *c : cells_)
        if (!c->used) {
            if (c->item) unbindCell(c);
            gtk_widget_set_child_visible(c->root, FALSE);
        }
    for (auto &g : groupHeaders_)
        if (!g.used) {
            gtk_widget_set_child_visible(g.root, FALSE);
            g.section = -1;
        }

    if (header_) {
        GtkAllocation a = {(int)-hv, 0, (int)std::max(contentW_, (double)width), (int)headerHeight()};
        int m, nn;
        gtk_widget_measure(header_, GTK_ORIENTATION_HORIZONTAL, -1, &m, &nn, nullptr, nullptr);
        a.width = std::max(a.width, m);
        gtk_widget_measure(header_, GTK_ORIENTATION_VERTICAL, a.width, &m, &nn, nullptr, nullptr);
        gtk_widget_size_allocate(header_, &a, -1);
    }
    if (rubber_) {
        double x0 = std::min(rbX0_, rbX1_), y0 = std::min(rbY0_, rbY1_);
        double x1 = std::max(rbX0_, rbX1_), y1 = std::max(rbY0_, rbY1_);
        GtkAllocation a = {(int)(x0 - hv), (int)(y0 - vv), std::max(1, (int)(x1 - x0)), std::max(1, (int)(y1 - y0))};
        int m, nn;
        gtk_widget_measure(rubber_, GTK_ORIENTATION_HORIZONTAL, -1, &m, &nn, nullptr, nullptr);
        gtk_widget_measure(rubber_, GTK_ORIENTATION_VERTICAL, -1, &m, &nn, nullptr, nullptr);
        gtk_widget_size_allocate(rubber_, &a, -1);
    }
    if (renameEntry_) {
        int pos = -1;
        if (renameItem_) {
            auto it = bound_.find(renameItem_);
            if (it != bound_.end() && it->second->used) pos = it->second->pos;
        }
        double x, y, w, h;
        if (pos >= 0 && contentRect(pos, x, y, w, h)) {
            int m, nat;
            gtk_widget_measure(renameEntry_, GTK_ORIENTATION_HORIZONTAL, -1, &m, &nat, nullptr, nullptr);
            int eh, ehn;
            gtk_widget_measure(renameEntry_, GTK_ORIENTATION_VERTICAL, -1, &eh, &ehn, nullptr, nullptr);
            GtkAllocation a;
            if (mode_ == 0) {
                a = {(int)(x - hv), (int)(y - vv + iconSize_ + (settings().iconsCompactSpacing ? 8 : 14)), (int)std::max(w, (double)m), eh};
            } else {
                double start = x + iconSize_ + 10;
                if (mode_ == 2) {
                    gpointer obj = g_list_model_get_item(G_LIST_MODEL(sel_), pos);
                    if (GTK_IS_TREE_LIST_ROW(obj)) start += 16.0 * gtk_tree_list_row_get_depth(GTK_TREE_LIST_ROW(obj)) + 4 + (tree_ ? 20 : 0);
                    if (obj) g_object_unref(obj);
                }
                double ww = mode_ == 2 ? columns_[0].width - (start - x) : w - (start - x);
                a = {(int)(start - hv), (int)(y - vv + (h - eh) / 2), (int)std::max(ww, (double)m), eh};
            }
            gtk_widget_set_child_visible(renameEntry_, TRUE);
            gtk_widget_size_allocate(renameEntry_, &a, -1);
        } else {
            gtk_widget_set_child_visible(renameEntry_, FALSE);
        }
    }
}

void ItemView::snapshot(GtkSnapshot *s)
{
    for (GtkWidget *c = gtk_widget_get_first_child(w_); c; c = gtk_widget_get_next_sibling(c))
        gtk_widget_snapshot_child(w_, c, s);
}

// ---- scrolling & current item ------------------------------------------------------------

void ItemView::scrollTo(int pos)
{
    if (layoutDirty_) {
        int w = gtk_widget_get_width(w_), h = gtk_widget_get_height(w_);
        if (w <= 0 || h <= 0) return;
        computeLayout(w, h);
        layoutDirty_ = false;
        lastW_ = w;
        lastH_ = h;
    }
    double x, y, w, h;
    if (!contentRect(pos, x, y, w, h)) return;
    if (vertical() && vadj_) {
        double v = gtk_adjustment_get_value(vadj_), page = gtk_adjustment_get_page_size(vadj_);
        double top = mode_ == 2 ? headerHeight() : 0;
        if (page <= 0) page = gtk_widget_get_height(w_);
        if (y - top < v) gtk_adjustment_set_value(vadj_, y - top);
        else if (y + h > v + page) gtk_adjustment_set_value(vadj_, y + h - page);
    } else if (hadj_) {
        double v = gtk_adjustment_get_value(hadj_), page = gtk_adjustment_get_page_size(hadj_);
        if (page <= 0) page = gtk_widget_get_width(w_);
        if (x < v) gtk_adjustment_set_value(hadj_, x);
        else if (x + w > v + page) gtk_adjustment_set_value(hadj_, x + w - page);
    }
}

void ItemView::scrollToTop()
{
    if (vadj_) gtk_adjustment_set_value(vadj_, 0);
    if (hadj_) gtk_adjustment_set_value(hadj_, 0);
}

double ItemView::scrollPosition() const
{
    GtkAdjustment *a = vertical() ? vadj_ : hadj_;
    return a ? gtk_adjustment_get_value(a) : 0;
}

void ItemView::setScrollPosition(double v)
{
    GtkAdjustment *a = vertical() ? vadj_ : hadj_;
    if (!a) return;
    if (gtk_adjustment_get_upper(a) < v + gtk_adjustment_get_page_size(a)) {
        // Layout not done yet: retry after the next allocation.
        struct D { ItemView *v; double val; int tries; };
        auto *d = new D{this, v, 0};
        g_timeout_add(30, [](gpointer p) -> gboolean {
            auto *d = static_cast<D *>(p);
            if (d->v->disposed_) { delete d; return G_SOURCE_REMOVE; }
            GtkAdjustment *a = d->v->vertical() ? d->v->vadj_ : d->v->hadj_;
            if (a && (gtk_adjustment_get_upper(a) >= d->val + gtk_adjustment_get_page_size(a) || ++d->tries > 10)) {
                gtk_adjustment_set_value(a, d->val);
                delete d;
                return G_SOURCE_REMOVE;
            }
            return G_SOURCE_CONTINUE;
        }, d);
        return;
    }
    gtk_adjustment_set_value(a, v);
}

int ItemView::currentPosition()
{
    if (!current_ || !sel_) return -1;
    if (currentPos_ >= 0 && currentPos_ < count() && item_at(G_LIST_MODEL(sel_), currentPos_) == current_) return currentPos_;
    int n = count();
    for (int i = 0; i < n; ++i)
        if (item_at(G_LIST_MODEL(sel_), i) == current_) return currentPos_ = i;
    return -1;
}

void ItemView::setCurrent(int pos, bool select, bool scroll)
{
    if (!sel_ || pos < 0 || pos >= count()) return;
    BelugaItem *item = item_at(G_LIST_MODEL(sel_), pos);
    g_set_weak_pointer(&current_, item);
    g_set_weak_pointer(&anchor_, item);
    currentPos_ = pos;
    if (select) gtk_selection_model_select_item(sel_, pos, TRUE);
    if (scroll) scrollTo(pos);
    for (Cell *c : cells_)
        if (c->used && c->item) updateCellState(c);
    gtk_widget_queue_allocate(w_);
}

std::vector<int> ItemView::selectedPositions() const
{
    std::vector<int> v;
    if (!sel_) return v;
    GtkBitset *bs = gtk_selection_model_get_selection(sel_);
    GtkBitsetIter iter;
    guint val;
    if (gtk_bitset_iter_init_first(&iter, bs, &val)) {
        do v.push_back((int)val);
        while (gtk_bitset_iter_next(&iter, &val));
    }
    gtk_bitset_unref(bs);
    return v;
}

void ItemView::selectAll()
{
    if (sel_) gtk_selection_model_select_all(sel_);
}

void ItemView::clearSelection()
{
    if (sel_) gtk_selection_model_unselect_all(sel_);
}

void ItemView::invertSelection()
{
    if (!sel_) return;
    int n = count();
    GtkBitset *all = gtk_bitset_new_range(0, n);
    GtkBitset *cur = gtk_selection_model_get_selection(sel_);
    GtkBitset *inv = gtk_bitset_copy(all);
    gtk_bitset_subtract(inv, cur);
    gtk_selection_model_set_selection(sel_, inv, all);
    gtk_bitset_unref(all);
    gtk_bitset_unref(cur);
    gtk_bitset_unref(inv);
}

void ItemView::selectItems(const std::vector<BelugaItem *> &items, bool scroll)
{
    if (!sel_ || items.empty()) return;
    int n = count();
    GtkBitset *bs = gtk_bitset_new_empty();
    int first = -1;
    std::unordered_map<BelugaItem *, bool> want;
    for (BelugaItem *i : items) want[i] = true;
    for (int i = 0; i < n; ++i)
        if (want.count(item_at(G_LIST_MODEL(sel_), i))) {
            gtk_bitset_add(bs, i);
            if (first < 0) first = i;
        }
    GtkBitset *all = gtk_bitset_new_range(0, n);
    gtk_selection_model_set_selection(sel_, bs, all);
    gtk_bitset_unref(bs);
    gtk_bitset_unref(all);
    if (first >= 0) {
        BelugaItem *item = item_at(G_LIST_MODEL(sel_), first);
        g_set_weak_pointer(&current_, item);
        g_set_weak_pointer(&anchor_, item);
        currentPos_ = first;
        if (scroll) scrollTo(first);
    }
}

void ItemView::selectRange(int a, int b, bool unselectRest)
{
    if (a < 0) a = b;
    int lo = std::min(a, b), hi = std::max(a, b);
    gtk_selection_model_select_range(sel_, lo, hi - lo + 1, unselectRest);
}

void ItemView::moveCurrent(int pos, GdkModifierType mods)
{
    int n = count();
    if (n == 0) return;
    pos = std::clamp(pos, 0, n - 1);
    BelugaItem *item = item_at(G_LIST_MODEL(sel_), pos);
    if (mods & GDK_SHIFT_MASK) {
        int a = -1;
        if (anchor_)
            for (int i = 0; i < n; ++i)
                if (item_at(G_LIST_MODEL(sel_), i) == anchor_) {
                    a = i;
                    break;
                }
        selectRange(a, pos, !(mods & GDK_CONTROL_MASK));
    } else if (!(mods & GDK_CONTROL_MASK) && !selectionMode_) {
        gtk_selection_model_select_item(sel_, pos, TRUE);
        g_set_weak_pointer(&anchor_, item);
    }
    g_set_weak_pointer(&current_, item);
    currentPos_ = pos;
    scrollTo(pos);
    for (Cell *c : cells_)
        if (c->used && c->item) updateCellState(c);
}

int ItemView::neighbor(int pos, int dx, int dy)
{
    int li = lineOf(pos);
    if (li < 0) return pos;
    const Line &l = lines_[li];
    int idx = pos - l.first;
    auto stepLine = [&](int dir) -> int {
        int j = li + dir;
        while (j >= 0 && j < (int)lines_.size() && lines_[j].header) j += dir;
        if (j < 0 || j >= (int)lines_.size()) return pos;
        return lines_[j].first + std::min(idx, lines_[j].count - 1);
    };
    if (mode_ == 0) {
        if (dx) return pos + dx;
        return stepLine(dy);
    }
    if (mode_ == 1) {
        if (dy) return pos + dy;
        return stepLine(dx);
    }
    return pos + dy;
}

// ---- rubber band & autoscroll --------------------------------------------------------------

void ItemView::updateRubberband()
{
    if (!rubberActive_ || !sel_) return;
    double hv = hadj_ ? gtk_adjustment_get_value(hadj_) : 0;
    double vv = vadj_ ? gtk_adjustment_get_value(vadj_) : 0;
    rbX1_ = lastPointerX_ + hv;
    rbY1_ = lastPointerY_ + vv;
    double x0 = std::min(rbX0_, rbX1_), x1 = std::max(rbX0_, rbX1_);
    double y0 = std::min(rbY0_, rbY1_), y1 = std::max(rbY0_, rbY1_);
    GtkBitset *bs = gtk_bitset_new_empty();
    for (auto &l : lines_) {
        if (l.header) continue;
        if (vertical() ? (l.off + l.ext < y0 || l.off > y1) : (l.off + l.ext < x0 || l.off > x1)) continue;
        for (int p = l.first; p < l.first + l.count; ++p) {
            double x, y, w, h;
            if (!contentRect(p, x, y, w, h)) continue;
            if (mode_ == 2) w = std::min(w, (double)columns_[0].width);
            if (x < x1 && x + w > x0 && y < y1 && y + h > y0) gtk_bitset_add(bs, p);
        }
    }
    if (rbToggle_ && rbBase_) {
        GtkBitset *res = gtk_bitset_copy(rbBase_);
        gtk_bitset_difference(res, bs);
        gtk_bitset_unref(bs);
        bs = res;
    } else if (rbBase_) {
        gtk_bitset_union(bs, rbBase_);
    }
    GtkBitset *all = gtk_bitset_new_range(0, count());
    gtk_selection_model_set_selection(sel_, bs, all);
    gtk_bitset_unref(all);
    gtk_bitset_unref(bs);
    gtk_widget_queue_allocate(w_);
}

void ItemView::stopAutoscroll()
{
    if (autoscrollSource_) g_source_remove(autoscrollSource_);
    autoscrollSource_ = 0;
    autoscrollDx_ = autoscrollDy_ = 0;
}

void ItemView::autoscrollStep()
{
    if (vadj_ && autoscrollDy_) gtk_adjustment_set_value(vadj_, gtk_adjustment_get_value(vadj_) + autoscrollDy_);
    if (hadj_ && autoscrollDx_) gtk_adjustment_set_value(hadj_, gtk_adjustment_get_value(hadj_) + autoscrollDx_);
}

static void set_autoscroll(ItemView *v, double &dx, double &dy, guint &source, double x, double y, int w, int h, double topMargin)
{
    const double edge = 28;
    dx = dy = 0;
    if (y < topMargin + edge) dy = -std::max(4.0, (topMargin + edge - y) / 2);
    else if (y > h - edge) dy = std::max(4.0, (y - (h - edge)) / 2);
    if (x < edge) dx = -std::max(4.0, (edge - x) / 2);
    else if (x > w - edge) dx = std::max(4.0, (x - (w - edge)) / 2);
    if ((dx || dy) && !source) {
        source = g_timeout_add(30, [](gpointer p) -> gboolean {
            auto *v = static_cast<ItemView *>(p);
            v->autoscrollStep();
            return G_SOURCE_CONTINUE;
        }, v);
    }
}

// ---- type-ahead ----------------------------------------------------------------------------

bool ItemView::typeahead(gunichar c)
{
    gint64 now = g_get_monotonic_time();
    if (now - typeaheadTime_ > 1000000) typeahead_.clear();
    typeaheadTime_ = now;
    char buf[8];
    int len = g_unichar_to_utf8(c, buf);
    typeahead_.append(buf, len);
    std::string needle = casefold(typeahead_);
    int n = count();
    if (n == 0) return true;
    int cur = currentPosition();
    int start = cur < 0 ? 0 : (typeahead_.size() == (size_t)len ? cur + 1 : cur);
    for (int k = 0; k < n; ++k) {
        int i = (start + k) % n;
        BelugaItem *it = item_at(G_LIST_MODEL(sel_), i);
        if (it && starts_with(casefold(item_data(it).name), needle)) {
            moveCurrent(i, GdkModifierType(0));
            return true;
        }
    }
    return true;
}

// ---- drag and drop helpers -------------------------------------------------------------------

void ItemView::setDropTarget(int pos)
{
    if (pos == dropPos_) return;
    dropPos_ = pos;
    for (Cell *c : cells_)
        if (c->used && c->item) updateCellState(c);
}

// ---- inline rename ---------------------------------------------------------------------------

void ItemView::startRename(int pos, int selStart, int selEnd, std::function<void(const std::string &)> done)
{
    if (renameEntry_) finishRename(false);
    if (pos < 0 || pos >= count()) return;
    scrollTo(pos);
    BelugaItem *item = item_at(G_LIST_MODEL(sel_), pos);
    g_set_weak_pointer(&renameItem_, item);
    renameDone_ = std::move(done);
    renameEntry_ = gtk_entry_new();
    gtk_widget_add_css_class(renameEntry_, "beluga-rename");
    gtk_editable_set_text(GTK_EDITABLE(renameEntry_), item_data(item).name.c_str());
    gtk_widget_insert_before(renameEntry_, w_, nullptr);
    g_signal_connect_swapped(renameEntry_, "activate", CB([](ItemView *v) { v->finishRename(true); }), this);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer p) -> gboolean {
        if (keyval == GDK_KEY_Escape) {
            static_cast<ItemView *>(p)->finishRename(false);
            return TRUE;
        }
        return FALSE;
    }), this);
    gtk_widget_add_controller(renameEntry_, key);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect_swapped(focus, "leave", CB([](ItemView *v) {
        g_idle_add([](gpointer p) -> gboolean {
            auto *v = static_cast<ItemView *>(p);
            if (v->renameEntry_ && !gtk_widget_has_focus(v->renameEntry_) && !gtk_widget_is_focus(v->renameEntry_)) {
                GtkWidget *f = gtk_root_get_focus(gtk_widget_get_root(v->w_));
                if (!f || !gtk_widget_is_ancestor(f, v->renameEntry_)) v->finishRename(true);
            }
            return G_SOURCE_REMOVE;
        }, v);
    }), this);
    gtk_widget_add_controller(renameEntry_, focus);
    gtk_widget_grab_focus(renameEntry_);
    gtk_editable_select_region(GTK_EDITABLE(renameEntry_), selStart, selEnd);
    gtk_widget_queue_allocate(w_);
}

void ItemView::finishRename(bool commit)
{
    if (!renameEntry_) return;
    std::string text = gtk_editable_get_text(GTK_EDITABLE(renameEntry_));
    GtkWidget *e = renameEntry_;
    renameEntry_ = nullptr;
    auto done = std::move(renameDone_);
    renameDone_ = nullptr;
    g_clear_weak_pointer(&renameItem_);
    gtk_widget_unparent(e);
    gtk_widget_grab_focus(w_);
    gtk_widget_queue_allocate(w_);
    if (commit && done) done(text);
}

// ---- controllers ---------------------------------------------------------------------------

void ItemView::setupControllers()
{
    // Clicks.
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", CB([](GtkGestureClick *g, int nPress, double x, double y, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
        GdkModifierType mods = current_mods(GTK_EVENT_CONTROLLER(g));
        if (!v->renameEntry_) gtk_widget_grab_focus(v->w_);
        v->dragStarted_ = false;
        v->deferSelect_ = false;
        v->pressPos_ = -1;
        if (!v->sel_) return;
        if (v->mode_ == 2 && y < v->headerHeight()) {
            if (button == 3 && v->onHeaderMenu) v->onHeaderMenu(x, y);
            return;
        }
        int pos = v->itemAt(x, y);
        if (button == 8 || button == 9) {
            if (v->onExtraButton) v->onExtraButton((int)button);
            return;
        }
        if (button == 2) {
            if (pos >= 0 && v->onMiddleClick) v->onMiddleClick(pos);
            return;
        }
        if (button == 3) {
            if (pos >= 0) {
                if (!gtk_selection_model_is_selected(v->sel_, pos)) gtk_selection_model_select_item(v->sel_, pos, TRUE);
                BelugaItem *it = item_at(G_LIST_MODEL(v->sel_), pos);
                g_set_weak_pointer(&v->current_, it);
                g_set_weak_pointer(&v->anchor_, it);
                v->currentPos_ = pos;
            } else if (!(mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) && !v->selectionMode_) {
                gtk_selection_model_unselect_all(v->sel_);
            }
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            if (v->onContextMenu) v->onContextMenu(pos, x, y);
            return;
        }
        if (button != 1) return;
        v->pressPos_ = pos;
        if (pos < 0) {
            if (nPress == 2 && v->onEmptyDoubleClick) v->onEmptyDoubleClick();
            if (!(mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) && !v->selectionMode_) gtk_selection_model_unselect_all(v->sel_);
            return;
        }
        // Expander in the details tree.
        if (v->mode_ == 2 && v->tree_) {
            gpointer obj = g_list_model_get_item(G_LIST_MODEL(v->sel_), pos);
            if (GTK_IS_TREE_LIST_ROW(obj) && gtk_tree_list_row_is_expandable(GTK_TREE_LIST_ROW(obj))) {
                double cx = x, cy = y;
                v->toContent(cx, cy);
                double ex = 16.0 * gtk_tree_list_row_get_depth(GTK_TREE_LIST_ROW(obj)) + 4;
                if (cx >= ex && cx <= ex + 18) {
                    gtk_tree_list_row_set_expanded(GTK_TREE_LIST_ROW(obj), !gtk_tree_list_row_get_expanded(GTK_TREE_LIST_ROW(obj)));
                    g_object_unref(obj);
                    v->pressPos_ = -1;
                    return;
                }
            }
            if (obj) g_object_unref(obj);
        }
        // Selection toggle (top left of the icon).
        v->pressOnToggle_ = v->selectionMode_;
        if (!v->pressOnToggle_ && settings().showSelectionToggle && v->mode_ != 2) {
            GdkRectangle r;
            if (v->itemRect(pos, r)) {
                double ix = v->mode_ == 0 ? r.x + (r.width - v->iconSize_) / 2.0 : r.x + 4;
                double iy = v->mode_ == 0 ? r.y + (settings().iconsCompactSpacing ? 4 : 8) : r.y + (r.height - v->iconSize_) / 2.0;
                if (x >= ix && x <= ix + 22 && y >= iy && y <= iy + 22) v->pressOnToggle_ = true;
            }
        }
        BelugaItem *item = item_at(G_LIST_MODEL(v->sel_), pos);
        bool selected = gtk_selection_model_is_selected(v->sel_, pos);
        if (nPress == 2 && !v->pressOnToggle_ && !settings().singleClick && !(mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))) {
            if (v->onActivate) v->onActivate(pos, mods);
            return;
        }
        if (v->pressOnToggle_ || (mods & GDK_CONTROL_MASK)) {
            if (selected) gtk_selection_model_unselect_item(v->sel_, pos);
            else gtk_selection_model_select_item(v->sel_, pos, FALSE);
            g_set_weak_pointer(&v->anchor_, item);
        } else if (mods & GDK_SHIFT_MASK) {
            int a = -1;
            int n = v->count();
            if (v->anchor_)
                for (int i = 0; i < n; ++i)
                    if (item_at(G_LIST_MODEL(v->sel_), i) == v->anchor_) {
                        a = i;
                        break;
                    }
            v->selectRange(a, pos, TRUE);
        } else {
            if (selected) v->deferSelect_ = true;
            else gtk_selection_model_select_item(v->sel_, pos, TRUE);
            g_set_weak_pointer(&v->anchor_, item);
        }
        g_set_weak_pointer(&v->current_, item);
        v->currentPos_ = pos;
        for (Cell *c : v->cells_)
            if (c->used && c->item) v->updateCellState(c);
    }), this);
    g_signal_connect(click, "released", CB([](GtkGestureClick *g, int nPress, double x, double y, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
        GdkModifierType mods = current_mods(GTK_EVENT_CONTROLLER(g));
        if (!v->sel_ || button != 1) return;
        if (v->mode_ == 2 && y < v->headerHeight()) {
            if (v->resizeColumn_ >= 0) return;
            double hv = v->hadj_ ? gtk_adjustment_get_value(v->hadj_) : 0;
            double cum = -hv;
            for (auto &c : v->columns_) {
                if (x >= cum && x < cum + c.width) {
                    if (v->onHeaderClicked) v->onHeaderClicked(c.role);
                    break;
                }
                cum += c.width;
            }
            return;
        }
        int pos = v->itemAt(x, y);
        if (pos < 0 || pos != v->pressPos_ || v->dragStarted_ || v->rubberActive_) return;
        if (v->deferSelect_) gtk_selection_model_select_item(v->sel_, pos, TRUE);
        v->deferSelect_ = false;
        if (settings().singleClick && !v->selectionMode_ && !v->pressOnToggle_ && nPress == 1
            && !(mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) && v->onActivate)
            v->onActivate(pos, mods);
    }), this);
    gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(click));

    // Rubber band and column resizing.
    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", CB([](GtkGestureDrag *g, double x, double y, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        v->resizeColumn_ = -1;
        v->rubberActive_ = false;
        if (!v->sel_) {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
            return;
        }
        if (v->mode_ == 2 && y < v->headerHeight()) {
            int col = v->columnBoundaryAt(x);
            if (col >= 0) {
                v->resizeColumn_ = col;
                v->resizeStartW_ = v->columns_[col].width;
                gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            } else {
                gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
            }
            return;
        }
        int pos = v->itemAt(x, y);
        if (pos >= 0 && v->contentHit(pos, x, y)) {
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_DENIED);
            return;
        }
        GdkModifierType mods = current_mods(GTK_EVENT_CONTROLLER(g));
        double cx = x, cy = y;
        v->toContent(cx, cy);
        v->rbX0_ = v->rbX1_ = cx;
        v->rbY0_ = v->rbY1_ = cy;
        v->lastPointerX_ = x;
        v->lastPointerY_ = y;
        if (v->rbBase_) gtk_bitset_unref(v->rbBase_);
        v->rbBase_ = nullptr;
        v->rbToggle_ = (mods & GDK_CONTROL_MASK) != 0;
        if (mods & (GDK_CONTROL_MASK | GDK_SHIFT_MASK) || v->selectionMode_) v->rbBase_ = gtk_selection_model_get_selection(v->sel_);
    }), this);
    g_signal_connect(drag, "drag-update", CB([](GtkGestureDrag *g, double dx, double dy, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        if (v->resizeColumn_ >= 0) {
            int w = std::max(40, (int)(v->resizeStartW_ + dx));
            Column &c = v->columns_[v->resizeColumn_];
            c.width = w;
            v->savedWidths_[c.role] = w;
            v->layoutDirty_ = true;
            if (v->header_) v->ensureHeaderCells();
            v->refreshAll();
            return;
        }
        double sx, sy;
        gtk_gesture_drag_get_start_point(g, &sx, &sy);
        v->lastPointerX_ = sx + dx;
        v->lastPointerY_ = sy + dy;
        if (!v->rubberActive_) {
            if (std::hypot(dx, dy) < 5) return;
            v->rubberActive_ = true;
            gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
            v->rubber_ = GTK_WIDGET(g_object_new(GTK_TYPE_BOX, "css-name", "rubberband", nullptr));
            gtk_widget_add_css_class(v->rubber_, "rubberband");
            gtk_widget_set_can_target(v->rubber_, FALSE);
            gtk_widget_insert_before(v->rubber_, v->w_, nullptr);
        }
        set_autoscroll(v, v->autoscrollDx_, v->autoscrollDy_, v->autoscrollSource_, v->lastPointerX_, v->lastPointerY_,
                       gtk_widget_get_width(v->w_), gtk_widget_get_height(v->w_), v->headerHeight());
        if (!v->autoscrollDx_ && !v->autoscrollDy_) v->stopAutoscroll();
        v->updateRubberband();
    }), this);
    g_signal_connect(drag, "drag-end", CB([](GtkGestureDrag *, double, double, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        v->stopAutoscroll();
        if (v->resizeColumn_ >= 0) {
            v->resizeColumn_ = -1;
            if (v->onColumnsResized) v->onColumnsResized();
            return;
        }
        if (v->rubber_) {
            gtk_widget_unparent(v->rubber_);
            v->rubber_ = nullptr;
        }
        v->rubberActive_ = false;
        if (v->rbBase_) gtk_bitset_unref(v->rbBase_);
        v->rbBase_ = nullptr;
        gtk_widget_queue_allocate(v->w_);
    }), this);
    gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(drag));

    // Hover.
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", CB([](GtkEventControllerMotion *, double x, double y, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        if (!v->sel_) return;
        int pos = v->itemAt(x, y);
        BelugaItem *item = pos >= 0 ? item_at(G_LIST_MODEL(v->sel_), pos) : nullptr;
        if (item == v->hover_) return;
        BelugaItem *old = v->hover_;
        g_set_weak_pointer(&v->hover_, item);
        for (Cell *c : v->cells_)
            if (c->used && c->item && (c->item == old || c->item == item)) v->updateCellState(c);
        if (v->mode_ == 0 || v->mode_ == 1) {
            if (settings().singleClick && item) gtk_widget_set_cursor_from_name(v->w_, "pointer");
            else gtk_widget_set_cursor(v->w_, nullptr);
        }
        if (v->onHover) v->onHover(pos);
    }), this);
    g_signal_connect(motion, "leave", CB([](GtkEventControllerMotion *, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        BelugaItem *old = v->hover_;
        g_clear_weak_pointer(&v->hover_);
        for (Cell *c : v->cells_)
            if (c->used && c->item == old) v->updateCellState(c);
        if (v->onHover) v->onHover(-1);
    }), this);
    gtk_widget_add_controller(w_, motion);

    // Wheel: Ctrl zooms; compact mode scrolls sideways.
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    gtk_event_controller_set_propagation_phase(scroll, GTK_PHASE_CAPTURE);
    g_signal_connect(scroll, "scroll", CB([](GtkEventControllerScroll *c, double dx, double dy, gpointer p) -> gboolean {
        auto *v = static_cast<ItemView *>(p);
        GdkModifierType mods = current_mods(GTK_EVENT_CONTROLLER(c));
        if (mods & GDK_CONTROL_MASK) {
            if (dy != 0 && v->onZoom) v->onZoom(dy < 0 ? 1 : -1);
            return TRUE;
        }
        if (v->mode_ == 1 && dx == 0 && dy != 0 && v->hadj_ && !(mods & GDK_SHIFT_MASK)) {
            double step = std::max(v->rowH_, 32.0) * 2;
            GdkScrollUnit unit = gtk_event_controller_scroll_get_unit(c);
            double delta = unit == GDK_SCROLL_UNIT_WHEEL ? dy * step : dy;
            gtk_adjustment_set_value(v->hadj_, gtk_adjustment_get_value(v->hadj_) + delta);
            return TRUE;
        }
        return FALSE;
    }), this);
    gtk_widget_add_controller(w_, scroll);

    // Keyboard.
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", CB([](GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer p) -> gboolean {
        auto *v = static_cast<ItemView *>(p);
        if (!v->sel_ || v->renameEntry_) return FALSE;
        GdkModifierType mods = GdkModifierType(state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
        int n = v->count();
        int cur = v->currentPosition();
        GdkModifierType navMods = GdkModifierType(mods & (GDK_SHIFT_MASK | GDK_CONTROL_MASK));
        if (mods & (GDK_ALT_MASK | GDK_SUPER_MASK)) return FALSE;
        auto go = [&](int pos) {
            if (n > 0) v->moveCurrent(pos, navMods);
            return TRUE;
        };
        switch (keyval) {
        case GDK_KEY_Up: case GDK_KEY_KP_Up:
            return cur < 0 ? go(0) : go(v->neighbor(cur, 0, -1));
        case GDK_KEY_Down: case GDK_KEY_KP_Down:
            return cur < 0 ? go(0) : go(v->neighbor(cur, 0, 1));
        case GDK_KEY_Left: case GDK_KEY_KP_Left:
            if (v->mode_ == 2) {
                if (cur < 0) return go(0);
                gpointer obj = g_list_model_get_item(G_LIST_MODEL(v->sel_), cur);
                gboolean r = FALSE;
                if (GTK_IS_TREE_LIST_ROW(obj)) {
                    GtkTreeListRow *row = GTK_TREE_LIST_ROW(obj);
                    if (gtk_tree_list_row_get_expanded(row)) gtk_tree_list_row_set_expanded(row, FALSE);
                    else if (GtkTreeListRow *parent = gtk_tree_list_row_get_parent(row)) {
                        go((int)gtk_tree_list_row_get_position(parent));
                        g_object_unref(parent);
                    }
                    r = TRUE;
                }
                if (obj) g_object_unref(obj);
                return r;
            }
            return cur < 0 ? go(0) : go(v->neighbor(cur, -1, 0));
        case GDK_KEY_Right: case GDK_KEY_KP_Right:
            if (v->mode_ == 2) {
                if (cur < 0) return go(0);
                gpointer obj = g_list_model_get_item(G_LIST_MODEL(v->sel_), cur);
                gboolean r = FALSE;
                if (GTK_IS_TREE_LIST_ROW(obj) && gtk_tree_list_row_is_expandable(GTK_TREE_LIST_ROW(obj))) {
                    GtkTreeListRow *row = GTK_TREE_LIST_ROW(obj);
                    if (!gtk_tree_list_row_get_expanded(row)) gtk_tree_list_row_set_expanded(row, TRUE);
                    else go(cur + 1);
                    r = TRUE;
                }
                if (obj) g_object_unref(obj);
                return r;
            }
            return cur < 0 ? go(0) : go(v->neighbor(cur, 1, 0));
        case GDK_KEY_Home: case GDK_KEY_KP_Home:
            return go(0);
        case GDK_KEY_End: case GDK_KEY_KP_End:
            return go(n - 1);
        case GDK_KEY_Page_Up: case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Up: case GDK_KEY_KP_Page_Down: {
            bool up = keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_KP_Page_Up;
            int page = std::max(1, (int)(v->vertical() ? gtk_widget_get_height(v->w_) / std::max(1.0, v->mode_ == 0 ? v->itemH_ : v->rowH_)
                                                        : gtk_widget_get_width(v->w_) / 150));
            int p2 = cur < 0 ? 0 : cur;
            for (int i = 0; i < page; ++i) {
                int nb = v->mode_ == 1 ? v->neighbor(p2, up ? -1 : 1, 0) : v->neighbor(p2, 0, up ? -1 : 1);
                if (nb == p2) break;
                p2 = nb;
            }
            return go(p2);
        }
        case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter:
            if (cur >= 0 && v->onActivate) v->onActivate(cur, mods);
            return TRUE;
        case GDK_KEY_Menu:
            goto menu;
        case GDK_KEY_F10:
            if (mods & GDK_SHIFT_MASK) goto menu;
            return FALSE;
        case GDK_KEY_Escape:
            if (!v->typeahead_.empty()) {
                v->typeahead_.clear();
                return TRUE;
            }
            if (v->onEscape) v->onEscape();
            return FALSE;
        case GDK_KEY_space:
            if (mods & GDK_CONTROL_MASK) {
                if (cur >= 0) {
                    if (gtk_selection_model_is_selected(v->sel_, cur)) gtk_selection_model_unselect_item(v->sel_, cur);
                    else gtk_selection_model_select_item(v->sel_, cur, FALSE);
                }
                return TRUE;
            }
            if (!v->typeahead_.empty() && g_get_monotonic_time() - v->typeaheadTime_ < 1000000) return v->typeahead(' ');
            return FALSE;
        default:
            break;
        }
        if (!(mods & GDK_CONTROL_MASK)) {
            gunichar c = gdk_keyval_to_unicode(keyval);
            if (c && g_unichar_isprint(c) && c != ' ' && keyval != GDK_KEY_slash) return v->typeahead(c);
        }
        return FALSE;
    menu:
        if (v->onContextMenu) {
            GdkRectangle r;
            if (cur >= 0 && v->itemRect(cur, r)) v->onContextMenu(cur, r.x + r.width / 2.0, r.y + r.height / 2.0);
            else v->onContextMenu(-1, gtk_widget_get_width(v->w_) / 2.0, gtk_widget_get_height(v->w_) / 2.0);
        }
        return TRUE;
    }), this);
    gtk_widget_add_controller(w_, key);

    GtkEventController *focus = gtk_event_controller_focus_new();
    auto refocus = CB([](ItemView *v) {
        for (Cell *c : v->cells_)
            if (c->used && c->item) v->updateCellState(c);
    });
    g_signal_connect_swapped(focus, "enter", refocus, this);
    g_signal_connect_swapped(focus, "leave", refocus, this);
    gtk_widget_add_controller(w_, focus);

    // Tooltips.
    g_signal_connect(w_, "query-tooltip", CB([](GtkWidget *, int x, int y, gboolean kb, GtkTooltip *t, gpointer p) -> gboolean {
        auto *v = static_cast<ItemView *>(p);
        if (kb || !v->sel_ || !v->onTooltip || v->dragStarted_ || v->rubberActive_) return FALSE;
        int pos = v->itemAt(x, y);
        if (pos < 0) return FALSE;
        GdkRectangle r;
        if (v->itemRect(pos, r)) gtk_tooltip_set_tip_area(t, &r);
        return v->onTooltip(pos, t);
    }), this);

    // Drag source.
    GtkDragSource *src = gtk_drag_source_new();
    gtk_drag_source_set_actions(src, GdkDragAction(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
    g_signal_connect(src, "prepare", CB([](GtkDragSource *, double x, double y, gpointer p) -> GdkContentProvider * {
        auto *v = static_cast<ItemView *>(p);
        if (!v->sel_ || v->renameEntry_ || !v->dragContent) return nullptr;
        int pos = v->itemAt(x, y);
        if (pos < 0 || !v->contentHit(pos, x, y) || v->pressOnToggle_) return nullptr;
        if (!gtk_selection_model_is_selected(v->sel_, pos)) gtk_selection_model_select_item(v->sel_, pos, TRUE);
        return v->dragContent(v->selectedPositions());
    }), this);
    g_signal_connect(src, "drag-begin", CB([](GtkDragSource *s, GdkDrag *, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        v->dragStarted_ = true;
        v->deferSelect_ = false;
        auto sel = v->selectedPositions();
        if (sel.empty()) return;
        BelugaItem *it = item_at(G_LIST_MODEL(v->sel_), sel[0]);
        ItemData &d = item_data(it);
        GdkPaintable *paint = nullptr;
        if (d.thumb) paint = GDK_PAINTABLE(g_object_ref(d.thumb.get()));
        else {
            GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(v->w_));
            paint = GDK_PAINTABLE(gtk_icon_theme_lookup_by_gicon(theme, d.icon.get(), std::min(v->iconSize_, 64),
                                                                 gtk_widget_get_scale_factor(v->w_), GTK_TEXT_DIR_NONE, GtkIconLookupFlags(0)));
        }
        if (paint) {
            gtk_drag_source_set_icon(s, paint, 0, 0);
            g_object_unref(paint);
        }
    }), this);
    g_signal_connect(src, "drag-end", CB([](GtkDragSource *, GdkDrag *, gboolean, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        v->dragStarted_ = false;
        if (v->onDragEnd) v->onDragEnd();
    }), this);
    gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(src));

    // Drop target.
    GtkDropTarget *dt = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GdkDragAction(GDK_ACTION_COPY | GDK_ACTION_MOVE | GDK_ACTION_LINK));
    auto motionCb = CB([](GtkDropTarget *t, double x, double y, gpointer p) -> GdkDragAction {
        auto *v = static_cast<ItemView *>(p);
        int pos = v->itemAt(x, y);
        if (pos >= 0 && !(v->canDropOn && v->canDropOn(pos))) pos = -1;
        v->setDropTarget(pos);
        set_autoscroll(v, v->autoscrollDx_, v->autoscrollDy_, v->autoscrollSource_, x, y,
                       gtk_widget_get_width(v->w_), gtk_widget_get_height(v->w_), v->headerHeight());
        if (!v->autoscrollDx_ && !v->autoscrollDy_) v->stopAutoscroll();
        if (settings().autoExpandFolders && pos >= 0 && pos != v->springPos_) {
            if (v->springSource_) g_source_remove(v->springSource_);
            v->springPos_ = pos;
            v->springSource_ = g_timeout_add(800, [](gpointer p2) -> gboolean {
                auto *v2 = static_cast<ItemView *>(p2);
                v2->springSource_ = 0;
                if (v2->springPos_ >= 0 && v2->onSpringLoad) v2->onSpringLoad(v2->springPos_);
                v2->springPos_ = -1;
                return G_SOURCE_REMOVE;
            }, v);
        } else if (pos < 0 && v->springSource_) {
            g_source_remove(v->springSource_);
            v->springSource_ = 0;
            v->springPos_ = -1;
        }
        GdkModifierType mods = current_mods(GTK_EVENT_CONTROLLER(t));
        if ((mods & GDK_CONTROL_MASK) && (mods & GDK_SHIFT_MASK)) return GDK_ACTION_LINK;
        if (mods & GDK_SHIFT_MASK) return GDK_ACTION_MOVE;
        return GDK_ACTION_COPY;
    });
    g_signal_connect(dt, "enter", motionCb, this);
    g_signal_connect(dt, "motion", motionCb, this);
    g_signal_connect(dt, "leave", CB([](GtkDropTarget *, gpointer p) {
        auto *v = static_cast<ItemView *>(p);
        v->setDropTarget(-1);
        v->stopAutoscroll();
        if (v->springSource_) g_source_remove(v->springSource_);
        v->springSource_ = 0;
        v->springPos_ = -1;
    }), this);
    g_signal_connect(dt, "drop", CB([](GtkDropTarget *t, const GValue *value, double x, double y, gpointer p) -> gboolean {
        auto *v = static_cast<ItemView *>(p);
        v->stopAutoscroll();
        if (v->springSource_) g_source_remove(v->springSource_);
        v->springSource_ = 0;
        v->springPos_ = -1;
        int pos = v->itemAt(x, y);
        if (pos >= 0 && !(v->canDropOn && v->canDropOn(pos))) pos = -1;
        v->setDropTarget(-1);
        if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
        auto *files = static_cast<GdkFileList *>(g_value_get_boxed(value));
        GdkModifierType mods = current_mods(GTK_EVENT_CONTROLLER(t));
        if (v->onDrop) v->onDrop(pos, files, mods, x, y);
        return TRUE;
    }), this);
    gtk_widget_add_controller(w_, GTK_EVENT_CONTROLLER(dt));
}

} // namespace beluga
