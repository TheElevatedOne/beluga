# Changelog

## Unreleased

- Fixed: context menus and other popovers use the theme's popover colours (`popover_bg_color`,
  `popover_fg_color`); style sheets that only colour `popover` (pywal, Noctalia) left them in GTK's grey. Menu rows
  are more compact, so the longest context menu (a folder's) is 502 px instead of 678 px and fits on a 1080p screen
  without scrolling.
- Added: complete translations for Czech, Slovak, German, French, Spanish, Italian, Brazilian Portuguese, Polish,
  Russian, Ukrainian, Dutch, Japanese and Simplified Chinese (703 of 704 messages; before: about 240). They live in
  `po/extra/` and survive a new import from KDE's catalogs.
- Added: a **Disk Usage** button in the tool bar (and **View → Panels → Disk Usage**, <kbd>Shift</kbd>+<kbd>F11</kbd>)
  opens Disk Usage as a pane on the right of the window, where the Information panel goes; it follows the folder
  you're in. **Tools → Disk Usage…** still opens a window of its own.
- Added: **View → Show Statusbar** (also in the Settings menu) hides and shows the status bar; it has no default
  shortcut and can get one in Configure Beluga → Keyboard Shortcuts.
- Added: the Information panel shows audio and video details like `ffprobe`: duration, format, bit rate, tags
  (title, artist, album, track, date, genre, …), cover size and each stream's codec, size, frame rate, pixel format,
  sample rate, channels and bit depth. They come from FFmpeg's libavformat, an optional build dependency
  (`-DBELUGA_FFMPEG=AUTO|ON|OFF`).
- Added: **View → Act as Administrator** asks for the administrator password with polkit (`pkexec`) and opens a
  separate Beluga window as root, with a warning bar and "(Administrator)" in its title, in your theme. It warns
  first (Configure Beluga → Confirmations). `sudo beluga` is not refused: a root Beluga finds your Wayland
  display, makes a runtime folder and a D-Bus session of its own and doesn't write into your session's folders.
- Added: `beluga --detach`, like kitty's: returns to the shell prompt at once, keeps running after the terminal
  closes, and opens the current folder when no location is given.
- Added: **Open in Beluga** in an archive's context menu.
- Changed: archives open in their program by default (double-click); turn on Configure Beluga → General → Open
  archives as folders to browse them in Beluga instead. Archives with no program set up still open in Beluga.
- Changed: the name index includes hidden files and folders, so searches with hidden files use it too (shown only
  when hidden files are asked for). The cache folder and other drives mounted in the home folder are left out of
  the index and read from the disk, so searches now also find files on those drives. On a home folder with 1.6
  million indexed names a search takes about 10 ms (32 ms with hidden files); `find` takes 0.78 s. The index is
  stored more compactly (`index-v2`; the old file is removed).
- Changed: with **Stay on this drive**, the Disk Usage column shows "other drive" for a drive mounted inside the
  folder (such as a music folder on another disk) instead of its size.
- Changed: new defaults: filter bar shown, full path in the location bar, folders from other programs open in a
  new tab, tab bar always shown, new tabs after the last one, closing split view closes the inactive view,
  full-width status bar with zoom slider, Copy To/Move To in the context menu, Disk Usage column in Details,
  preview size limits of 20 MiB (local) and 5 MiB (remote), hidden folders in the Folders panel, apparent sizes in
  Disk Usage, and maximized windows. Existing settings are kept.
- Changed: windows report the class `Beluga` (Hyprland's `class`, X11's WM_CLASS) instead of the application ID
  `org.beluga.Beluga`, so window rules can match `Beluga`. The desktop entry has `StartupWMClass=Beluga` so
  taskbars still find its icon.
- Added: a file chooser for other programs through xdg-desktop-portal (`beluga --portal`), laid out like KDE's
  Dolphin-based dialog: Places, a Beluga view, back/forward/up, view modes, hidden files, new folder, name, file
  types and the program's options. Open, open several, choose folders, save, save several. Nothing changes until
  `org.freedesktop.impl.portal.FileChooser=beluga` is set in portals.conf (see README).
- Added: an index of the names in your home folder for instant name and type searches (about 10 ms for 360 000
  names, against 145 ms for `find`), kept current without a daemon by re-reading only folders whose time
  changed; hits are checked on the disk. Saved in `~/.cache/beluga`; Configure Beluga → General turns it off.
- Added: disk usage is remembered. The Disk Usage window shows the last scan of a folder or of a folder above it at
  once and reads again after 10 minutes or on Rescan; the Disk Usage column shows remembered sizes at once and
  reads a folder again only when it changed or its size is older than 10 minutes.
- Added: zip, tar (any compression), 7z, rar and ISO files open like folders (libarchive, no gvfs needed; turn it
  off in Configure Beluga → General). Read only: copying or dragging entries out extracts them, with the usual
  "already exists" questions, and double-clicking a file opens an extracted copy (programs and desktop entries
  open as text). Typed paths such as `~/x.zip/docs` work too. Entries that try to leave the target folder
  (`..`, absolute paths) are skipped. libarchive is now required to build.
- Added: **View → Remember View for This Folder** (also in the toolbar's view menu and the folder's context menu)
  gives one folder its own view mode, sorting and columns while the others keep the shared style. Turning it off
  returns the folder to the shared style. **Adjust View Display Style** can now apply to the current folder (and its
  subfolders) when all folders share one style, and "All folders" sets the shared style and clears the folders'
  own settings.
- Added: **Disk Usage** (Ctrl+Shift+U, Tools → Disk Usage…, a folder's context menu): a tree of what takes up the
  space in a folder, biggest first, with share bars and file counts, like dust. It reads folders on all CPU cores
  and counts hard links once (2 million files under `/usr`: 0.3 s with a warm cache, the same total as `du`).
  Disk usage or apparent size; optionally stays on one drive. Double-click opens an entry in Beluga.
- Added: a **Disk Usage** column for the Details view (Configure Beluga → View → Details, or per folder in Show
  Additional Information): the space each file and each whole folder takes up on disk, read in the background,
  with a bar for its share of the folder. It can be sorted (also in Sort By) and grouped by.
- Changed: searching local folders, folder sizes in the views and the size in Properties use the same parallel
  reader instead of GIO. Search still needs no index (no Baloo).
- Changed: Beluga has its own icon (`org.beluga.Beluga`) instead of the system file manager icon: a folder in the
  style of Dolphin's icon, with a beluga where Dolphin has its dolphin.
- Fixed: typing in the location bar, the filter, the search field or an inline rename no longer triggers
  shortcuts. Delete used to move the selected files to the trash, and Backspace, Space, `/`, Ctrl+C/V/X/Z/A
  acted on the files instead of the text.
- Fixed: removing an entry from the Places panel's context menu froze Beluga.
- Fixed: dragging an entry in the Places panel to reorder it always moved the first entry.
- Fixed: opening a broken `.desktop` file crashed Beluga.
- Fixed: closing a window while a paste, drop, duplicate or device mount was still running could crash Beluga.
- Fixed: closing a window while a "File Already Exists" question was open left that copy or move hanging.
- Fixed: Paste waited for the program that owned the clipboard and froze the window when it was slow; long
  lists of copied files were cut off at 1 MiB.
- Fixed: "Apply to all subfolders and files" in Properties → Permissions also changed the owner, group or others
  permissions you didn't touch.
- Fixed: deleting an item in the Trash, or moving it out, left its `.trashinfo` file behind.
- Fixed: "Extract Here" overwrote existing files and "Compress to .tar.gz" overwrote an existing archive. Both
  now keep existing files; the archive gets a free name such as `name (1).tar.gz`. Existing `actions.conf`
  files with the old default commands are updated.
- Fixed: custom actions run in a terminal ignored `%d` in the terminal command.
- Fixed: selecting files in a remote folder (sftp, smb) queried the server on every click.
- Fixed: a negative zoom level in `viewprops.conf` gave negative icon sizes.

## 0.1.0

First release of Beluga, a rewrite of KDE's Dolphin with plain GTK 4.

- Added the Icons, Compact, Details and Columns views, with grouping, previews, expandable folders in Details
  and resizable columns.
- Added tabs (reorder, detach, reopen closed tabs) and split view with copy/move to the other view.
- Added the Places, Folders and Information panels. The Information panel shows image dimensions and the
  duration of audio and video files, and plays them.
- Added threaded copy, move, link, rename (inline and batch), duplicate, trash and delete with conflict dialogs
  and undo/redo.
- Added the freedesktop.org trash (restore, empty, age and size limits), search by name and content, UDisks2
  devices, thumbnails from the shared cache, `.thumbnailer` programs and Tumbler.
- Added <kbd>F4</kbd> to open your terminal, custom context menu actions, and Configure Beluga with the settings
  Dolphin kept in KDE System Settings.
- Added the `org.freedesktop.FileManager1` D-Bus service and `--daemon`.
- Added translations for 61 languages, taken from Dolphin, KDE Frameworks and GTK where the text is the same
  (about 40% of the messages).
- Removed everything that needs KDE: the Konsole panel, the Help menu, KDE-only actions, Baloo metadata and
  version control plugins.
