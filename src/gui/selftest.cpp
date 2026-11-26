// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: --selftest. Runs the file operation engine against a temporary
// folder and, with --screenshots DIR, renders the main window in each view mode to PNG.
// Everything runs in a throwaway profile (XDG dirs point into the temporary folder), so
// the user's settings, places and trash are never touched.
#include "app.hpp"
#include "archive.hpp"
#include "diskusage.hpp"
#include "fileindex.hpp"
#include "diskusagewindow.hpp"
#include "fileops.hpp"
#include "mainwindow.hpp"
#include "places.hpp"
#include "placespanel.hpp"
#include "preferences.hpp"
#include "search.hpp"
#include "settings.hpp"
#include "tabpage.hpp"
#include "trash.hpp"
#include "undo.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <iostream>

namespace beluga {

namespace {

int g_failures = 0;
std::string g_root;
std::string g_shots;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "PASS " : "FAIL ") << what << std::endl;
    if (!ok) g_failures++;
}

bool exists(const std::string &rel) { return g_file_test((g_root + "/files/" + rel).c_str(), G_FILE_TEST_EXISTS); }

void write(const std::string &rel, const std::string &content)
{
    std::string p = g_root + "/files/" + rel;
    g_mkdir_with_parents(dirname_of(p).c_str(), 0755);
    g_file_set_contents(p.c_str(), content.c_str(), -1, nullptr);
}

// A test archive: a folder, a folder only implied by its file, a file, a symbolic link and an entry that
// tries to escape with "..".
void make_archive(const std::string &path, const std::string &format)
{
    struct archive *a = archive_write_new();
    if (format == "zip") archive_write_set_format_zip(a);
    else if (format == "7z") archive_write_set_format_7zip(a);
    else if (format == "iso") archive_write_set_format_iso9660(a);
    else {
        archive_write_set_format_pax_restricted(a);
        archive_write_add_filter_gzip(a);
    }
    archive_write_open_filename(a, path.c_str());
    auto add = [&](const char *name, const std::string &data, unsigned type, const char *link = nullptr) {
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, name);
        archive_entry_set_filetype(e, type);
        archive_entry_set_perm(e, type == AE_IFDIR ? 0755 : 0644);
        archive_entry_set_size(e, type == AE_IFREG ? (la_int64_t)data.size() : 0);
        archive_entry_set_mtime(e, 1700000000, 0);
        if (link) archive_entry_set_symlink(e, link);
        archive_write_header(a, e);
        if (type == AE_IFREG) archive_write_data(a, data.data(), data.size());
        archive_entry_free(e);
    };
    add("docs/", "", AE_IFDIR);
    add("docs/readme.txt", "hello archive", AE_IFREG);
    add("docs/sub/deep.txt", "deep", AE_IFREG);
    add("top.bin", std::string(1000, 'z'), AE_IFREG);
    if (format != "7z") add("link", "", AE_IFLNK, "docs/readme.txt");
    if (format != "iso") add("../evil.txt", "evil", AE_IFREG);
    archive_write_close(a);
    archive_write_free(a);
}

bool has_entry(const std::vector<archive::Entry> *v, const char *name)
{
    return v && std::any_of(v->begin(), v->end(), [&](const archive::Entry &e) { return e.name == name; });
}

Ref<GFile> F(const std::string &rel) { return Ref<GFile>(g_file_new_for_path((g_root + "/files/" + rel).c_str())); }

void run_job(FileJob::Ptr job)
{
    bool done = false;
    auto prev = job->onFinished;
    job->onFinished = [&done, prev](FileJob &j) {
        if (prev) prev(j);
        done = true;
    };
    job->start();
    gint64 deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
    while (!done && g_get_monotonic_time() < deadline) g_main_context_iteration(nullptr, TRUE);
}

void wait_ms(int ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        g_usleep(10000);
    }
}

