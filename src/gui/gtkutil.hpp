// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: helpers for GTK code.
#pragma once

#include <gtk/gtk.h>

namespace beluga {

// G_CALLBACK for captureless lambdas (a macro would split the lambda at its commas).
template <typename F>
inline GCallback CB(F f)
{
    return reinterpret_cast<GCallback>(+f);
}

// A texture from a pixbuf (gdk_texture_new_for_pixbuf is deprecated).
inline GdkTexture *texture_from_pixbuf(GdkPixbuf *p)
{
    GBytes *bytes = gdk_pixbuf_read_pixel_bytes(p);
    GdkTexture *t = gdk_memory_texture_new(gdk_pixbuf_get_width(p), gdk_pixbuf_get_height(p),
                                           gdk_pixbuf_get_has_alpha(p) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                                           bytes, gdk_pixbuf_get_rowstride(p));
    g_bytes_unref(bytes);
    return t;
}

// Removes the rows of a list box. gtk_list_box_remove_all() loops forever when the box also holds
// a popover (a context menu parented to it), because it can't remove that child.
inline void list_box_remove_rows(GtkListBox *box)
{
    // Removing a row also removes its header, which may be the next sibling, so start over each time.
    for (;;) {
        GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(box));
        while (c && !GTK_IS_LIST_BOX_ROW(c)) c = gtk_widget_get_next_sibling(c);
        if (!c) break;
        gtk_list_box_remove(box, c);
    }
}

} // namespace beluga
