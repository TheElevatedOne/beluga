// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: item counts and sizes of folders for the Size column, computed in
// worker threads.
#pragma once

#include <gio/gio.h>

#include <functional>
#include <memory>
#include <string>

namespace beluga::dirsize {

// count: entries directly inside; size: bytes up to depthLimit levels deep (when
// wantSize). Callback on the main thread; count < 0 when the folder is unreadable.
using Callback = std::function<void(int count, guint64 size)>;
void request(const std::string &uri, bool includeHidden, bool wantSize, int depthLimit,
             std::weak_ptr<void> owner, Callback cb);

// The space a whole folder takes up on disk (every level, hard links once, the folder itself
// included), for the Disk Usage column. Folders are read one after another, each on all CPU cores.
// Callback on the main thread with -1 when it can't be read or isn't local, and kOtherDrive for a folder that is
// another drive mounted there while oneFileSystem is set.
constexpr gint64 kOtherDrive = -3;
void request_usage(const std::string &uri, bool oneFileSystem, std::weak_ptr<void> owner,
                   std::function<void(gint64 bytes)> cb);

} // namespace beluga::dirsize
