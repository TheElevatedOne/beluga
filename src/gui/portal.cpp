// SPDX-License-Identifier: GPL-2.0-or-later
#include "portal.hpp"

#include "filechooser.hpp"
#include "settings.hpp"
#include "style.hpp"
#include "util.hpp"

#include <cstring>
#include <memory>

namespace beluga {

const char *const kPortalBusName = "org.freedesktop.impl.portal.desktop.beluga";

namespace {

const char *kObjectPath = "/org/freedesktop/portal/desktop";

const char *kChooserXml =
    "<node><interface name='org.freedesktop.impl.portal.FileChooser'>"
    "<method name='OpenFile'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "<arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "<arg type='a{sv}' name='options' direction='in'/><arg type='u' name='response' direction='out'/>"
    "<arg type='a{sv}' name='results' direction='out'/></method>"
    "<method name='SaveFile'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "<arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "<arg type='a{sv}' name='options' direction='in'/><arg type='u' name='response' direction='out'/>"
    "<arg type='a{sv}' name='results' direction='out'/></method>"
    "<method name='SaveFiles'><arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>"
    "<arg type='s' name='parent_window' direction='in'/><arg type='s' name='title' direction='in'/>"
    "<arg type='a{sv}' name='options' direction='in'/><arg type='u' name='response' direction='out'/>"
    "<arg type='a{sv}' name='results' direction='out'/></method>"
    "</interface></node>";

const char *kRequestXml = "<node><interface name='org.freedesktop.impl.portal.Request'><method name='Close'/></interface></node>";

GDBusInterfaceInfo *iface_info(const char *xml)
{
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, nullptr);
    GDBusInterfaceInfo *info = g_dbus_interface_info_ref(node->interfaces[0]);
    g_dbus_node_info_unref(node);
    return info;   // kept for the life of the process
}

// One request: its reply and the Request object at its handle.
struct Pending {
    GDBusMethodInvocation *invocation = nullptr;
    GDBusConnection *bus = nullptr;
    guint requestId = 0;
    GtkWidget *window = nullptr;
    ChooserRequest req;
};

guint g_idleQuit = 0;

std::string bytestring(GVariant *v)
{
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) return {};
    return str(g_variant_get_bytestring(v));
}

ChooserRequest::Filter parse_filter(GVariant *f)
{
    ChooserRequest::Filter out;
    const char *name = nullptr;
    GVariantIter *rules = nullptr;
    g_variant_get(f, "(&sa(us))", &name, &rules);
    out.name = str(name);
    guint32 type;
    const char *rule;
    while (g_variant_iter_next(rules, "(u&s)", &type, &rule)) out.rules.push_back({type, rule});
    g_variant_iter_free(rules);
    return out;
}

GVariant *filter_variant(const ChooserRequest::Filter &f)
{
    GVariantBuilder rules;
    g_variant_builder_init(&rules, G_VARIANT_TYPE("a(us)"));
    for (auto &[t, r] : f.rules) g_variant_builder_add(&rules, "(us)", t, r.c_str());
    return g_variant_new("(sa(us))", f.name.c_str(), &rules);
}

