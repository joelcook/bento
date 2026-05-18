# Bento

Bento is a familiar lightweight editor rebuilt for 2026: a small native editor
that opens fast, edits real files, and does not ask you to adopt a whole
ecosystem first.

It is for the moment when you need to change a note, script, config file, web
page, or small project and you do not want a startup screen, account, workspace
wizard, extension marketplace, or cloud sync pitch. Open the file. Make the
change. Save it. Move on.

For technical users, Bento keeps the local, direct feel of a terminal tool and
adds the pieces that make repeated editing comfortable: tabs, a project tree,
search, replace, selections, command palette, file context menus, syntax
highlighting, undo, session restore, and a GUI render path built to stay light.

## Why Bother?

Most people do not need an IDE every time they touch text. They need a reliable
place to open files quickly, see what is there, and edit without friction.

That is why lightweight editors worked so well for so many people: they were
understandable. They respected the file. They opened quickly. They helped
without taking over.

Bento is that kind of tool, rebuilt around what matters now: fast native UI,
project-level navigation, modern syntax highlighting, predictable local files,
and a codebase small enough to reason about.

Bento is not trying to beat a full IDE at being an IDE. It is trying to make the
small and medium edits feel sharp again.

## Features

- Native GUI build with raylib.
- Terminal build with direct VT100 output.
- Project tree, tabs, session restore, command palette, search, replace, and
  selections.
- Recent files, drag/drop open, settings, file context menus, and common line
  edits like duplicate, move, comment, indent, outdent, and trim whitespace.
- Syntax highlighting for C, Python, JavaScript, Go, Rust, shell, and Markdown.
- Single-file core with a small build script.
- Bench modes for terminal and GUI performance checks.

## Downloads

The clean release setup is:

- GitHub Releases for direct downloads from a personal site.
- itch.io for people who want install and update notifications through the itch
  desktop app.
- Source releases for people who want to build it themselves.

Recommended project-site links:

```text
macOS download:
https://github.com/OWNER/REPO/releases/latest/download/Bento-macos-arm64.zip

All releases:
https://github.com/OWNER/REPO/releases/latest

itch page:
https://OWNER.itch.io/bento

Source:
https://github.com/OWNER/REPO
```

Replace `OWNER/REPO` and `OWNER.itch.io` with the real project slugs when the
release pages exist.

## Platform Support

Bento is portable C, but downloadable app support means shipping and testing one
artifact per platform. A single binary will not support all platforms.

| Platform | Status | Notes |
| --- | --- | --- |
| macOS | First release target | `./release.sh macos` builds a `.app` bundle and zips it. Unsigned builds will show macOS Gatekeeper warnings. Code signing and notarization are the next step for a polished public download. |
| Linux | Buildable target | `./release.sh linux` creates a tarball with `bento`, `bento-gui`, icon, README, and VERSION. Test on the oldest distro you intend to support, or package as AppImage/Flatpak later. |
| Windows | Not a promised download yet | The GUI code has Windows branches, but do not call it supported until a Windows build is produced and tested in CI. The terminal build is POSIX-oriented. |
| Web | Not supported | Bento is a native editor, not a browser app. |

## Updates

The lightweight update hook is distribution-level:

- If users install through the itch app, itch handles update checks and update
  prompts when new builds are pushed to the same channel.
- If users download directly from your site, make the button point at GitHub's
  latest release asset URL so new visitors always get the newest build.
- Bento itself does not phone home. Adding in-app update checks would mean
  adding networking, platform updater frameworks, signing concerns, and more
  code. That is the wrong tradeoff while the goal is to keep Bento small.

If direct-download update notices become necessary later, use the itch
`wharf/latest` endpoint or a platform updater such as Sparkle for macOS as an
opt-in release feature, not as part of the core editor loop.

## Building

Terminal:

```sh
./build.sh terminal
```

GUI:

```sh
./build.sh gui
```

macOS app bundle:

```sh
./build.sh icons
./build.sh bundle-mac
```

Release artifacts:

```sh
./release.sh macos
./release.sh linux
./release.sh source
```

The version comes from `VERSION`. You can override it for a build with:

```sh
VERSION=0.1.1 ./release.sh macos
```

## Publishing

Before a public release:

- Pick a real repository slug and replace `OWNER/REPO` in this README.
- Pick an itch project slug and replace `OWNER/bento`.
- Add a license if this repository is meant to be open source.
- For macOS public downloads, plan for code signing and notarization.

For GitHub Releases, upload the files from `dist/` to a release tagged with the
same version:

```sh
./release.sh macos
./release.sh source
gh release create "v$(cat VERSION)" dist/* --title "Bento $(cat VERSION)"
```

For itch.io, push one stable channel per platform. Keep channel names stable so
updates land on the same install:

```sh
butler push dist/Bento-macos-arm64.zip OWNER/bento:macos-arm64 --userversion "$(cat VERSION)"
butler push dist/Bento-linux-arm64.tar.gz OWNER/bento:linux-arm64 --userversion "$(cat VERSION)"
```

## Personal Site Copy

Use this as a project card on a portfolio page:

```markdown
### Bento

A familiar lightweight editor rebuilt for 2026.

Bento is a small native editor for notes, scripts, config files, web pages, and
small projects. It opens fast, edits local files directly, and gives you the
comforts people expect now: tabs, project tree, command palette, search,
replace, syntax highlighting, undo, recent files, and session restore.

Under the hood it stays deliberately small: one C-file core, native GUI,
terminal build, compact render path, and performance bench modes.

[Download](https://github.com/OWNER/REPO/releases/latest)
[itch.io](https://OWNER.itch.io/bento)
[Source](https://github.com/OWNER/REPO)
```
