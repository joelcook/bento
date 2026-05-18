# Bento Itch Page Draft

## Tagline

A familiar lightweight editor rebuilt for 2026.

## Short Description

Bento is a small native editor for notes, scripts, config files, web pages, and
small projects. It opens fast, edits local files directly, and gives you the
comforts people expect now: tabs, project tree, command palette, search,
replace, syntax highlighting, undo, recent files, and session restore.

## Page Copy

Most editors want to become your whole workspace. Bento is for the moments when
you just need to edit a file.

If you have ever liked the directness of a simple desktop editor, Bento should
make sense immediately: open a file, change the text, save it, move on. No
account. No project import. No cloud setup. No extension hunt before you can
type.

The 2026 part is what sits around that simple idea: project navigation, tabs,
session restore, command palette, file context menus, search and replace,
selections, syntax highlighting, a native GUI, and a terminal build for people
who like that workflow.

For technical users, Bento is deliberately small. The core lives in one C file.
The terminal build draws directly with VT100 escape codes. The GUI build uses
raylib and keeps a compact render path focused on text, rows, and files. It is
local-first software that you can build, inspect, and understand.

Bento is not trying to replace a full IDE. It is trying to make small and medium
edits feel sharp again.

## Features

- Native GUI editor.
- Terminal editor.
- Tabs and session restore.
- Project tree.
- Command palette, recent files, and drag/drop open.
- File context menus and settings.
- Search and replace.
- Copy, cut, paste, select all, undo, duplicate line, move line, toggle
  comment, indent, outdent, and trim whitespace.
- Syntax highlighting for common languages.
- Lightweight build and benchmark commands.

## Install Notes

For update notifications, install through the itch desktop app. Direct downloads
work too, but they will not update themselves.

macOS users may see a warning if the app is not yet signed and notarized. That
does not mean the app is trying to do anything networked or hidden. It means the
build has not gone through Apple's developer-signing flow yet.

## Platform Notes

The first polished downloadable target is macOS.

Linux builds are planned as tarballs first, then AppImage or Flatpak if the
audience needs a smoother install.

Windows should not be advertised as supported until a Windows build is produced
and tested.

## Release Channels

Suggested itch channels:

```text
macos-arm64
macos-x64
linux-x64
windows-x64
source
```

Push updates to the same channel name each time so the itch app can notify users
and apply updates correctly.

## Download Button Text

Download Bento

## Launch Description

Open a folder or file and edit locally. No login. No project import. No setup
wizard.
