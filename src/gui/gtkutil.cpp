// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtkutil.hpp"

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/wayland/gdkwayland.h>
#endif
#ifdef GDK_WINDOWING_X11
#include <gdk/x11/gdkx.h>
#endif

namespace beluga {

void use_window_class()
{
    static bool done = false;
    if (done) return;
    done = true;
#ifdef GDK_WINDOWING_X11
    // WM_CLASS is set when a window's surface is created; GTK uses the program name ("beluga") unless a class is
    // set here. The call is deprecated (X11 is on its way out) but still works.
    GdkDisplay *display = gdk_display_get_default();
    if (display && GDK_IS_X11_DISPLAY(display)) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gdk_x11_display_set_program_class(display, kWindowClass);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
#endif
#ifdef GDK_WINDOWING_WAYLAND
    // GTK sends the application ID as the xdg_toplevel app_id when it creates the toplevel, which happens when a
    // window is mapped. An emission hook on GtkWidget::map (a run-first signal, so the hook runs after GtkWindow's
    // map handler) replaces it before the first frame is committed, for every window, including dialogs that
    // don't belong to the application.
    g_type_class_unref(g_type_class_ref(GTK_TYPE_WINDOW));
    g_signal_add_emission_hook(g_signal_lookup("map", GTK_TYPE_WIDGET), 0,
                               [](GSignalInvocationHint *, guint, const GValue *params, gpointer) -> gboolean {
                                   GObject *o = G_OBJECT(g_value_get_object(&params[0]));
                                   if (GTK_IS_WINDOW(o)) {
                                       GdkSurface *s = gtk_native_get_surface(GTK_NATIVE(o));
                                       if (s && GDK_IS_WAYLAND_TOPLEVEL(s))
                                           gdk_wayland_toplevel_set_application_id(GDK_TOPLEVEL(s), kWindowClass);
                                   }
                                   return TRUE;   // keep the hook
                               },
                               nullptr, nullptr);
#endif
}

} // namespace beluga
