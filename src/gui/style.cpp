// SPDX-License-Identifier: GPL-2.0-or-later
#include "style.hpp"

#include <gtk/gtk.h>

#include <string>

namespace beluga {

// Square corners everywhere, solid backgrounds, colours from the GTK theme's named
// colours (pywal / Noctalia style sheets define the same names as Adwaita).
static const char *kCss = R"css(
* { border-radius: 0; }

beluga-view {
  background-color: @beluga_view_bg;
  color: @beluga_view_fg;
}
beluga-view .beluga-item { padding: 0; }
beluga-view .beluga-item.icons { padding: 2px; }
beluga-view .beluga-item:hover,
beluga-view .beluga-item .name-cell:hover {
  background-color: mix(@beluga_view_bg, @beluga_accent_bg, 0.18);
}
beluga-view .beluga-item:selected,
beluga-view .beluga-item .name-cell:selected {
  background-color: @beluga_accent_bg;
  color: @beluga_accent_fg;
}
beluga-view .beluga-item:selected:hover,
beluga-view .beluga-item .name-cell:selected:hover {
  background-color: mix(@beluga_accent_bg, @beluga_view_fg, 0.12);
}
beluga-view .beluga-item:selected .dim-label,
beluga-view .beluga-item .name-cell:selected .dim-label { color: @beluga_accent_fg; }
beluga-view:focus-within .beluga-item.current {
  outline: 1px dotted mix(@beluga_view_fg, @beluga_view_bg, 0.4);
  outline-offset: -2px;
}
beluga-view .beluga-item:drop(active),
beluga-view .beluga-item .name-cell:drop(active) {
  background-color: mix(@beluga_view_bg, @beluga_accent_bg, 0.4);
  box-shadow: inset 0 0 0 1px @beluga_accent_bg;
}
beluga-view .beluga-item.hidden-item .item-icon { opacity: 0.55; }
/* Cut items: fainter than hidden ones, and in italics, so a cut hidden file still looks cut. */
beluga-view .beluga-item.cut .item-icon { opacity: 0.3; }
beluga-view .beluga-item.cut label { opacity: 0.6; font-style: italic; }
beluga-view .beluga-item .selection-toggle { margin: 0; padding: 0; }
beluga-view rubberband, beluga-view .rubberband {
  background-color: alpha(@beluga_accent_bg, 0.22);
  border: 1px solid @beluga_accent_bg;
}
beluga-view .group-header { padding: 6px 4px 0 4px; }
beluga-view .group-header label { font-weight: bold; }
beluga-view .beluga-header {
  background-color: @beluga_view_bg;
  border-bottom: 1px solid mix(@beluga_view_bg, @beluga_view_fg, 0.15);
}
beluga-view .beluga-header button.header-cell {
  padding: 3px 8px;
  min-height: 0;
  background: none;
  box-shadow: none;
  border: none;
  border-right: 1px solid mix(@beluga_view_bg, @beluga_view_fg, 0.12);
  font-weight: normal;
}
beluga-view entry.beluga-rename { min-height: 0; padding: 2px 4px; }
.beluga-placeholder { font-size: 1.2em; }
.beluga-column { border-right: 1px solid mix(@beluga_view_bg, @beluga_view_fg, 0.15); }
/* Menus and popovers. GTK 4 paints a popover on its "contents" node; themes that only colour "popover" (pywal
   and Noctalia style sheets do) leave the menus in the built-in grey. Rows are compact, so that long context menus
   fit on a 1080p screen instead of scrolling. */
popover > contents, popover > arrow {
  background-color: @beluga_popover_bg;
  color: @beluga_popover_fg;
}
popover.menu > contents { padding: 4px 0; }
popover.menu modelbutton {
  min-height: 20px;
  padding: 2px 12px;
}
popover.menu modelbutton:hover,
popover.menu modelbutton:focus-visible,
popover.menu modelbutton:selected {
  background-color: mix(@beluga_popover_bg, @beluga_popover_fg, 0.12);
}
popover.menu modelbutton:disabled { color: mix(@beluga_popover_bg, @beluga_popover_fg, 0.5); }
popover.menu separator { margin: 2px 0; }
popover.menu button.beluga-menu-row { min-height: 20px; padding: 2px 12px; font-weight: normal; }
popover.menu button.beluga-menu-row:hover { background-color: mix(@beluga_popover_bg, @beluga_popover_fg, 0.12); }
/* Configure Beluga, laid out like Dolphin's settings. */
.beluga-prefs-title { font-size: 1.35em; padding: 8px 14px 6px 14px; }
.beluga-prefs-buttons { padding: 6px; }
.beluga-prefs-side { background-color: @beluga_window_bg; }
.beluga-prefs-side list { background-color: transparent; }
.beluga-prefs-side list row { padding: 2px 4px; color: @beluga_window_fg; }
.beluga-prefs-side list row:hover { background-color: mix(@beluga_window_bg, @beluga_window_fg, 0.08); }
.beluga-prefs-side list row:selected { background-color: @beluga_accent_bg; color: @beluga_accent_fg; }
/* Lists in dialogs (Configure Toolbars, the Context Menu page): readable text in themes that dim list rows. */
.beluga-dialog list > row, .beluga-preferences list > row { color: @beluga_window_fg; }
.beluga-dialog list > row:selected, .beluga-preferences list > row:selected {
  background-color: @beluga_accent_bg;
  color: @beluga_accent_fg;
}
/* Vim mode: the mode line and command line at the bottom of the window. */
.beluga-vim-bar {
  padding: 1px 8px;
  background-color: @beluga_window_bg;
  border-top: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.12);
}
.beluga-vim-mode { font-weight: bold; }
.beluga-vim-mode.visual { color: @beluga_accent_bg; }
.beluga-vim-bar entry.beluga-vim-command { min-height: 0; padding: 0 4px; border: none; background: none; }
.beluga-vim-bar label.error { color: @beluga_error; }
.beluga-admin-bar {
  background-color: mix(@beluga_window_bg, @beluga_error, 0.3);
  color: @beluga_window_fg;
  padding: 6px 10px;
  font-weight: bold;
}

