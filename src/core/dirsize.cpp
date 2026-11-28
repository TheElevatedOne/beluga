// SPDX-License-Identifier: GPL-2.0-or-later
#include "dirsize.hpp"

#include "diskusage.hpp"
#include "fileops.hpp"
#include "workqueue.hpp"

namespace beluga::dirsize {

namespace {

WorkQueue &queue()
{
    static WorkQueue *q = new WorkQueue(2, true);   // never destroyed: threads wait on it at exit
    return *q;
}

guint64 tree_size(GFile *dir, int depth, int limit, const std::weak_ptr<void> &owner)
{
    if (depth > limit || owner.expired()) return 0;
    GFileEnumerator *en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
    if (!en) return 0;
    guint64 total = 0;
    GFileInfo *info;
    while ((info = g_file_enumerator_next_file(en, nullptr, nullptr))) {
        if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
            GFile *child = g_file_get_child(dir, g_file_info_get_name(info));
            total += tree_size(child, depth + 1, limit, owner);
            g_object_unref(child);
        } else {
            total += info_size(info);
        }
        g_object_unref(info);
    }
    g_object_unref(en);
    return total;
}

} // namespace

void request(const std::string &uri, bool includeHidden, bool wantSize, int depthLimit, std::weak_ptr<void> owner, Callback cb)
{
    queue().push([=] {
        if (owner.expired()) return;
        GFile *dir = g_file_new_for_uri(uri.c_str());
        int count = -1;
        GFileEnumerator *en = g_file_enumerate_children(dir, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN,
                                                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, nullptr);
        if (en) {
            count = 0;
            GFileInfo *info;
            while ((info = g_file_enumerator_next_file(en, nullptr, nullptr))) {
                if (includeHidden || !g_file_info_get_is_hidden(info)) count++;
                g_object_unref(info);
            }
            g_object_unref(en);
        }
        guint64 size = 0;
        if (wantSize && count >= 0) {
            // Local folders with the fast walker (one thread: several folders are measured at once).
            std::string path = take(g_file_get_path(dir));
            if (!path.empty() && g_file_is_native(dir))
                size = du::totals(path, true, 1, depthLimit, [owner] { return owner.expired(); }).size;
            else
                size = tree_size(dir, 0, depthLimit, owner);
        }
        g_object_unref(dir);
        run_on_main([owner, cb, count, size] {
            if (!owner.expired()) cb(count, size);
        });
    });
}

void request_usage(const std::string &uri, bool oneFileSystem, std::weak_ptr<void> owner, std::function<void(gint64)> cb)
{
    static WorkQueue *usageQueue = new WorkQueue(1, true);   // never destroyed, like queue()
    // A remembered total shows at once; the folder is read again only when it isn't current.
    std::string local = take(g_filename_from_uri(uri.c_str(), nullptr, nullptr));
    // A drive mounted inside the folder being viewed (say ~/music on another disk) isn't counted when staying on
    // one drive, like inside the Disk Usage window: kOtherDrive instead of its size.
    if (oneFileSystem && !local.empty()) {
        struct stat st, pst;
        std::string parent = take(g_path_get_dirname(local.c_str()));
        if (lstat(local.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && stat(parent.c_str(), &pst) == 0 && st.st_dev != pst.st_dev) {
            run_on_main([owner, cb] {
                if (!owner.expired()) cb(kOtherDrive);
            });
            return;
        }
    }
    du::CachedTotal c;
    if (!local.empty() && du::cached_total(local, oneFileSystem, c)) {
        gint64 bytes = (gint64)c.size;
        run_on_main([owner, cb, bytes] {
            if (!owner.expired()) cb(bytes);
        });
        if (du::is_fresh(local, c)) return;
    }
    usageQueue->push([=] {
        if (owner.expired()) return;
        gint64 bytes = -1;
        GFile *dir = g_file_new_for_uri(uri.c_str());
        std::string path = take(g_file_get_path(dir));
        struct stat st;
        if (!path.empty() && g_file_is_native(dir) && lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            du::Totals t = du::totals(path, false, 0, -1, [owner] { return owner.expired(); }, oneFileSystem);
            bytes = (gint64)(t.size + (guint64)st.st_blocks * 512);
            if (!owner.expired())
                du::remember_total(path, oneFileSystem, {(guint64)bytes, t.files, t.folders, g_get_real_time() / G_USEC_PER_SEC,
                                                         (gint64)st.st_mtim.tv_sec, (gint64)st.st_mtim.tv_nsec});
        }
        g_object_unref(dir);
        run_on_main([owner, cb, bytes] {
            if (!owner.expired()) cb(bytes);
        });
    });
}

} // namespace beluga::dirsize
