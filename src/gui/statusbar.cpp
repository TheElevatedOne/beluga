// SPDX-License-Identifier: GPL-2.0-or-later
#include "statusbar.hpp"

#include "gtkutil.hpp"
#include "settings.hpp"

namespace beluga {

StatusBar::StatusBar()
{
    bar_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_object_ref_sink(bar_);
    gtk_widget_add_css_class(bar_, "beluga-statusbar");
    label_ = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(label_), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label_, TRUE);
    gtk_box_append(GTK_BOX(bar_), label_);

    zoom_ = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, kMinZoom, kMaxZoom, 1);
    gtk_scale_set_draw_value(GTK_SCALE(zoom_), FALSE);
    gtk_widget_set_size_request(zoom_, 140, -1);
    gtk_widget_set_tooltip_text(zoom_, _("Zoom"));
    g_signal_connect(zoom_, "value-changed", CB([](GtkRange *r, gpointer p) {
        auto *s = static_cast<StatusBar *>(p);
        if (!s->settingZoom_ && s->onZoom) s->onZoom((int)gtk_range_get_value(r));
    }), this);
    gtk_box_append(GTK_BOX(bar_), zoom_);

    spaceBox_ = gtk_overlay_new();
    spaceBar_ = gtk_level_bar_new_for_interval(0, 1);
    gtk_level_bar_set_mode(GTK_LEVEL_BAR(spaceBar_), GTK_LEVEL_BAR_MODE_CONTINUOUS);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(spaceBar_), GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(spaceBar_), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(spaceBar_), GTK_LEVEL_BAR_OFFSET_FULL);
    gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(spaceBar_), "space-low", 0.9);
    gtk_widget_set_size_request(spaceBar_, 160, -1);
    gtk_widget_set_valign(spaceBar_, GTK_ALIGN_CENTER);
    gtk_overlay_set_child(GTK_OVERLAY(spaceBox_), spaceBar_);
    spaceLabel_ = gtk_label_new(nullptr);
    gtk_widget_add_css_class(spaceLabel_, "space-label");
    gtk_overlay_add_overlay(GTK_OVERLAY(spaceBox_), spaceLabel_);
    gtk_box_append(GTK_BOX(bar_), spaceBox_);

    small_ = gtk_label_new(nullptr);
    g_object_ref_sink(small_);
    gtk_widget_add_css_class(small_, "beluga-statusbar-small");
    gtk_label_set_ellipsize(GTK_LABEL(small_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(small_), 0);
    gtk_widget_set_halign(small_, GTK_ALIGN_START);
    gtk_widget_set_valign(small_, GTK_ALIGN_END);
    gtk_widget_set_can_target(small_, FALSE);
    gtk_widget_set_visible(small_, FALSE);

    timer_ = g_timeout_add_seconds(15, [](gpointer p) -> gboolean {
        static_cast<StatusBar *>(p)->updateSpace();
        return G_SOURCE_CONTINUE;
    }, this);
    applySettings();
}

StatusBar::~StatusBar()
{
    if (timer_) g_source_remove(timer_);
    if (cancel_) {
        g_cancellable_cancel(cancel_);
        g_object_unref(cancel_);
    }
    g_object_unref(bar_);
    g_object_unref(small_);
}

void StatusBar::applySettings()
{
    const Settings &s = settings();
    gtk_widget_set_visible(zoom_, s.showZoomSlider);
    gtk_widget_set_visible(spaceBox_, s.showSpaceInfo && !uri_.empty() && !is_virtual_uri(uri_));
}

void StatusBar::setText(const std::string &text)
{
    gtk_label_set_text(GTK_LABEL(label_), text.c_str());
    gtk_label_set_text(GTK_LABEL(small_), text.c_str());
    gtk_widget_set_visible(small_, !text.empty());
}

void StatusBar::setZoom(int level)
{
    settingZoom_ = true;
    gtk_range_set_value(GTK_RANGE(zoom_), level);
    settingZoom_ = false;
}

void StatusBar::setLocation(const std::string &uri)
{
    uri_ = uri;
    applySettings();
    updateSpace();
}

void StatusBar::updateSpace()
{
    if (uri_.empty() || is_virtual_uri(uri_) || !settings().showSpaceInfo || !gtk_widget_get_mapped(bar_)) return;
    if (cancel_) {
        g_cancellable_cancel(cancel_);
        g_object_unref(cancel_);
    }
    cancel_ = g_cancellable_new();
    Ref<GFile> f(g_file_new_for_uri(uri_.c_str()));
    g_file_query_filesystem_info_async(f.get(), G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                       G_PRIORITY_LOW, cancel_, [](GObject *src, GAsyncResult *res, gpointer p) {
        GError *err = nullptr;
        GFileInfo *info = g_file_query_filesystem_info_finish(G_FILE(src), res, &err);
        if (!info) {
            bool cancelled = g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED);
            g_error_free(err);
            if (!cancelled) gtk_widget_set_visible(static_cast<StatusBar *>(p)->spaceBox_, FALSE);
            return;
        }
        auto *s = static_cast<StatusBar *>(p);
        guint64 total = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        guint64 free = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        g_object_unref(info);
        if (total == 0) {
            gtk_widget_set_visible(s->spaceBox_, FALSE);
            return;
        }
        gtk_level_bar_set_value(GTK_LEVEL_BAR(s->spaceBar_), 1.0 - (double)free / total);
        gtk_label_set_text(GTK_LABEL(s->spaceLabel_), printf_str(_("%s free"), format_size(free).c_str()).c_str());
        gtk_widget_set_tooltip_text(s->spaceBox_, printf_str(_("%s free of %s (%d%% used)"), format_size(free).c_str(),
                                                             format_size(total).c_str(), (int)(100.0 * (total - free) / total)).c_str());
        gtk_widget_set_visible(s->spaceBox_, settings().showSpaceInfo);
    }, this);
}

} // namespace beluga
