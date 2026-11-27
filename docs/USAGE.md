# Using Beluga

- [Command line](#command-line)
- [Keyboard shortcuts](#keyboard-shortcuts)
- [Vim mode](#vim-mode)
- [Beluga as the file chooser](#beluga-as-the-file-chooser)
- [Folder views](#folder-views)
- [Search and Disk Usage](#search-and-disk-usage)
- [Archives](#archives)
- [Testing](#testing)
- [Translating](#translating)

## Command line

```text
Usage:
  beluga [OPTION…] [FILE or FOLDER…]

Application Options:
  -s, --select              Select the given files in their folders
  --split                   Open the window in split view
  --new-window              Always open a new window
  --daemon                  Keep running in the background for the FileManager1 D-Bus service
  --detach                  Detach from the terminal (without a location: open the current folder)
  --portal                  Run the file chooser for xdg-desktop-portal (started by D-Bus)
  -v, --version             Show the version
```

A second `beluga` call opens a new window in the running instance, or a new tab when **Configure Beluga → Interface →
Folders & Tabs → Keep a single Beluga window, opening new folders in tabs** is on.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| <kbd>F2</kbd> / <kbd>Del</kbd> / <kbd>Shift</kbd>+<kbd>Del</kbd> | rename / move to trash / delete |
| <kbd>F3</kbd> / <kbd>F4</kbd> | split view / open terminal |
| <kbd>F5</kbd> / <kbd>F6</kbd> / <kbd>Ctrl</kbd>+<kbd>L</kbd> | reload / editable location / replace location |
| <kbd>Ctrl</kbd>+<kbd>1</kbd> … <kbd>Ctrl</kbd>+<kbd>4</kbd> | Icons / Compact / Details / Columns view |
| <kbd>F7</kbd> / <kbd>F9</kbd> / <kbd>F11</kbd> | Folders / Places / Information panel |
| <kbd>Shift</kbd>+<kbd>F11</kbd> | Disk Usage pane |
| <kbd>F10</kbd> | new folder |
| <kbd>Ctrl</kbd>+<kbd>I</kbd> or <kbd>/</kbd> / <kbd>Ctrl</kbd>+<kbd>F</kbd> | filter bar / search |
| <kbd>Ctrl</kbd>+<kbd>H</kbd> | show hidden files |
| <kbd>Ctrl</kbd>+<kbd>T</kbd> / <kbd>Ctrl</kbd>+<kbd>W</kbd> / <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>T</kbd> | new tab / close tab / reopen closed tab |
| <kbd>Space</kbd> | selection mode |
| <kbd>Alt</kbd>+<kbd>←</kbd> / <kbd>Backspace</kbd> / <kbd>Alt</kbd>+<kbd>↑</kbd> | back / back / up |
| <kbd>Ctrl</kbd>+<kbd>M</kbd> | show the menu bar |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>U</kbd> | Disk Usage of the selected or current folder |
| <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>,</kbd> | Configure Beluga |

Every shortcut can be changed in **Configure Beluga → Keyboard Shortcuts**. Typing a name in a view jumps to the
first matching item.

## Vim mode

Turn it on in **Configure Beluga → Vim Mode** (it is off by default). The views then take Vim-like keys; text
fields (renaming, the location bar) keep typing normally, and keys with <kbd>Ctrl</kbd> or <kbd>Alt</kbd> that Vim
mode doesn't use keep their usual meaning. A line at the bottom of the window shows the mode and the keys typed so
far. Every key sequence below can be changed on the same page; `<leader>` is <kbd>Space</kbd> unless you pick
another key.

| Keys | Action |
|---|---|
| `h` `j` `k` `l`, arrows | move (with a count: `5j`) |
| `gg` / `G`, `Ctrl+d` / `Ctrl+u` | first / last item, half a page down / up |
| `Enter` / `-` | open / go to the parent folder |
| `H` / `L`, `gh` | back / forward, home |
| `v` | select every item the cursor passes |
| `V` | select a range, like <kbd>Shift</kbd> (in reading order, across lines) |
| `Ctrl+v` | select a block of the grid |
| `Esc` | back to normal mode |
| `y` / `yy` | copy the selection / the item under the cursor |
| `d` / `dd` / `D` | cut / move to the trash / delete |
| `p` / `P` | paste / paste into the folder under the cursor |
| `i` / `a` / `cw` | rename with the cursor at the start / after the name / with the name selected |
| `o` / `O` | create a folder / a file |
| `u` / `Ctrl+r` | undo / redo |
| `/`, `<leader>/` | filter, search |
| `gt` / `gT`, `qq`, `ZZ` | next / previous tab, close the tab, close the window |
| `<leader>t` / `<leader>s` / `<leader>h` | new tab / split view / hidden files |
| `<leader>d` / `<leader>i` / `<leader>p` | Disk Usage pane / Information panel / Places panel |
| `K` | properties |
| `:` | the command line |

Commands: `:q` closes the window (`:q!` without asking about its tabs), `:qa` (`:qa!`) quits Beluga, `:e PATH` or
`:cd PATH` opens a folder, `:tabnew [PATH]`, `:tabclose`, `:tabonly`, `:sp` / `:vs [PATH]` split the view, `:only`,
`:mkdir NAME`, `:touch NAME`, `:set hidden` / `nohidden` / `hidden!`, `:set view=details`, `:sort size`, `:pwd`,
`:term`, `:du`, `:help`, and `:!COMMAND` runs a shell command in the folder.

## Beluga as the file chooser

Programs that open and save files through xdg-desktop-portal can get Beluga's dialog instead of the GTK or KDE one.
Installing Beluga adds the backend (`/usr/share/xdg-desktop-portal/portals/beluga.portal` and a D-Bus service that
starts `beluga --portal`), but nothing uses it until you pick it. In `~/.config/xdg-desktop-portal/portals.conf`:

```ini
[preferred]
default=hyprland;gtk
org.freedesktop.impl.portal.FileChooser=beluga
```

Keep your own `default=` line, then restart the portal: `systemctl --user restart xdg-desktop-portal`.

Flatpak programs always use the portal. Others need to be told: Firefox with `widget.use-xdg-desktop-portal.file-picker`
set to `1` in `about:config`, GTK 4 programs with `GDK_DEBUG=portals`, GTK 3 programs with `GTK_USE_PORTAL=1`, Qt
programs with `QT_QPA_PLATFORMTHEME=xdgdesktopportal`.

The dialog remembers its last folder, size and view in `~/.config/beluga/filechooser.conf`. It never changes the view
settings of your folders or Beluga's own settings.

## Folder views

All folders share one view style (mode, zoom, sorting, columns, hidden files). **View → Remember View for This
Folder** gives a folder its own; it is saved in the `[Beluga]` group of the folder's `.directory` file, so it stays
with the folder when you move, copy or sync it. Dolphin's `[Dolphin]` group and folder icons (`[Desktop Entry]`) in
the same file are left alone. Folders where `.directory` can't be written (read only, remote, inside archives) and
the trash, search and recent views keep their own view in `~/.local/share/beluga/viewprops.conf`.

**View → Adjust View Display Style…** applies a style to the current folder, the folder and all its subfolders, or
all folders. "All folders" also makes every folder's own view older than that moment stop counting.

With **Configure Beluga → View → General → Display style → Remember display style for each folder**, every folder
you change remembers its own view, as in Dolphin.

## Search and Disk Usage

Name and type searches in your home folder answer from Beluga's own index of names, hidden folders included. Measured
on a home folder with 1.6 million indexed names (warm disk cache): a search takes about 10 ms, or 32 ms with hidden
files; `find ~ -xdev -iname` with the same pattern takes 0.78 s. The index needs no daemon: before showing results it
reads again only the folders whose time changed, and every hit is checked on the disk. `~/.cache` and other drives
mounted inside your home folder aren't indexed. The index takes about 140 MB of memory and 84 MB in `~/.cache/beluga`
for those 1.6 million names, and can be turned off in **Configure Beluga → View → General**. Content, date and size
searches read the folders on all CPU cores.

**Disk Usage** (<kbd>Shift</kbd>+<kbd>F11</kbd> for a pane that follows the current folder,
<kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>U</kbd> for a window) shows what takes up the space in a folder as a tree,
biggest first, like [dust](https://github.com/bootandy/dust). It counts hard links once and never follows links:
2 million files under `/usr` take 0.3 s with a warm cache (dust: 1.4 s, `du`: 2.6 s), with the same total as `du`.
**Stay on this drive** leaves out other drives mounted inside the folder. Results are remembered in `~/.cache/beluga`
until the folder changes. **Configure Beluga → View → Details → Show a Disk Usage column** adds the same numbers to
the Details view.

## Archives

zip, tar (`.tar.gz`, `.tar.xz`, `.tar.zst`, …), 7z, rar and ISO images can be browsed like folders through
libarchive. By default a double-click opens an archive in its program (Ark, File Roller, …) and **Open in Beluga** in
its context menu browses it; **Configure Beluga → View → General → Browse compressed files as folders** swaps the two.
Archives are read only: copy or drag entries out to extract them. Entries with `..` or absolute paths are never
written outside the target folder.

## Testing

`beluga --selftest` runs 82 checks of the file operation engine (copy, move, conflicts, trash and restore, undo,
rename, search, sorting, places, folder views) and of the window (all four views, filter, navigation, split view,
tabs, search, trash, menu actions and dialogs, preferences). It uses a temporary folder and a throwaway profile, so
your settings, places and trash are not touched. Add `--screenshots DIR` to save PNGs of each view.

To run it without opening a window, use GTK's offscreen Broadway backend:

```sh
gtk4-broadwayd :5 &
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 ./build/beluga --selftest --screenshots /tmp/beluga-shots
```

## Translating

The messages are in `po/beluga.pot` (gettext domain `beluga`). `po/import-kde-translations.py` builds each
`po/<language>.po` from Dolphin's translations in this repository's history, the KDE Frameworks and GTK catalogs
installed on the machine, and `po/extra/<language>.po`, which holds the translations written for Beluga's own
messages. Add or correct translations in `po/extra/<language>.po`, then run:

```sh
cmake --build build --target pot
python3 po/import-kde-translations.py . 30
```
