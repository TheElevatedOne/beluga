// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: small dialogs. Questions, errors, name prompts, renaming, the drop
// menu, file conflicts, place editing and the view display style dialog.
#pragma once

#include "fileops.hpp"
#include "item.hpp"
#include "places.hpp"
#include "viewprops.hpp"

#include <functional>
#include <string>
#include <vector>

namespace beluga {

GtkWindow *window_of(GtkWidget *w);

// Buttons left to right; callback gets the index or -1 when dismissed.
void ask(GtkWidget *parent, const std::string &message, const std::string &detail,
         const std::vector<std::string> &buttons, int defaultButton, int cancelButton, std::function<void(int)> cb);
void show_error(GtkWidget *parent, const std::string &message, const std::string &detail = {});
// Prompts for a name. selectEnd: characters selected initially (-1 all).
void ask_name(GtkWidget *parent, const std::string &title, const std::string &label, const std::string &initial,
              int selectEnd, const std::string &okLabel, std::function<void(const std::string &)> cb);
// Renames one item (name prompt) or several (pattern with # for numbers).
void show_rename_dialog(GtkWidget *parent, const std::vector<BelugaItem *> &items);
// Move / Copy / Link menu at a point. Callback gets 0 when cancelled.
void show_drop_menu(GtkWidget *widget, double x, double y, bool canMove, std::function<void(GdkDragAction)> cb);
void edit_place_dialog(GtkWidget *parent, const Place &initial, bool isNew, std::function<void(const Place &)> cb);
void view_properties_dialog(GtkWidget *parent, const std::string &uri, const ViewProps &current,
                            std::function<void(const ViewProps &, int scope)> cb);   // scope 0 folder, 1 subfolders, 2 all
// Pops up a GMenuModel at a point of a widget.
void popup_menu(GtkWidget *widget, GMenuModel *model, double x, double y);
// Makes a popover menu as tall as its rows (up to most of the screen) so it doesn't scroll when it fits.
// popup_menu() does this itself; call it once for other menus (a menu button's popover).
void fit_menu(GtkPopover *pop);
// Picks an icon: one of the icon theme's (searchable) or an image file. done gets an icon name or a path, or "" for
// the default icon; it isn't called when cancelled.
void choose_icon(GtkWidget *parent, const std::string &current, std::function<void(const std::string &)> done);
// A menu entry drawn with its icon (GTK shows icons only on entries without a label). Put it in a menu with
// menu_icon_item(); the next popup_menu() turns it into a row with the icon and the label.
void menu_icon_item(GMenu *menu, const std::string &label, const char *action, GVariant *target, GIcon *icon);

// The GUI side of file operations: conflict and error questions.
class GtkJobUi : public JobUi {
public:
    void conflict(FileJob &job, const ConflictRequest &req, std::function<void(ConflictReply)> reply) override;
    void error(FileJob &job, const std::string &message, bool canSkip, std::function<void(ErrorChoice)> reply) override;
};

} // namespace beluga