void core_tests()
{
    write("a.txt", "alpha");
    write("b.txt", "beta needle");
    write("file10.txt", "x");
    write("file2.txt", "x");
    write("Folder 1/c.txt", "gamma");
    write("Folder 2/deep/d.md", "delta");
    write(".hidden", "h");

    run_job(FileJob::copy({F("a.txt")}, F("Folder 1").get()));
    check(exists("Folder 1/a.txt") && exists("a.txt"), "copy a file into a folder");

    run_job(FileJob::copy({F("a.txt")}, F("Folder 1").get()));
    check(exists("Folder 1/a.txt") && !exists("Folder 1/a (1).txt"), "copy conflict without UI skips");

    run_job(FileJob::copy({F("a.txt")}, F("").get()));
    check(exists("a copy.txt"), "copy onto itself makes a copy");

    run_job(FileJob::move({F("b.txt")}, F("Folder 2").get()));
    check(exists("Folder 2/b.txt") && !exists("b.txt"), "move a file");

    UndoManager::instance().undo();
    wait_ms(500);
    check(exists("b.txt") && !exists("Folder 2/b.txt"), "undo move");
    UndoManager::instance().redo();
    wait_ms(500);
    check(exists("Folder 2/b.txt"), "redo move");

    run_job(FileJob::copy({F("Folder 2")}, F("Folder 1").get()));
    check(exists("Folder 1/Folder 2/deep/d.md"), "copy a folder tree");

    auto trashJob = FileJob::trash({F("Folder 1/c.txt")});
    run_job(trashJob);
    bool inTrash = false;
    for (auto &e : trash::list())
        if (ends_with(e.originalPath, "/Folder 1/c.txt")) inTrash = true;
    check(!exists("Folder 1/c.txt") && inTrash, "move to trash");
    check(!trash::is_empty(), "trash is not empty");
    run_job(FileJob::restore(trashJob->trashed()));
    check(exists("Folder 1/c.txt"), "restore from trash");

    run_job(FileJob::trash({F("file2.txt")}));
    UndoManager::instance().undo();
    wait_ms(500);
    check(exists("file2.txt"), "undo trash");

    Ref<GFile> renamed;
    rename_file(F("file10.txt").get(), "renamed.txt", renamed, nullptr);
    UndoCommand c;
    c.type = UndoCommand::Rename;
    c.pairs.emplace_back(file_uri(F("file10.txt").get()), file_uri(renamed.get()));
    UndoManager::instance().push(c);
    check(exists("renamed.txt"), "rename");
    UndoManager::instance().undo();
    check(exists("file10.txt") && !exists("renamed.txt"), "undo rename");

    run_job(FileJob::remove({F("a copy.txt")}));
    check(!exists("a copy.txt"), "delete permanently");

    // Deleting an item in the Trash view removes its .trashinfo too.
    write("doomed.txt", "x");
    auto doomed = FileJob::trash({F("doomed.txt")});
    run_job(doomed);
    std::string doomedInfo = doomed->trashed().empty() ? std::string() : doomed->trashed()[0].infoPath;
    if (!doomed->trashed().empty())
        run_job(FileJob::remove({Ref<GFile>(g_file_new_for_path(doomed->trashed()[0].trashedPath.c_str()))}));
    check(!doomedInfo.empty() && !g_file_test(doomedInfo.c_str(), G_FILE_TEST_EXISTS), "delete from the trash removes its info file");

    run_job(FileJob::emptyTrash());
    check(trash::is_empty(), "empty trash");

    // Search: names and content.
    SearchQuery q;
    q.rootUri = file_uri(F("").get());
    q.text = "c.txt";
    int found = 0;
    bool searchDone = false;
    auto job = SearchJob::start(q, [&](std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> &&r) { found += (int)r.size(); },
                                [&] { searchDone = true; });
    while (!searchDone) g_main_context_iteration(nullptr, TRUE);
    check(found == 1, "search by name");
    q.text = "needle";
    q.content = true;
    found = 0;
    searchDone = false;
    job = SearchJob::start(q, [&](std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> &&r) { found += (int)r.size(); },
                           [&] { searchDone = true; });
    while (!searchDone) g_main_context_iteration(nullptr, TRUE);
    check(found == 2, printf_str("search by content (%d)", found));
    SearchQuery back;
    check(SearchQuery::fromUri(q.toUri(), back) && back.text == "needle" && back.content, "search URI round trip");

    // Disk usage: sizes add up, a hard link counts once, a folder lists its biggest files.
    write("du/a/big.bin", std::string(300000, 'x'));
    write("du/a/small.txt", "hello");
    write("du/b/mid.bin", std::string(100000, 'y'));
    link((g_root + "/files/du/a/big.bin").c_str(), (g_root + "/files/du/a/big-link.bin").c_str());
    {
        du::Scanner sc;
        du::Options o;
        o.apparentSize = true;
        o.filesPerFolder = 1;
        auto tree = sc.run(g_root + "/files/du", o);
        const du::Node *a = tree && !tree->children.empty() ? tree->children[0].get() : nullptr;
        check(tree && tree->files == 3 && tree->folders == 2 && tree->size == 400005,
              printf_str("disk usage adds up and counts a hard link once (%llu bytes)", tree ? (unsigned long long)tree->size : 0ull));
        check(a && a->name == "a" && a->children.size() == 2 && a->children[0]->size == 300000 && a->children[1]->rest()
                  && a->children[1]->files == 1,
              "disk usage lists the biggest files first and sums the rest");
        du::Totals t = du::totals(g_root + "/files/du", true, 1);
        check(tree && t.files == 3 && t.folders == 2 && t.size == tree->size, "folder totals match the tree");
    }
    trash::delete_recursive(F("du").get(), nullptr, nullptr);

    // Archives: listed like folders, extracted without writing outside the target.
    {
        std::string dir = g_root + "/arch";
        g_mkdir_with_parents(dir.c_str(), 0755);
        for (const char *fmt : {"zip", "tar.gz", "7z", "iso"}) {
            std::string path = dir + "/test." + fmt;
            make_archive(path, fmt);
            std::string err;
            auto index = archive::load(path, &err);
            const auto *top = index ? index->children("") : nullptr;
            bool ok = top && has_entry(top, "docs") && has_entry(top, "top.bin") && !has_entry(top, "evil.txt")
                   && has_entry(index->children("docs"), "readme.txt") && has_entry(index->children("docs/sub"), "deep.txt");
            check(ok, printf_str("archive lists like a folder (%s)%s", fmt, err.empty() ? "" : (": " + err).c_str()));
        }
        std::string zip = dir + "/test.zip", out = dir + "/out";
        g_mkdir_with_parents(out.c_str(), 0755);
        std::string err;
        bool ok = archive::extract(zip, {"docs"}, out, nullptr, &err);
        char *text = nullptr;
        g_file_get_contents((out + "/docs/readme.txt").c_str(), &text, nullptr, nullptr);
        check(ok && text && std::string(text) == "hello archive" && g_file_test((out + "/docs/sub/deep.txt").c_str(), G_FILE_TEST_EXISTS),
              "extract a folder from an archive" + (err.empty() ? std::string() : ": " + err));
        g_free(text);
        archive::extract(zip, {""}, out + "/all", nullptr, nullptr);
        check(!g_file_test((dir + "/evil.txt").c_str(), G_FILE_TEST_EXISTS) && !g_file_test((out + "/evil.txt").c_str(), G_FILE_TEST_EXISTS)
                  && g_file_test((out + "/all/top.bin").c_str(), G_FILE_TEST_EXISTS),
              "extracting never writes outside the target folder");
        check(archive::resolve_path(zip + "/docs/sub") == archive::make_uri(zip, "docs/sub") && archive::resolve_path(out).empty(),
              "a typed path into an archive resolves");
        std::string dest = dir + "/copied";
        g_mkdir_with_parents(dest.c_str(), 0755);
        Ref<GFile> destDir(g_file_new_for_path(dest.c_str()));
        run_job(FileJob::copy({Ref<GFile>(g_file_new_for_uri(archive::make_uri(zip, "docs/readme.txt").c_str())),
                               Ref<GFile>(g_file_new_for_uri(archive::make_uri(zip, "docs/sub").c_str()))},
                              destDir.get()));
        check(g_file_test((dest + "/readme.txt").c_str(), G_FILE_TEST_IS_REGULAR) && g_file_test((dest + "/sub/deep.txt").c_str(), G_FILE_TEST_IS_REGULAR),
              "copy files and folders out of an archive");
        GDir *d = g_dir_open(dest.c_str(), 0, nullptr);
        bool leftover = false;
        while (const char *n = d ? g_dir_read_name(d) : nullptr)
            if (starts_with(n, ".beluga-extract-")) leftover = true;
        if (d) g_dir_close(d);
        check(!leftover, "no temporary folder left after copying out");
    }

    // The name index: answers searches, picks up new files, never shows deleted ones.
    {
        auto search = [](const std::string &text) {
            SearchQuery sq;
            sq.rootUri = file_uri(F("").get());
            sq.text = text;
            int n = 0;
            bool done = false;
            auto job = SearchJob::start(sq, [&](std::vector<std::pair<Ref<GFile>, Ref<GFileInfo>>> &&r) { n += (int)r.size(); }, [&] { done = true; });
            while (!done) g_main_context_iteration(nullptr, TRUE);
            return n;
        };
        fileindex::start(g_root + "/files");
        gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
        while (!fileindex::ready() && g_get_monotonic_time() < until) g_usleep(10000);
        write("Folder 1/.secret/x.txt", "x");
        check(fileindex::ready() && fileindex::covers(g_root + "/files") && !fileindex::covers(g_root + "/files/Folder 1/.secret"),
              "the name index covers the folder, not hidden ones");
        write("Folder 2/indexed-new-file.txt", "new");
        int found = search("indexed-new");
        g_unlink((g_root + "/files/Folder 2/indexed-new-file.txt").c_str());
        auto stale = fileindex::query(g_root + "/files", [](const char *, const char *f, bool) { return strstr(f, "indexed-new") != nullptr; });
        int after = search("indexed-new");
        check(found == 1 && after == 0, printf_str("indexed search finds new files and drops deleted ones (%d, then %d; %zu in the index before refreshing)",
                                                   found, after, stale.size()));
        trash::delete_recursive(F("Folder 1/.secret").get(), nullptr, nullptr);
    }

    // Remembered disk usage: current until the folder changes; a scan answers for its subfolders.
    {
        std::string folder = g_root + "/files/Folder 1";
        struct stat st;
        lstat(folder.c_str(), &st);
        du::remember_total(folder, true, {12345, 3, 0, g_get_real_time() / G_USEC_PER_SEC, (gint64)st.st_mtim.tv_sec, (gint64)st.st_mtim.tv_nsec});
        du::CachedTotal c;
        bool fresh = du::cached_total(folder, true, c) && c.size == 12345 && du::is_fresh(folder, c);
        g_usleep(20000);
        write("Folder 1/touch.txt", "t");
        bool stale = !du::is_fresh(folder, c);
        g_unlink((folder + "/touch.txt").c_str());
        check(fresh && stale, "remembered folder sizes stay current until the folder changes");
        du::Scanner sc;
        du::Options o;
        std::shared_ptr<du::Node> tree = sc.run(g_root + "/files", o);
        if (tree) du::remember_tree(tree, o, g_get_real_time() / G_USEC_PER_SEC);
        du::CachedTree ct;
        bool sub = du::cached_tree(folder, o, ct) && ct.node && ct.node->name == "Folder 1";
        du::CachedTotal fromTree;
        check(sub && du::cached_total(folder, true, fromTree) && fromTree.size == ct.node->size,
              "a remembered scan answers for its subfolders");
    }

    check(collate_key("file2", 0) < collate_key("file10", 0), "natural sorting");
    check(filter_match("*.txt", "notes.TXT") && filter_match("ote", "Notes") && !filter_match("*.md", "a.txt"), "filter patterns");
    check(unique_name(F("").get(), "a.txt") == "a (1).txt", "unique names");
    check(format_permissions(040755, 0) == "drwxr-xr-x" && format_permissions(0100644, 1) == "0644", "permission formats");

    Places &pl = Places::instance();
    size_t n = pl.list().size();
    pl.add({"Test", file_uri(F("").get()), "folder", false});
    check(pl.list().size() == n + 1 && pl.bestMatch(file_uri(F("Folder 1").get())) == (int)n, "places add and match");
    pl.remove((int)n);
}

} // namespace

