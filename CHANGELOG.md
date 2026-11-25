# Changelog

## Unreleased

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
