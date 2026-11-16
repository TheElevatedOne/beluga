// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: previews through the freedesktop.org thumbnail cache. Images are
// scaled with gdk-pixbuf, other types use the system's .thumbnailer programs or the
// Tumbler D-Bus service when it is installed. Results are shared with other apps.
#pragma once

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <functional>
#include <memory>
#include <string>

namespace beluga::thumbs {

struct Request {
    std::string uri;
    std::string path;       // local path, "" for remote files
    std::string mime;
    gint64 mtime = 0;
    guint64 size = 0;       // file size in bytes
    int pixels = 128;       // wanted edge length in device pixels
};

// Called on the main thread with a new reference (or nullptr when there is none).
using Callback = std::function<void(GdkPixbuf *)>;

bool supported(const std::string &mime, bool local);
bool allowed(const Request &r);   // preview settings: type and size limits
void request(const Request &r, std::weak_ptr<void> owner, Callback cb);
// Synchronous lookup in the cache only.
GdkPixbuf *cached(const Request &r);

} // namespace beluga::thumbs