// Renders a widget (normally a window) to a PNG file. The paintable is empty until the
// widget has been drawn since its last change; offscreen backends draw rarely, so ask
// for frames until one arrives.
bool save_widget_png(GtkWidget *w, const std::string &path, int retries)
{
    int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    if (width <= 0 || height <= 0) return false;
    GskRenderNode *node = nullptr;
    for (int tries = 0; tries <= retries && !node; ++tries) {
        GdkPaintable *p = gtk_widget_paintable_new(w);
        GtkSnapshot *s = gtk_snapshot_new();
        gdk_paintable_snapshot(p, s, width, height);
        node = gtk_snapshot_free_to_node(s);
        g_object_unref(p);
        if (!node) {
            gtk_widget_queue_draw(w);
            wait_ms(100);
        }
    }
    if (!node) return false;
    // A private Cairo renderer works on every backend, including offscreen Broadway.
    GskRenderer *r = gsk_cairo_renderer_new();
    gsk_renderer_realize_for_display(r, gtk_widget_get_display(w), nullptr);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height);
    GdkTexture *t = gsk_renderer_render_texture(r, node, &bounds);
    gsk_renderer_unrealize(r);
    g_object_unref(r);
    gsk_render_node_unref(node);
    bool ok = gdk_texture_save_to_png(t, path.c_str());
    g_object_unref(t);
    return ok;
}