.beluga-toolbar {
  padding: 3px 4px;
  background-color: @beluga_window_bg;
  border-bottom: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.12);
}
.beluga-toolbar separator { margin: 4px 4px; }
.beluga-location {
  padding: 2px 4px;
  background-color: @beluga_window_bg;
  border-bottom: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.10);
}
.beluga-location button.crumb { padding: 2px 6px; min-height: 0; }
.beluga-location button.crumb.current { font-weight: bold; }
.beluga-location button.crumb-arrow,
.beluga-location menubutton.crumb-arrow > button {
  padding: 2px 0; min-width: 16px; min-height: 0;
  border: none; background: none; box-shadow: none;
}
.beluga-location menubutton.crumb-arrow > button:hover { background-color: mix(@beluga_window_bg, @beluga_window_fg, 0.08); }
.beluga-location.inactive button.crumb { color: mix(@beluga_window_fg, @beluga_window_bg, 0.45); }
.beluga-location entry { min-height: 0; }
/* In the tool bar the location bar is a field, as in Dolphin. */
.beluga-location-slot .beluga-location {
  background-color: @beluga_view_bg;
  border: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.18);
  padding: 0 2px;
}
.beluga-location-slot .beluga-location.inactive { background-color: @beluga_window_bg; }
.beluga-container.inactive beluga-view { background-color: mix(@beluga_view_bg, @beluga_window_bg, 0.35); }