ChooserRequest parse_request(const std::string &method, const char *parent, const char *title, GVariant *opts)
{
    ChooserRequest req;
    req.mode = method == "SaveFile" ? ChooserRequest::Mode::Save : method == "SaveFiles" ? ChooserRequest::Mode::SaveFiles : ChooserRequest::Mode::Open;
    req.title = str(title);
    req.parentWindow = str(parent);
    GVariantDict d;
    g_variant_dict_init(&d, opts);
    const char *s = nullptr;
    gboolean b = FALSE;
    if (g_variant_dict_lookup(&d, "accept_label", "&s", &s)) req.acceptLabel = s;
    if (g_variant_dict_lookup(&d, "multiple", "b", &b)) req.multiple = b;
    if (g_variant_dict_lookup(&d, "directory", "b", &b)) req.directory = b;
    if (g_variant_dict_lookup(&d, "current_name", "&s", &s)) req.currentName = s;
    if (GVariant *v = g_variant_dict_lookup_value(&d, "current_folder", G_VARIANT_TYPE_BYTESTRING)) {
        req.currentFolder = bytestring(v);
        g_variant_unref(v);
    }
    if (GVariant *v = g_variant_dict_lookup_value(&d, "current_file", G_VARIANT_TYPE_BYTESTRING)) {
        req.currentFile = bytestring(v);
        g_variant_unref(v);
    }
    if (GVariant *v = g_variant_dict_lookup_value(&d, "files", G_VARIANT_TYPE("aay"))) {
        GVariantIter it;
        g_variant_iter_init(&it, v);
        while (GVariant *f = g_variant_iter_next_value(&it)) {
            req.files.push_back(basename_of(bytestring(f)));
            g_variant_unref(f);
        }
        g_variant_unref(v);
    }
    if (GVariant *v = g_variant_dict_lookup_value(&d, "filters", G_VARIANT_TYPE("a(sa(us))"))) {
        GVariantIter it;
        g_variant_iter_init(&it, v);
        while (GVariant *f = g_variant_iter_next_value(&it)) {
            req.filters.push_back(parse_filter(f));
            g_variant_unref(f);
        }
        g_variant_unref(v);
    }
    if (GVariant *v = g_variant_dict_lookup_value(&d, "current_filter", G_VARIANT_TYPE("(sa(us))"))) {
        ChooserRequest::Filter cur = parse_filter(v);
        g_variant_unref(v);
        for (size_t i = 0; i < req.filters.size(); ++i)
            if (req.filters[i].name == cur.name && req.filters[i].rules == cur.rules) req.currentFilter = (int)i;
        if (req.currentFilter < 0 && req.filters.empty()) {
            req.filters.push_back(cur);
            req.currentFilter = 0;
        }
    }
    if (GVariant *v = g_variant_dict_lookup_value(&d, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"))) {
        GVariantIter it;
        g_variant_iter_init(&it, v);
        const char *id, *label, *selected;
        GVariantIter *options;
        while (g_variant_iter_next(&it, "(&s&sa(ss)&s)", &id, &label, &options, &selected)) {
            ChooserRequest::Choice c{id, label, {}, selected};
            const char *oid, *olabel;
            while (g_variant_iter_next(options, "(&s&s)", &oid, &olabel)) c.options.push_back({oid, olabel});
            g_variant_iter_free(options);
            req.choices.push_back(c);
        }
        g_variant_unref(v);
    }
    g_variant_dict_clear(&d);
    return req;
}

// Quits a while after the last chooser closed; D-Bus starts us again for the next one.
void schedule_idle_quit(GtkApplication *app)
{
    if (!app || !g_object_get_data(G_OBJECT(app), "beluga-portal")) return;
    if (g_idleQuit) g_source_remove(g_idleQuit);
    g_idleQuit = g_timeout_add_seconds(300, [](gpointer a) -> gboolean {
        g_idleQuit = 0;
        if (!gtk_application_get_windows(GTK_APPLICATION(a))) g_application_quit(G_APPLICATION(a));
        return G_SOURCE_REMOVE;
    }, app);
}

void request_call(GDBusConnection *, const char *, const char *, const char *, const char *method, GVariant *,
                  GDBusMethodInvocation *inv, gpointer data)
{
    auto *p = static_cast<Pending *>(data);
    if (!strcmp(method, "Close")) {
        GtkWidget *w = p->window;
        g_dbus_method_invocation_return_value(inv, nullptr);
        // After this handler returns: closing replies to the request and removes this object.
        g_idle_add([](gpointer w) -> gboolean {
            file_chooser_close(GTK_WIDGET(w));
            g_object_unref(w);
            return G_SOURCE_REMOVE;
        }, g_object_ref(w));
    }
}

void chooser_call(GDBusConnection *bus, const char *, const char *, const char *, const char *method, GVariant *params,
                  GDBusMethodInvocation *inv, gpointer data)
{
    auto *app = static_cast<GtkApplication *>(data);
    const char *handle, *appId, *parent, *title;
    GVariant *opts = nullptr;
    g_variant_get(params, "(&o&s&s&s@a{sv})", &handle, &appId, &parent, &title, &opts);
    auto p = std::make_shared<Pending>();
    p->invocation = inv;
    p->bus = bus;
    p->req = parse_request(method, parent, title, opts);
    g_variant_unref(opts);
    if (g_idleQuit) {
        g_source_remove(g_idleQuit);
        g_idleQuit = 0;
    }
    static GDBusInterfaceInfo *requestInfo = iface_info(kRequestXml);
    static const GDBusInterfaceVTable requestVtable = {request_call, nullptr, nullptr, {nullptr}};
    p->requestId = g_dbus_connection_register_object(bus, handle, requestInfo, &requestVtable, p.get(), nullptr, nullptr);
    p->window = show_file_chooser(app, p->req, [p, app](const ChooserResult &r) {
        GVariantBuilder results;
        g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
        if (r.accepted) {
            GVariantBuilder uris;
            g_variant_builder_init(&uris, G_VARIANT_TYPE_STRING_ARRAY);
            for (auto &u : r.uris) g_variant_builder_add(&uris, "s", u.c_str());
            g_variant_builder_add(&results, "{sv}", "uris", g_variant_builder_end(&uris));
            if (!r.choices.empty()) {
                GVariantBuilder ch;
                g_variant_builder_init(&ch, G_VARIANT_TYPE("a(ss)"));
                for (auto &[id, val] : r.choices) g_variant_builder_add(&ch, "(ss)", id.c_str(), val.c_str());
                g_variant_builder_add(&results, "{sv}", "choices", g_variant_builder_end(&ch));
            }
            if (r.filter >= 0 && r.filter < (int)p->req.filters.size())
                g_variant_builder_add(&results, "{sv}", "current_filter", filter_variant(p->req.filters[r.filter]));
        }
        // 0: done, 1: cancelled.
        g_dbus_method_invocation_return_value(p->invocation, g_variant_new("(ua{sv})", r.accepted ? 0u : 1u, &results));
        p->invocation = nullptr;
        if (p->requestId) g_dbus_connection_unregister_object(p->bus, p->requestId);
        p->requestId = 0;
        schedule_idle_quit(app);
    });
}

} // namespace