namespace {

bool shoot(GtkWidget *w, const std::string &name)
{
    if (g_shots.empty()) return true;
    std::string path = g_shots + "/" + name + ".png";
    bool ok = save_widget_png(w, path, 40);
    std::cout << (ok ? "SHOT " : "FAIL shot ") << path << std::endl;
    if (!ok) g_failures++;
    return ok;
}

void gui_tests(GtkApplication *app)
{
    App::instance().selftest = true;
    App::instance().init(app);
    // A picture for previews.
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 320, 200);
    gdk_pixbuf_fill(pb, 0x3c8fd0ff);
    gdk_pixbuf_save(pb, (g_root + "/files/picture.png").c_str(), "png", nullptr, nullptr);
    g_object_unref(pb);
    for (int i = 0; i < 30; ++i) write(printf_str("many/item %02d.txt", i), "x");

    MainWindow *w = App::instance().newWindow({file_uri(F("").get())}, false);
    gtk_window_set_default_size(w->window(), 1100, 700);
    wait_ms(1500);
    View *v = w->activeView();
    check(v && v->location() == file_uri(F("").get()), "window shows the folder");
    int files = 0, folders = 0;
    guint64 size = 0;
    if (v) v->countItems(files, folders, size);
    check(folders == 3 && files >= 4, printf_str("view lists items (%d folders, %d files)", folders, files));
    check(v && !v->findItem(file_uri(F(".hidden").get())), "hidden files are hidden");
    v->setHiddenShown(true);
    wait_ms(200);
    check(v->findItem(file_uri(F(".hidden").get())) != nullptr, "show hidden files");
    v->setHiddenShown(false);

