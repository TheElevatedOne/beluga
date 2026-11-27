# Beluga

![Static Badge](https://img.shields.io/badge/CLAUDE-VIBECODED?style=for-the-badge&logo=claude&label=VIBECODED&labelColor=%23333333&color=%23de7356) [![AUR Version](https://img.shields.io/aur/version/beluga-bin?style=for-the-badge&logo=archlinux)](https://aur.archlinux.org/packages/beluga-bin) [![AUR Version](https://img.shields.io/aur/version/beluga-git?style=for-the-badge&logo=archlinux)](https://aur.archlinux.org/packages/beluga-git)

<p align="center"><img src="data/icons/org.beluga.Beluga.svg" alt="Beluga icon: a white beluga whale swimming across a blue folder" width="128"></p>

A file manager for Linux written in C++ with plain GTK 4. It is a rewrite of KDE's Dolphin: the same views,
panels, tabs, split view and shortcuts, without Qt, KDE Frameworks or libadwaita. It contains no Dolphin code.

![Main window with a pywal colour theme: the Places panel on the left, a folder in the Icons view on the right](docs/screenshots/main.png)

## Features

- **Views**: Icons, Compact, Details and Columns (<kbd>Ctrl</kbd>+<kbd>1</kbd>–<kbd>4</kbd>), grouping, and a
  view per folder saved in the folder's `.directory` file, so it moves with the folder.
- **Tabs and split view**, with copy and move to the other view.
- **Panels**: Places, Folders tree, Information (previews, audio/video details through FFmpeg).
- **File operations** in the background with conflict dialogs, batch rename and undo/redo.
- **Trash without gvfs**, with age and size limits, optionally kept on the home drive.
- **Search without Baloo**: an index of your home folder answers name searches in about 10 ms.
- **Disk Usage**: a dust-like tree of what takes the space, and an optional Details column.
- **Archives as folders** (zip, tar, 7z, rar, ISO) through libarchive, read only.
- **Folder icons**, **devices** (UDisks2), **custom context menu actions**, **Act as Administrator** (polkit).
- **File chooser** for other programs through xdg-desktop-portal.
- **Vim mode** (off by default).
- **63 languages**, all complete.
- **Your GTK theme's colours** (pywal works), square corners, no titlebar of its own.

Details, shortcuts and Vim keys are in [docs/USAGE.md](docs/USAGE.md).

![Split view with tabs, the Folders panel and the Information panel](docs/screenshots/split.png)

## Installation

| Package | Contents |
|---|---|
| [`beluga-bin`](https://aur.archlinux.org/packages/beluga-bin) | the release binary from GitHub |
| [`beluga-git`](https://aur.archlinux.org/packages/beluga-git) | built from the latest commit |

Other distributions: download `beluga-<version>-linux-x86_64.tar.gz` from the
[releases](https://github.com/TheElevatedOne/beluga/releases) and copy its `bin/` and `share/` folders into `/usr`
or `~/.local`, or build it.

Needs GTK 4.16, GLib 2.84 and libarchive 3.6 or newer. Optional at run time: UDisks2 (devices), Tumbler
(video/PDF previews), gvfs (`sftp://` and other network locations), polkit with an agent (**Act as Administrator**).

### Building

```sh
# Arch:   sudo pacman -S base-devel cmake gtk4 libarchive ffmpeg gettext
# Debian: sudo apt install build-essential cmake pkg-config gettext libgtk-4-dev libarchive-dev libavformat-dev
# Fedora: sudo dnf install gcc-c++ cmake gettext gtk4-devel libarchive-devel ffmpeg-free-devel
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

FFmpeg is optional (`-DBELUGA_FFMPEG=AUTO|ON|OFF`). `-DBELUGA_INSTALL_FILEMANAGER1_SERVICE=ON` installs a D-Bus
service so "Show in folder" starts Beluga when it isn't running.

## Quick start

`beluga` opens your home folder (or your last session), `beluga ~/Downloads` a folder, `beluga --select FILE` its
folder with the file selected, and `beluga --detach` the current folder without holding the terminal.

## Coming from Dolphin

- **Removed**: the Konsole panel, the Help menu, KDE-only actions (KFind, Kompare, Filelight, activities), Baloo
  tags and ratings, version control plugins.
- **Moved into Beluga**: click behaviour, the terminal command, trash limits, places and shortcuts, which KDE kept in
  System Settings. **Disk Usage** replaces Filelight.
- **Places** are copied from Dolphin's `user-places.xbel` (or GTK bookmarks) on the first start.
- **Act as Administrator** opens a separate root window through `pkexec`.

## Settings

| File | Contents |
|---|---|
| `~/.config/beluga/beluga.conf` | all settings and custom shortcuts |
| `~/.config/beluga/places.conf` | the Places panel |
| `~/.config/beluga/actions.conf` | custom context menu actions |
| `.directory` in a folder | that folder's own view (`[Beluga]` group) and icon |
| `~/.local/share/beluga/viewprops.conf` | the default view, and own views of folders where `.directory` can't be written |
| `~/.local/state/beluga/session.conf` | open windows and tabs |
| `~/.cache/beluga/` | the search index, remembered folder sizes; safe to delete |

## Known limitations

- Archives are read only, and files in password-protected archives can't be opened.
- Network locations need gvfs.
- Content search skips binary files, so it doesn't find text inside PDFs or office documents.
- The Information panel shows no photo details (camera, exposure, location).

## License

GPL-2.0-or-later, see [COPYING](COPYING).