guint portal_register(GDBusConnection *bus, GtkApplication *app, GError **error)
{
    static GDBusInterfaceInfo *info = iface_info(kChooserXml);
    static const GDBusInterfaceVTable vtable = {chooser_call, nullptr, nullptr, {nullptr}};
    return g_dbus_connection_register_object(bus, kObjectPath, info, &vtable, app, nullptr, error);
}

int run_portal(int, char **argv)
{
    // Inside a portal, never ask a portal for anything: that would wait on ourselves.
    const char *dbg = g_getenv("GDK_DEBUG");
    g_setenv("GDK_DEBUG", (dbg && *dbg ? std::string(dbg) + ",no-portals" : std::string("no-portals")).c_str(), TRUE);
    g_setenv("GTK_USE_PORTAL", "0", TRUE);
    settings().load();
    GtkApplication *app = gtk_application_new("org.beluga.Beluga.FileChooser", G_APPLICATION_NON_UNIQUE);
    g_object_set_data(G_OBJECT(app), "beluga-portal", GINT_TO_POINTER(1));
    g_application_set_resource_base_path(G_APPLICATION(app), "/org/beluga/Beluga");   // the icon
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *) {}), nullptr);
    g_signal_connect(app, "startup", G_CALLBACK(+[](GtkApplication *a) {
        load_style();
        gtk_window_set_default_icon_name(BELUGA_APP_ID);
        g_application_hold(G_APPLICATION(a));
        // The object is exported when the bus is there, before the name is taken, so that no call arrives
        // before it exists.
        g_bus_own_name(G_BUS_TYPE_SESSION, kPortalBusName, G_BUS_NAME_OWNER_FLAGS_NONE,
                       [](GDBusConnection *bus, const char *, gpointer a) {
                           GError *err = nullptr;
                           if (!portal_register(bus, GTK_APPLICATION(a), &err)) {
                               g_printerr("beluga --portal: %s\n", err ? err->message : "could not export the file chooser");
                               g_clear_error(&err);
                               g_application_quit(G_APPLICATION(a));
                           }
                       },
                       [](GDBusConnection *, const char *, gpointer a) { schedule_idle_quit(GTK_APPLICATION(a)); },
                       [](GDBusConnection *, const char *name, gpointer a) {
                           g_printerr("beluga --portal: could not own %s (another file chooser runs?)\n", name);
                           g_application_quit(G_APPLICATION(a));
                       },
                       a, nullptr);
    }), nullptr);
    char *args[] = {argv[0], nullptr};
    int status = g_application_run(G_APPLICATION(app), 1, args);
    g_object_unref(app);
    return status;
}

} // namespace beluga
