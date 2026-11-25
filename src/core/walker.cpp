// SPDX-License-Identifier: GPL-2.0-or-later
#include "walker.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace beluga {

namespace {

struct Task {
    std::string path;   // "" for the root folder "/"
    void *data;
};

bool is_pseudo(const std::string &p) { return p == "/proc" || p == "/sys" || p == "/dev"; }

} // namespace

void Walker::run(const std::string &path, void *rootData, const Options &o)
{
    struct stat rootSt;
    if (lstat(path.c_str(), &rootSt) != 0 || !S_ISDIR(rootSt.st_mode)) {
        if (onError) onError(path, errno ? errno : ENOTDIR, rootData);
        return;
    }
    const dev_t rootDev = rootSt.st_dev;
    std::string root = path;
    while (!root.empty() && root.back() == '/') root.pop_back();   // "/" becomes ""

    // Reads one folder and adds the folders to walk into to `out`.
    auto readFolder = [&](const Task &t, std::vector<Task> &out) {
        const char *p = t.path.empty() ? "/" : t.path.c_str();
        int fd = open(p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        DIR *d = fd >= 0 ? fdopendir(fd) : nullptr;
        if (!d) {
            int err = errno;
            if (fd >= 0) close(fd);
            if (onError) onError(p, err, t.data);
            if (onFolderDone) onFolderDone(t.data);
            return;
        }
        const int dfd = dirfd(d);
        while (!cancelled) {
            errno = 0;
            struct dirent *de = readdir(d);
            if (!de) break;
            const char *n = de->d_name;
            if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0))) continue;
            if (o.skipHidden && n[0] == '.') continue;
            struct stat st;
            if (fstatat(dfd, n, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;   // removed meanwhile
            const bool isDir = S_ISDIR(st.st_mode);
            const bool other = isDir && st.st_dev != rootDev;
            void *child = nullptr;
            const WalkEntry e{t.path, n, st, t.data, other};
            const bool descend = onEntry ? onEntry(e, &child) : true;
            if (isDir && descend && !(other && o.oneFileSystem)) {
                std::string cp = t.path + "/" + n;
                if (!is_pseudo(cp)) out.push_back({std::move(cp), child});
            }
        }
        closedir(d);
        if (onFolderDone) onFolderDone(t.data);
    };

    int threads = o.threads > 0 ? o.threads : (int)std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    if (threads == 1) {
        std::vector<Task> stack{{root, rootData}}, out;
        while (!stack.empty() && !cancelled) {
            Task t = std::move(stack.back());
            stack.pop_back();
            out.clear();
            readFolder(t, out);
            for (auto &c : out) stack.push_back(std::move(c));
        }
        return;
    }

    // A shared last-in-first-out list of folders: depth first keeps it short. Every thread takes a
    // folder, reads it and adds its subfolders; the walk ends when the list is empty and no thread
    // is still reading.
    std::mutex m;
    std::condition_variable cv;
    std::vector<Task> tasks{{root, rootData}};
    int busy = 0;
    bool done = false;
    auto worker = [&] {
        std::vector<Task> out;
        std::unique_lock<std::mutex> l(m);
        for (;;) {
            cv.wait(l, [&] { return done || !tasks.empty(); });
            if (done) return;
            Task t = std::move(tasks.back());
            tasks.pop_back();
            ++busy;
            l.unlock();
            out.clear();
            if (!cancelled) readFolder(t, out);
            l.lock();
            --busy;
            if (cancelled) tasks.clear();
            else
                for (auto &c : out) tasks.push_back(std::move(c));
            if (tasks.empty() && busy == 0) {
                done = true;
                cv.notify_all();
                return;
            }
            if (!out.empty()) cv.notify_all();
        }
    };
    std::vector<std::thread> pool;
    for (int i = 1; i < threads; ++i) pool.emplace_back(worker);
    worker();
    for (auto &th : pool) th.join();
}

} // namespace beluga