    v->setViewMode(ModeIcons);
    wait_ms(1200);
    shoot(w->widget(), "1-icons");
    check(gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(gtk_widget_get_display(w->widget())), BELUGA_APP_ID),
          "the app icon is available without installing");
    v->setViewMode(ModeCompact);
    wait_ms(600);
    shoot(w->widget(), "2-compact");
    v->setViewMode(ModeDetails);
    wait_ms(600);
    int before = v->itemView()->allocCount;
    wait_ms(500);
    check(v->itemView()->allocCount - before < 5, printf_str("details view settles (%d allocations in 0.5 s)", v->itemView()->allocCount - before));
    shoot(w->widget(), "3-details");
    v->setGrouped(true);
    v->setSortRole("type");
    wait_ms(600);
    shoot(w->widget(), "4-details-grouped");
    v->setGrouped(false);
    v->setSortRole("text");
    v->setViewMode(ModeIcons);

    w->activeContainer()->navigate(file_uri(F("Folder 2").get()));
    w->activeView()->setViewMode(ModeColumns);
    w->activeContainer()->navigate(file_uri(F("Folder 2/deep").get()));
    wait_ms(1000);
    check(w->activeView()->location() == file_uri(F("Folder 2/deep").get()) && w->activeView()->listMode(), "columns view follows into subfolders");
    shoot(w->widget(), "8-columns");
    w->activeView()->setViewMode(ModeIcons);

    // A folder that remembers its own view keeps it; other folders keep using the default.
    {
        auto go = [w](const std::string &rel) {
            w->activeContainer()->navigate(file_uri(F(rel).get()));
            wait_ms(300);
            return w->activeView();
        };
        View *pv = go("Folder 1");
        pv->setOwnProps(true);
        pv->setViewMode(ModeDetails);
        int other = go("Folder 2")->props().viewMode;
        pv = go("Folder 1");
        check(pv->hasOwnProps() && pv->props().viewMode == ModeDetails && other == ModeIcons, "a folder remembers its own view mode");
        pv->setOwnProps(false);
        check(!pv->hasOwnProps() && pv->props().viewMode == ModeIcons, "a folder goes back to the default view mode");
    }

    // The Disk Usage column: whole folders counted in the background, the biggest first when sorted by it.
    {
        settings().detailsDiskUsage = true;
        w->activeContainer()->navigate(file_uri(F("").get()));
        wait_ms(400);
        View *dv = w->activeView();
        dv->setViewMode(ModeDetails);
        dv->setSortRole("diskusage");
        dv->setSortDescending(true);
        BelugaItem *many = nullptr;
        gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        while (g_get_monotonic_time() < until) {
            wait_ms(100);
            many = dv->findItem(file_uri(F("many").get()));
            if (many && item_usage(many) > 0) break;
        }
        wait_ms(500);   // the re-sort after the last folder
        auto shown = dv->shownRoles();
        check(std::find(shown.begin(), shown.end(), "diskusage") != shown.end() && many && item_usage(many) > 0
                  && dv->itemAt(0) == many,
              "disk usage column counts whole folders and sorts the biggest first");
        shoot(w->widget(), "11-details-disk-usage");
        dv->setSortDescending(false);
        dv->setSortRole("text");
        dv->setViewMode(ModeIcons);
        settings().detailsDiskUsage = false;
    }

    w->activeContainer()->navigate(file_uri(F("").get()));
    wait_ms(600);
    v = w->activeView();

    v->setNameFilter("*.txt");
    wait_ms(200);
    v->countItems(files, folders, size);
    check(folders == 0 && files >= 2, "name filter");
    v->setNameFilter({});

    w->activeContainer()->navigate(file_uri(F("Folder 1").get()));
    wait_ms(600);
    w->activeContainer()->goUp();
    wait_ms(600);
    check(w->activeView()->location() == file_uri(F("").get()), "go up");
    w->activeContainer()->goBack();
    wait_ms(400);
    check(w->activeView()->location() == file_uri(F("Folder 1").get()), "go back");
    w->activeContainer()->goBack();
    wait_ms(400);

    settings().foldersPanel = true;
    settings().infoPanel = true;
    notify_settings_changed();
    w->activeTab()->setSplit(true, file_uri(F("many").get()));
    w->openTab(file_uri(F("Folder 2").get()), {}, false);
    wait_ms(1500);
    shoot(w->widget(), "5-split-panels-tabs");
    check(w->activeTab()->split() && w->tabCount() == 2, "split view and tabs");

    w->activeContainer()->setSearchBarVisible(true);
    SearchQuery q;
    q.rootUri = file_uri(F("").get());
    q.text = "item";
    w->activeContainer()->navigate(q.toUri());
    wait_ms(1000);
    shoot(w->widget(), "6-search");
    int sf = 0, sd = 0;
    w->activeView()->countItems(sf, sd, size);
    check(sf == 30, printf_str("search results in the view (%d)", sf));

    // Removing a place from its context menu rebuilds the list while the menu is still a child of it.
    {
        Places &pl = Places::instance();
        size_t n = pl.list().size();
        pl.add({"Menu Test", file_uri(F("Folder 1").get()), "folder", false});
        wait_ms(200);
        w->placesPanel()->runMenuAction((int)n, "remove");
        wait_ms(300);
        check(pl.list().size() == n, "remove a place from its context menu");
    }

    // Run the actions a user reaches from menus and shortcuts on a selected file, so that the
    // dialogs and panels they open are built at least once (catches crashes under ASan).
    w->activeContainer()->navigate(file_uri(F("").get()));
    wait_ms(600);
    w->activeView()->selectUris({file_uri(F("picture.png").get())});
    wait_ms(300);
    {
        GActionGroup *ag = G_ACTION_GROUP(w->window());
        const char *plain[] = {"properties", "folder-properties", "open-with", "view-properties", "remember-view", "remember-view", "selection-mode",
                               "selection-mode", "select-all", "invert-selection", "zoom-in", "zoom-out", "zoom-reset",
                               "copy", "copy-location", "show-menubar", "show-menubar", "focus-other-view",
                               "focus-other-view", "editable-location", "editable-location", "toggle-filter",
                               "toggle-filter", "reload", "duplicate", "split-to-tabs"};
        for (const char *a : plain) {
            g_action_group_activate_action(ag, a, nullptr);
            wait_ms(150);
        }
        for (const char *role : {"size", "modificationtime", "type", "text"}) {
            g_action_group_activate_action(ag, "sort-by", g_variant_new_string(role));
            wait_ms(80);
        }
        w->activeView()->selectUris({file_uri(F("picture.png").get())});
        wait_ms(200);
        w->showContextMenu(w->activeContainer(), 0, 40, 40);
        wait_ms(400);
        w->showContextMenu(w->activeContainer(), -1, 400, 400);
        wait_ms(400);
    }
    int dialogs = 0;
    GList *tops = gtk_window_list_toplevels();
    for (GList *l = tops; l; l = l->next) {
        GtkWindow *t = GTK_WINDOW(l->data);
        if (t == w->window() || MainWindow::from(t) || !gtk_widget_get_visible(GTK_WIDGET(t))) continue;
        int minH = 0;
        gtk_widget_measure(GTK_WIDGET(t), GTK_ORIENTATION_VERTICAL, -1, &minH, nullptr, nullptr, nullptr);
        const char *title = gtk_window_get_title(t);
        std::cout << "  dialog \"" << (title ? title : "") << "\" min height " << minH << std::endl;
        check(minH <= 800, printf_str("dialog \"%s\" fits a small screen (%d px)", title ? title : "", minH));
        shoot(GTK_WIDGET(t), printf_str("9-dialog-%d", dialogs));
        dialogs++;
        gtk_window_destroy(t);
    }
    g_list_free(tops);
    wait_ms(300);
    check(dialogs >= 3, printf_str("menu actions and dialogs (%d dialogs)", dialogs));

    {
        GtkWidget *duw = show_disk_usage(w, g_root + "/files");
        gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
        while (disk_usage_busy(duw) && g_get_monotonic_time() < until) wait_ms(50);
        wait_ms(300);
        check(!disk_usage_busy(duw) && disk_usage_rows(duw) >= 4, printf_str("disk usage window (%u rows)", disk_usage_rows(duw)));
        shoot(duw, "10-disk-usage");
        gtk_window_destroy(GTK_WINDOW(duw));
    }

    // Archives open like folders, and Up leads back out.
    {
        std::string archDir = file_uri(Ref<GFile>(g_file_new_for_path((g_root + "/arch").c_str())).get());
        std::string zip = g_root + "/arch/test.zip";
        ViewContainer *c = w->activeContainer();
        c->navigate(archDir);
        wait_ms(600);
        BelugaItem *zi = w->activeView()->findItem(file_uri(Ref<GFile>(g_file_new_for_path(zip.c_str())).get()));
        if (zi) w->openItems(c, {zi}, GdkModifierType(0));
        wait_ms(900);
        View *av = w->activeView();
        check(av->location() == archive::make_uri(zip, "") && av->findItem(archive::make_uri(zip, "docs"))
                  && av->findItem(archive::make_uri(zip, "top.bin")),
              "open an archive as a folder");
        shoot(w->widget(), "12-archive");
        c->navigate(archive::make_uri(zip, "docs"));
        wait_ms(700);
        bool inside = w->activeView()->findItem(archive::make_uri(zip, "docs/readme.txt")) != nullptr;
        c->goUp();
        wait_ms(600);
        bool atTop = w->activeView()->location() == archive::make_uri(zip, "");
        c->goUp();
        wait_ms(700);
        check(inside && atTop && w->activeView()->location() == archDir, "browse into an archive and back out");
    }

    w->activeContainer()->navigate("trash:///");
    wait_ms(600);
    check(w->activeView()->isTrash(), "trash location");

    show_preferences(w->widget());
    wait_ms(800);
    GtkWindow *prefs = nullptr;
    for (GList *l = gtk_application_get_windows(app); l; l = l->next)
        if (gtk_widget_has_css_class(GTK_WIDGET(l->data), "beluga-preferences")) prefs = GTK_WINDOW(l->data);
    if (prefs) {
        shoot(GTK_WIDGET(prefs), "7-preferences");
        gtk_window_destroy(prefs);
    }
    check(prefs != nullptr, "preferences window");
}

} // namespace

