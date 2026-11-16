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

} // namespace beluga::dirsize
