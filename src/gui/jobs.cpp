// SPDX-License-Identifier: GPL-2.0-or-later
#include "jobs.hpp"

#include "fileops.hpp"
#include "gtkutil.hpp"

#include <map>

namespace beluga {

namespace {

struct Row {
    GtkWidget *box, *title, *current, *bar, *stats;
};

struct JobsUi {
    GtkWidget *button = nullptr;
    GtkWidget *bar = nullptr;
    GtkWidget *list = nullptr;
    guint timer = 0;
    int listener = 0;
    std::map<int, Row> rows;
};

void refresh(JobsUi *ui)
{
    auto &jobs = JobManager::instance().jobs();
    double total = 0;
    for (auto &j : jobs) {
        total += j->fraction();
        auto it = ui->rows.find(j->id());
        if (it == ui->rows.end()) continue;
        Row &r = it->second;
        gtk_label_set_text(GTK_LABEL(r.title), j->title().c_str());
        gtk_label_set_text(GTK_LABEL(r.current), j->currentText().c_str());
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(r.bar), j->fraction());
        std::string stats;
        if (j->totalBytes() > 0) {
            stats = printf_str(_("%s of %s"), format_size(j->doneBytes()).c_str(), format_size(j->totalBytes()).c_str());
            double bps = j->bytesPerSecond();
            if (bps > 0) {
                stats += " — " + format_size((guint64)bps) + "/s";
                double left = (double)(j->totalBytes() - std::min(j->totalBytes(), j->doneBytes())) / bps;
                stats += ", " + printf_str(_("%s left"), format_duration(left).c_str());
            }
        } else if (j->totalFiles() > 0) {
            stats = printf_str(_("%d of %d"), j->doneFiles(), j->totalFiles());
        }
        gtk_label_set_text(GTK_LABEL(r.stats), stats.c_str());
    }
    if (!jobs.empty()) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui->bar), total / jobs.size());
}

void rebuild(JobsUi *ui)
{
    auto &jobs = JobManager::instance().jobs();
    gtk_widget_set_visible(ui->button, !jobs.empty());
    for (auto it = ui->rows.begin(); it != ui->rows.end();) {
        bool alive = false;
        for (auto &j : jobs)
            if (j->id() == it->first) alive = true;
        if (!alive) {
            gtk_box_remove(GTK_BOX(ui->list), it->second.box);
            it = ui->rows.erase(it);
        } else {
            ++it;
        }
    }
    for (auto &j : jobs) {
        if (ui->rows.count(j->id())) continue;
        Row r;
        r.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_add_css_class(r.box, "beluga-job");
        r.title = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(r.title), 0);
        gtk_label_set_ellipsize(GTK_LABEL(r.title), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(r.title), 50);
        gtk_widget_add_css_class(r.title, "heading");
        r.current = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(r.current), 0);
        gtk_label_set_ellipsize(GTK_LABEL(r.current), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(r.current), 50);
        gtk_widget_add_css_class(r.current, "dim-label");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        r.bar = gtk_progress_bar_new();
        gtk_widget_set_hexpand(r.bar, TRUE);
        gtk_widget_set_valign(r.bar, GTK_ALIGN_CENTER);
        GtkWidget *cancel = gtk_button_new_from_icon_name("process-stop");
        gtk_widget_add_css_class(cancel, "flat");
        gtk_widget_set_tooltip_text(cancel, _("Cancel"));
        int id = j->id();
        g_object_set_data(G_OBJECT(cancel), "beluga-job", GINT_TO_POINTER(id));
        g_signal_connect(cancel, "clicked", CB([](GtkButton *b) {
            int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "beluga-job"));
            for (auto &job : JobManager::instance().jobs())
                if (job->id() == id) job->cancel();
        }), nullptr);
        gtk_box_append(GTK_BOX(row), r.bar);
        gtk_box_append(GTK_BOX(row), cancel);
        r.stats = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(r.stats), 0);
        gtk_widget_add_css_class(r.stats, "dim-label");
        gtk_box_append(GTK_BOX(r.box), r.title);
        gtk_box_append(GTK_BOX(r.box), r.current);
        gtk_box_append(GTK_BOX(r.box), row);
        gtk_box_append(GTK_BOX(r.box), r.stats);
        gtk_box_append(GTK_BOX(ui->list), r.box);
        ui->rows[id] = r;
    }
    if (!jobs.empty() && !ui->timer)
        ui->timer = g_timeout_add(250, [](gpointer p) -> gboolean {
            auto *ui = static_cast<JobsUi *>(p);
            if (JobManager::instance().jobs().empty()) {
                ui->timer = 0;
                return G_SOURCE_REMOVE;
            }
            refresh(ui);
            return G_SOURCE_CONTINUE;
        }, ui);
    refresh(ui);
}

} // namespace

GtkWidget *jobs_indicator_new()
{
    auto *ui = new JobsUi;
    ui->button = gtk_menu_button_new();
    gtk_widget_add_css_class(ui->button, "flat");
    gtk_widget_add_css_class(ui->button, "beluga-jobs");
    gtk_widget_set_tooltip_text(ui->button, _("File operations in progress"));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    ui->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(ui->bar, 80, -1);
    gtk_widget_set_valign(ui->bar, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), spinner);
    gtk_box_append(GTK_BOX(box), ui->bar);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(ui->button), box);
    GtkWidget *pop = gtk_popover_new();
    ui->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(ui->list, 6);
    gtk_widget_set_margin_end(ui->list, 6);
    gtk_widget_set_margin_top(ui->list, 6);
    gtk_widget_set_margin_bottom(ui->list, 6);
    gtk_widget_set_size_request(ui->list, 360, -1);
    gtk_popover_set_child(GTK_POPOVER(pop), ui->list);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ui->button), pop);
    ui->listener = JobManager::instance().addListener([ui] { rebuild(ui); });
    g_object_set_data_full(G_OBJECT(ui->button), "beluga-jobs", ui, [](gpointer p) {
        auto *ui = static_cast<JobsUi *>(p);
        JobManager::instance().removeListener(ui->listener);
        if (ui->timer) g_source_remove(ui->timer);
        delete ui;
    });
    rebuild(ui);
    return ui->button;
}

} // namespace beluga
