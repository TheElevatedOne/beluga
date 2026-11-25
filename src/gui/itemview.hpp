// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the item view used for the Icons, Compact and Details modes. A
// virtualised GtkScrollable widget: only visible items get (recycled) child widgets,
// so large folders stay fast. Supports group headers, rubber band selection,
// selection toggles, drag and drop, type-ahead and inline renaming.
#pragma once

#include "item.hpp"

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace beluga {

class ItemView;
GtkWidget *item_view_new();
ItemView *item_view_get(GtkWidget *w);

struct Column {
    std::string role;
    int width;
};

class ItemView {
public:
    explicit ItemView(GtkWidget *w);
    ~ItemView();
    GtkWidget *widget() const { return w_; }

    void setModel(GtkSelectionModel *model);
    GtkSelectionModel *model() const { return sel_; }
    void setMode(int mode);
    int mode() const { return mode_; }
    void setIconSize(int px);
    int iconSize() const { return iconSize_; }
    void setRoles(const std::vector<std::string> &roles);
    void setColumnWidths(const std::map<std::string, int> &w);
    std::map<std::string, int> columnWidths() const;
    void setSortIndicator(const std::string &role, bool descending);
    void setPreviews(bool on);
    void setGrouped(bool on);
    void setTreeMode(bool on) { tree_ = on; relayout(); }
    void setHeaderVisible(bool on);
    void setSelectionMode(bool on);
    void setFont(const std::string &pangoFont);
    void relayout();
    void refreshAll();       // rebind every visible item

    int count() const;
    int itemAt(double x, double y) const;     // widget coordinates
    bool contentHit(int pos, double x, double y) const;
    void scrollTo(int pos);
    void scrollToTop();
    int currentPosition();
    BelugaItem *currentItem() const { return current_; }
    void setCurrent(int pos, bool select, bool scroll = true);
    std::vector<int> selectedPositions() const;
    void selectAll();
    void clearSelection();
    void invertSelection();
    void selectItems(const std::vector<BelugaItem *> &items, bool scroll);
    void startRename(int pos, int selStart, int selEnd, std::function<void(const std::string &)> done);
    bool renaming() const { return renameEntry_ != nullptr; }
    // Rectangle of an item in widget coordinates (for popovers).
    bool itemRect(int pos, GdkRectangle &r) const;
    double scrollPosition() const;
    void setScrollPosition(double v);

    // Callbacks set by DolphinView.
    std::function<void(int pos, GdkModifierType mods)> onActivate;
    std::function<void(int pos)> onMiddleClick;
    std::function<void(int pos, double x, double y)> onContextMenu;   // pos -1: empty area
    std::function<void(int pos)> onHover;
    std::function<void(const std::string &role)> onHeaderClicked;
    std::function<void(double x, double y)> onHeaderMenu;
    std::function<void()> onColumnsResized;
    std::function<void(int steps)> onZoom;
    std::function<void(int button)> onExtraButton;                   // 8 back, 9 forward
    std::function<void()> onEmptyDoubleClick;
    std::function<void()> onEscape;
    std::function<void(BelugaItem *)> onNeedPreview;
    std::function<void(BelugaItem *)> onNeedDirSize;
    std::function<void(BelugaItem *)> onNeedUsage;   // a folder's disk usage, for the "diskusage" column
    // What the Disk Usage column's bars are relative to: the space the shown items take up together.
    void setUsageTotal(guint64 total);
    std::function<std::string(int pos)> groupLabel;
    std::function<bool(int pos, GtkTooltip *t)> onTooltip;
    // Drop of files: pos is the target item (-1: the view's folder).
    std::function<void(int pos, GdkFileList *files, GdkModifierType mods, double x, double y)> onDrop;
    std::function<bool(int pos)> canDropOn;
    std::function<void(int pos)> onSpringLoad;
    std::function<GdkContentProvider *(const std::vector<int> &positions)> dragContent;
    std::function<void()> onDragEnd;

    // GObject plumbing.
    void measure(GtkOrientation o, int forSize, int *min, int *nat);
    void allocate(int width, int height);
    void snapshot(GtkSnapshot *s);
    void dispose();
    void setAdjustment(bool horizontal, GtkAdjustment *adj);
    void autoscrollStep();
    GtkAdjustment *hadj_ = nullptr, *vadj_ = nullptr;
    int hpolicy_ = 0, vpolicy_ = 0;
    int allocCount = 0;   // diagnostics for the self-test

private:
    struct Cell {
        GtkWidget *root = nullptr;
        GtkWidget *highlight = nullptr;   // gets :selected
        GtkWidget *icon = nullptr;
        GtkWidget *emblem = nullptr;
        GtkWidget *toggle = nullptr;
        GtkWidget *expander = nullptr;
        GtkWidget *indent = nullptr;
        GtkWidget *name = nullptr;
        std::vector<GtkWidget *> extras;
        std::vector<GtkWidget *> bars;    // the Disk Usage column's bar, or nullptr (same index as extras)
        int mode = -1;
        size_t columns = 0;
        BelugaItem *item = nullptr;
        gpointer rowObj = nullptr;
        gulong handler = 0;
        int pos = -1;
        bool used = false;
    };
    struct Line {
        double off, ext;
        int first, count, section;
        bool header;
    };
    struct Section {
        int start, end;
        std::string label;
        double off = 0, ext = 0;      // compact: span of the group
    };
    struct GroupHeader {
        GtkWidget *root, *label;
        int section = -1;
        bool used = false;
    };

    void computeLayout(int width, int height);
    void computeSections();
    double lineHeight() const;
    int itemWidthIcons() const;
    int compactColumnWidth(int first, int count);
    int lineOf(int pos) const;
    bool contentRect(int pos, double &x, double &y, double &w, double &h) const;
    void toWidget(double &x, double &y) const;
    void toContent(double &x, double &y) const;
    bool vertical() const { return mode_ != 1; }
    double headerHeight() const;
    Cell *takeCell();
    void buildCell(Cell *c);
    void bindCell(Cell *c, int pos, gpointer obj, BelugaItem *item);
    void unbindCell(Cell *c);
    void updateCellState(Cell *c);
    void rebuildHeader();
    void updateHeaderLabels();
    void ensureHeaderCells();
    int neighbor(int pos, int dx, int dy);
    void moveCurrent(int pos, GdkModifierType mods);
    void selectRange(int a, int b, bool unselectRest);
    void updateRubberband();
    void stopAutoscroll();
    bool typeahead(gunichar c);
    void setDropTarget(int pos);
    void finishRename(bool commit);
    int columnBoundaryAt(double x) const;

    static void onItemsChanged(GListModel *, guint, guint, guint, gpointer self);
    static void onSelectionChanged(GtkSelectionModel *, guint, guint, gpointer self);
    static void onItemSignal(BelugaItem *, gpointer self);
    void setupControllers();

    GtkWidget *w_;
    GtkSelectionModel *sel_ = nullptr;
    int mode_ = 0;
    int iconSize_ = 48;
    bool previews_ = true;
    bool grouped_ = false;
    bool tree_ = false;
    bool selectionMode_ = false;
    bool headerVisible_ = true;
    std::vector<std::string> roles_;
    std::vector<Column> columns_;            // details, first is "text"
    std::map<std::string, int> savedWidths_;
    std::string sortRole_ = "text";
    bool sortDesc_ = false;
    GtkCssProvider *fontCss_ = nullptr;

    std::vector<Line> lines_;
    std::vector<Section> sections_;
    bool layoutDirty_ = true;
    bool sectionsDirty_ = true;
    int lastW_ = -1, lastH_ = -1;
    double contentW_ = 0, contentH_ = 0;
    double itemW_ = 0, itemH_ = 0, gapX_ = 0, rowH_ = 0, lineH_ = 16;
    int cols_ = 1, rows_ = 1;
    double margin_ = 4;

    std::vector<Cell *> cells_;
    std::unordered_map<BelugaItem *, Cell *> bound_;
    guint64 usageTotal_ = 0;
    std::vector<GroupHeader> groupHeaders_;
    GtkWidget *header_ = nullptr;            // details column header
    std::vector<GtkWidget *> headerCells_;
    GtkWidget *rubber_ = nullptr;
    GtkWidget *renameEntry_ = nullptr;
    BelugaItem *renameItem_ = nullptr;
    std::function<void(const std::string &)> renameDone_;

    BelugaItem *current_ = nullptr;             // keyboard focus item
    BelugaItem *anchor_ = nullptr;              // shift-selection anchor
    BelugaItem *hover_ = nullptr;
    int currentPos_ = -1;
    int dropPos_ = -1;

    // Press / drag state.
    int pressPos_ = -1;
    bool pressOnToggle_ = false;
    bool deferSelect_ = false;
    bool dragStarted_ = false;
    bool rubberActive_ = false;
    double rbX0_ = 0, rbY0_ = 0, rbX1_ = 0, rbY1_ = 0;   // content coordinates
    GtkBitset *rbBase_ = nullptr;
    bool rbToggle_ = false;
    int resizeColumn_ = -1;
    double resizeStartW_ = 0;
    guint autoscrollSource_ = 0;
    double autoscrollDx_ = 0, autoscrollDy_ = 0;
    guint springSource_ = 0;
    int springPos_ = -1;
    std::string typeahead_;
    gint64 typeaheadTime_ = 0;
    double lastPointerX_ = -1, lastPointerY_ = -1;
    bool disposed_ = false;
};

} // namespace beluga
