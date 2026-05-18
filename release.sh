#!/bin/sh

set -eu

MODE="${1:-macos}"
VERSION="${VERSION:-$(cat VERSION 2>/dev/null || printf '0.0.1')}"
DIST="${DIST:-dist}"
ARCH="$(uname -m)"

mkdir -p "$DIST"

case "$MODE" in
  macos)
    if [ "$(uname -s)" != "Darwin" ]; then
      echo "macOS packaging must run on macOS" >&2
      exit 2
    fi

    ./build.sh gui
    ./build.sh icons
    ./build.sh bundle-mac

    artifact="$DIST/Bento-macos-$ARCH.zip"
    if command -v ditto >/dev/null 2>&1; then
      ditto -c -k --sequesterRsrc --keepParent bento.app "$artifact"
    elif command -v zip >/dev/null 2>&1; then
      zip -qry "$artifact" bento.app
    else
      echo "need ditto or zip to package bento.app" >&2
      exit 2
    fi
    echo "wrote $artifact"
    ;;

  linux)
    ./build.sh gui
    ./build.sh terminal

    package_dir="$DIST/Bento-$VERSION-linux-$ARCH"
    rm -rf "$package_dir"
    mkdir -p "$package_dir"
    cp bento bento-gui bento.png README.md VERSION "$package_dir"/
    artifact="$DIST/Bento-linux-$ARCH.tar.gz"
    tar -C "$DIST" -czf "$artifact" "$(basename "$package_dir")"
    echo "wrote $artifact"
    ;;

  source)
    artifact="$DIST/Bento-source.tar.gz"
    tar -czf "$artifact" \
      bento.c bento_mac.m build.sh release.sh gen_icon.c icon_data.h \
      icons bento.icns bento.png VERSION README.md ITCH_PAGE.md
    echo "wrote $artifact"
    ;;

  *)
    echo "Usage: ./release.sh [macos|linux|source]" >&2
    exit 2
    ;;
esac