int run_selftest(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--screenshots") && i + 1 < argc) g_shots = argv[++i];
    char *tmp = g_dir_make_tmp("beluga-selftest-XXXXXX", nullptr);
    if (!tmp) return 2;
    g_root = tmp;
    g_free(tmp);
    for (const char *d : {"config", "data", "state", "cache", "files"}) g_mkdir_with_parents((g_root + "/" + d).c_str(), 0700);
    // Without XDG_RUNTIME_DIR, GLib falls back to the cache folder, where the display server
    // (e.g. gtk4-broadwayd) put its socket. Keep pointing there after XDG_CACHE_HOME changes.
    if (!g_getenv("XDG_RUNTIME_DIR")) g_setenv("XDG_RUNTIME_DIR", g_get_user_runtime_dir(), TRUE);
    g_setenv("XDG_CONFIG_HOME", (g_root + "/config").c_str(), TRUE);
    g_setenv("XDG_DATA_HOME", (g_root + "/data").c_str(), TRUE);
    g_setenv("XDG_STATE_HOME", (g_root + "/state").c_str(), TRUE);
    g_setenv("XDG_CACHE_HOME", (g_root + "/cache").c_str(), TRUE);
    if (!g_shots.empty()) g_mkdir_with_parents(g_shots.c_str(), 0755);
    settings().load();
    settings().confirmClosingMultipleTabs = false;
    std::cout << "Self-test folder: " << g_root << std::endl;

    core_tests();

    GtkApplication *app = gtk_application_new(BELUGA_APP_ID ".SelfTest", G_APPLICATION_NON_UNIQUE);
    // The real app's resources (its icon), which GTK would otherwise look for under the test's own ID.
    g_application_set_resource_base_path(G_APPLICATION(app), "/org/beluga/Beluga");
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *a) {
        g_application_hold(G_APPLICATION(a));
        gui_tests(a);
        App::instance().quit();
        g_application_release(G_APPLICATION(a));
    }), nullptr);
    char *fakeArgv[] = {argv[0], nullptr};
    g_application_run(G_APPLICATION(app), 1, fakeArgv);
    g_object_unref(app);

    GFile *root = g_file_new_for_path(g_root.c_str());
    trash::delete_recursive(root, nullptr, nullptr);
    g_object_unref(root);
    std::cout << (g_failures ? "SELFTEST FAILED: " : "SELFTEST OK") << (g_failures ? std::to_string(g_failures) : "") << std::endl;
    return g_failures ? 1 : 0;
}

} // namespace beluga
