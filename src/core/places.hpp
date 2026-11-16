// SPDX-License-Identifier: GPL-2.0-or-later
// Beluga: the user's places (bookmarks shown in the Places panel), kept in
// ~/.config/beluga/places.conf. On first start the list is imported from the
// old KDE places file or GTK bookmarks when present, else filled with defaults.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace beluga {

struct Place {
    std::string label;
    std::string uri;
    std::string icon;
    bool hidden = false;
};

enum class PlaceSection { Places, Remote, Recent, Search, Devices };
PlaceSection section_for_uri(const std::string &uri);
const char *section_title(PlaceSection s);
const char *section_id(PlaceSection s);

class Places {
public:
    static Places &instance();
    const std::vector<Place> &list() const { return places_; }
    void add(const Place &p, int index = -1);
    void remove(int index);
    void update(int index, const Place &p);
    void move(int from, int to);
    void setHidden(int index, bool hidden);
    int indexOf(const std::string &uri) const;
    // Place whose URI is the longest prefix of uri, or -1.
    int bestMatch(const std::string &uri) const;
    static std::string iconForUri(const std::string &uri);

    int addListener(std::function<void()> l);
    void removeListener(int id);

private:
    Places();
    void load();
    void save();
    void changed();
    std::vector<Place> places_;
    std::vector<std::pair<int, std::function<void()>>> listeners_;
};

std::vector<Place> default_places();

} // namespace beluga
