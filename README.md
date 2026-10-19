# Bento

A small native text editor, written in C.

Bento opens fast, edits local files directly, and stays small enough to read:
the whole editor is one C file. It's for the edits that don't need an IDE: a
note, a script, a config file, a web page, a small project. Open the file,
change it, save it, move on.

Bento started as a fork of antirez's [kilo](https://github.com/antirez/kilo),
the terminal editor in under 1,000 lines of C, and grew a native GUI and the
comforts people expect from an editor today.

## Features

- Two builds from one source file: a GUI built on [raylib](https://www.raylib.com/),
  and a terminal editor that draws with plain VT100 escapes (no curses).
- Tabs, pinned tabs, a project sidebar, quick open, a command palette, recent
  files, and session restore.
- Line editing: duplicate, move up and down, indent and outdent, toggle comment,
  select next occurrence, and trim trailing whitespace.
- Syntax highlighting for C and C++, Python, JavaScript and TypeScript, Go,
  Rust, shell, and Markdown.
- Light and dark themes, adjustable font size, and drag-and-drop to open files.
- No accounts, no extensions, no network code. Bento never phones home.
- Built-in benchmarks for the terminal and GUI render paths.

## Build

You need a C99 compiler. The GUI also needs raylib (`brew install raylib` on
macOS).

```sh
./build.sh terminal   # builds ./bento
./build.sh gui        # builds ./bento-gui
```

Run them:

```sh
./bento notes.md
./bento-gui                  # restores your last session
./bento-gui notes.md
./bento-gui --fresh-window   # skip session restore
```

On macOS, wrap the GUI in a clickable app with its icon:

```sh
./build.sh gui
./build.sh icons
./build.sh bundle-mac        # writes bento.app
```

## Keyboard

In the GUI, Cmd and Ctrl both work as the modifier.

| Keys | Action |
| --- | --- |
| Cmd+P | Quick open |
| Cmd+Shift+P | Command palette |
| Cmd+O / Cmd+Shift+O | Open file / open folder |
| Cmd+Shift+R | Open recent |
| Cmd+N / Cmd+Shift+N | New file / new window |
| Cmd+S / Cmd+Alt+S | Save / save all |
| Cmd+W / Cmd+Shift+T | Close tab / reopen closed tab |
| Cmd+F | Find |
| Cmd+G | Go to line |
| Cmd+D | Duplicate line |
| Cmd+E | Select next occurrence |
| Cmd+/ | Toggle comment |
| Cmd+B | Toggle sidebar |
| Cmd+= / Cmd+- | Bigger / smaller font |
| Cmd+, | Settings |
| Cmd+Q | Quit |

The terminal build keeps kilo's keys: Ctrl-S saves, Ctrl-F finds, Ctrl-Z
undoes, and Ctrl-Q quits.

## Benchmarks

```sh
./bento --bench big-file.c
./bento-gui --bench-gui big-file.c
```

## Platforms

| Platform | Status |
| --- | --- |
| macOS | Main target. `./release.sh macos` builds a zipped `.app`. It isn't signed or notarized yet, so macOS will warn the first time you open it. |
| Linux | Builds. `./release.sh linux` packages both binaries in a tarball. |
| Windows | Not supported yet. The GUI code has Windows branches, but there's no tested build, and the terminal build is POSIX-only. |

## Known gaps

Bento is at version 0.1.0. Things that don't work yet:

- Replace and replace-all.
- Line wrap.
- Idle resource use. Bento uses more CPU and GPU sitting still than it should.
- C highlighting works but needs polish.

## Credits

Bento is built on [kilo](https://github.com/antirez/kilo) by Salvatore
Sanfilippo. kilo's copyright notice and BSD 2-Clause license are in
[LICENSE-kilo](LICENSE-kilo).
