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

} // namespace beluga
