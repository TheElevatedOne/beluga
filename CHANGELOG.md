# Changelog

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
