// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the file chooser shown for other programs (through the xdg-desktop-portal backend in portal.cpp),
// laid out like KDE's dialog: Places on the left, the folder in a Beluga view, then name, file type, the
// program's extra options and the buttons.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace beluga {

class View;

struct ChooserRequest {
    enum class Mode { Open, Save, SaveFiles };
    Mode mode = Mode::Open;
    std::string title, acceptLabel;
    bool multiple = false;     // Open: several files
    bool directory = false;    // Open: folders instead of files
    struct Filter {
        std::string name;
        std::vector<std::pair<guint32, std::string>> rules;   // 0: glob pattern, 1: MIME type
    };
    std::vector<Filter> filters;
    int currentFilter = -1;
    struct Choice {
        std::string id, label;
        std::vector<std::pair<std::string, std::string>> options;   // empty: a check box ("true"/"false")
        std::string selected;
    };
    std::vector<Choice> choices;
    std::string currentName;     // Save: the suggested name
    std::string currentFolder;   // a local path to start in
    std::string currentFile;     // Save: the file being saved again
    std::vector<std::string> files;   // SaveFiles: the names to save into the chosen folder
    std::string parentWindow;    // "wayland:<exported handle>" or "x11:<id>"
};

struct ChooserResult {
    bool accepted = false;
    std::vector<std::string> uris;
    std::vector<std::pair<std::string, std::string>> choices;
    int filter = -1;
};

// Shows the chooser; done is called once, when it's accepted or cancelled.
GtkWidget *show_file_chooser(GtkApplication *app, const ChooserRequest &req, std::function<void(const ChooserResult &)> done);
// Cancels and closes it (the portal's Request.Close).
void file_chooser_close(GtkWidget *chooser);

// For the self-test.
View *file_chooser_view(GtkWidget *chooser);
void file_chooser_set_name(GtkWidget *chooser, const std::string &name);
void file_chooser_navigate(GtkWidget *chooser, const std::string &uri);
void file_chooser_set_filter(GtkWidget *chooser, int index);
void file_chooser_accept(GtkWidget *chooser);

} // namespace beluga