.beluga-statusbar {
  padding: 2px 8px;
  background-color: @beluga_window_bg;
  border-top: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.12);
}
.beluga-statusbar menubutton.beluga-space > button { padding: 0 6px; min-height: 0; }
.beluga-statusbar-small {
  background-color: @beluga_window_bg;
  color: @beluga_window_fg;
  padding: 3px 8px;
  margin: 0 0 6px 6px;
  border: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.2);
}
.beluga-message {
  padding: 6px 10px;
  background-color: mix(@beluga_window_bg, @beluga_accent_bg, 0.22);
}
.beluga-message.error { background-color: mix(@beluga_window_bg, @beluga_error, 0.28); }
.beluga-filterbar, .beluga-searchbar, .beluga-selection-bar {
  padding: 4px 6px;
  background-color: @beluga_window_bg;
  border-top: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.12);
  border-bottom: 1px solid mix(@beluga_window_bg, @beluga_window_fg, 0.12);
}
.beluga-places, .beluga-folders, .beluga-info { background-color: @beluga_window_bg; }
.beluga-places .places-header { font-weight: bold; font-size: 0.9em; padding: 8px 8px 2px 8px; }
.beluga-places levelbar.places-usage trough { min-height: 3px; }
.beluga-places levelbar.places-usage block { min-height: 3px; }
notebook > header.top { padding: 3px 3px 0 3px; }
notebook > header > tabs > tab { padding: 3px 6px; margin: 0 3px 0 0; min-height: 0; }
/* The dividers between the panels and the views, visible in dark themes too. */
paned > separator { background-color: mix(@beluga_window_bg, @beluga_window_fg, 0.28); min-width: 1px; min-height: 1px; }
notebook tab button.tab-close { padding: 0; min-height: 16px; min-width: 16px; }
.beluga-job progressbar trough, .beluga-jobs progressbar trough { min-height: 6px; }
.beluga-jobs progressbar progress { min-height: 6px; }
.monospace { font-family: monospace; }
)css";

// A few state rules that must win over user style sheets which flatten every
// button.flat (and so hide the checked view mode) or style menu buttons as boxes.
static const char *kStateCss = R"css(
.beluga-toolbar button.flat:checked,
.beluga-toolbar menubutton > button:checked {
  background-color: @beluga_accent_bg;
  color: @beluga_accent_fg;
}
.beluga-toolbar menubutton.flat > button,
.beluga-location menubutton.crumb-arrow > button {
  background-color: transparent;
  border-color: transparent;
  box-shadow: none;
}
.beluga-toolbar menubutton.flat > button:hover,
.beluga-location menubutton.crumb-arrow > button:hover {
  background-color: mix(@beluga_window_bg, @beluga_window_fg, 0.1);
}
)css";

void load_style()
{
    // Map the colours this style sheet uses to whatever the theme provides.
    struct Alias {
        const char *name;
        const char *candidates[4];
        const char *fallback;
    };
    static const Alias aliases[] = {
        {"beluga_view_bg", {"view_bg_color", "theme_base_color", nullptr}, "#ffffff"},
        {"beluga_view_fg", {"view_fg_color", "theme_text_color", nullptr}, "#000000"},
        {"beluga_window_bg", {"window_bg_color", "theme_bg_color", nullptr}, "#f0f0f0"},
        {"beluga_window_fg", {"window_fg_color", "theme_fg_color", nullptr}, "#000000"},
        {"beluga_accent_bg", {"accent_bg_color", "theme_selected_bg_color", nullptr}, "#3584e4"},
        {"beluga_accent_fg", {"accent_fg_color", "theme_selected_fg_color", nullptr}, "#ffffff"},
        {"beluga_error", {"error_color", "error_bg_color", nullptr}, "#c01c28"},
        {"beluga_popover_bg", {"popover_bg_color", "theme_base_color", nullptr}, "#ffffff"},
        {"beluga_popover_fg", {"popover_fg_color", "theme_fg_color", nullptr}, "#000000"},
    };
    GtkWidget *probe = gtk_label_new(nullptr);
    g_object_ref_sink(probe);
    GtkStyleContext *ctx = gtk_widget_get_style_context(probe);
    std::string defs;
    for (const Alias &a : aliases) {
        const char *use = nullptr;
        for (int i = 0; a.candidates[i]; ++i) {
            GdkRGBA c;
            if (gtk_style_context_lookup_color(ctx, a.candidates[i], &c)) {
                use = a.candidates[i];
                break;
            }
        }
        defs += std::string("@define-color ") + a.name + " " + (use ? std::string("@") + use : std::string(a.fallback)) + ";\n";
    }
    g_object_unref(probe);
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, (defs + kCss).c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
    GtkCssProvider *states = gtk_css_provider_new();
    gtk_css_provider_load_from_string(states, (defs + kStateCss).c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(states), GTK_STYLE_PROVIDER_PRIORITY_USER + 1);
    g_object_unref(states);
}

} // namespace beluga
