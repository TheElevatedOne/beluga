// SPDX-License-Identifier: GPL-2.0-or-later
#include "dirsize.hpp"

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
            total += g_file_info_get_size(info);
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
        guint64 size = wantSize && count >= 0 ? tree_size(dir, 0, depthLimit, owner) : 0;
        g_object_unref(dir);
        run_on_main([owner, cb, count, size] {
            if (!owner.expired()) cb(count, size);
        });
    });
}

} // namespace beluga::dirsize
