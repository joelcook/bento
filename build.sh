#!/bin/sh

set -eu

CC="${CC:-cc}"
CFLAGS="${CFLAGS:-}"
VERSION="${VERSION:-$(cat VERSION 2>/dev/null || printf '0.0.1')}"
COMMON_CFLAGS="-Wall -Wextra -pedantic -std=c99 -Dbento_VERSION=\"$VERSION\" $CFLAGS"
MODE="${1:-terminal}"

case "$MODE" in
  terminal)
    "$CC" bento.c -o bento $COMMON_CFLAGS
    ;;
  raylib|gui)
    RAYLIB_CFLAGS="${RAYLIB_CFLAGS:-}"
    RAYLIB_LIBS="${RAYLIB_LIBS:-}"

    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists raylib; then
      RAYLIB_CFLAGS="$RAYLIB_CFLAGS $(pkg-config --cflags raylib)"
      RAYLIB_LIBS="$RAYLIB_LIBS $(pkg-config --libs raylib)"
    else
      if [ -d /opt/homebrew/include ]; then
        RAYLIB_CFLAGS="$RAYLIB_CFLAGS -I/opt/homebrew/include"
      fi
      if [ -d /usr/local/include ]; then
        RAYLIB_CFLAGS="$RAYLIB_CFLAGS -I/usr/local/include"
      fi
      if [ -d /opt/homebrew/lib ]; then
        RAYLIB_LIBS="$RAYLIB_LIBS -L/opt/homebrew/lib"
      fi
      if [ -d /usr/local/lib ]; then
        RAYLIB_LIBS="$RAYLIB_LIBS -L/usr/local/lib"
      fi
      RAYLIB_LIBS="$RAYLIB_LIBS -lraylib"

      case "$(uname -s)" in
        Darwin)
          RAYLIB_LIBS="$RAYLIB_LIBS -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo"
          ;;
        Linux)
          RAYLIB_LIBS="$RAYLIB_LIBS -lm -lpthread -ldl -lrt -lX11"
          ;;
        MINGW*|MSYS*|CYGWIN*)
          RAYLIB_LIBS="$RAYLIB_LIBS -lopengl32 -lgdi32 -lwinmm"
          ;;
      esac
    fi

    EXTRA_SRC=""
    case "$(uname -s)" in
      Darwin)
        # Cocoa hook that puts the bento icon in the dock when running
        # unbundled (./bento-gui). Compiled as Objective-C; Cocoa is already
        # linked above.
        RAYLIB_CFLAGS="$RAYLIB_CFLAGS -fblocks"
        EXTRA_SRC="$EXTRA_SRC bento_mac.m"
        ;;
    esac

    "$CC" bento.c $EXTRA_SRC -o bento-gui $COMMON_CFLAGS -Dbento_RAYLIB $RAYLIB_CFLAGS $RAYLIB_LIBS
    ;;
  test)
    "$CC" test_undo.c -o test_undo $COMMON_CFLAGS
    ./test_...
    ;;
  icons)
    # Build a tiny helper that reuses guiBuildBentoIcon from bento.c, then run
    # it to emit icon-{16,32,...,1024}.png. After that, fan out to whatever
    # platform-specific bundlers are installed.
    RAYLIB_CFLAGS="${RAYLIB_CFLAGS:-}"
    RAYLIB_LIBS="${RAYLIB_LIBS:-}"
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists raylib; then
      RAYLIB_CFLAGS="$RAYLIB_CFLAGS $(pkg-config --cflags raylib)"
      RAYLIB_LIBS="$RAYLIB_LIBS $(pkg-config --libs raylib)"
    else
      [ -d /opt/homebrew/include ] && RAYLIB_CFLAGS="$RAYLIB_CFLAGS -I/opt/homebrew/include"
      [ -d /usr/local/include ]    && RAYLIB_CFLAGS="$RAYLIB_CFLAGS -I/usr/local/include"
      [ -d /opt/homebrew/lib ]     && RAYLIB_LIBS="$RAYLIB_LIBS -L/opt/homebrew/lib"
      [ -d /usr/local/lib ]        && RAYLIB_LIBS="$RAYLIB_LIBS -L/usr/local/lib"
      RAYLIB_LIBS="$RAYLIB_LIBS -lraylib"
      case "$(uname -s)" in
        Darwin) RAYLIB_LIBS="$RAYLIB_LIBS -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo" ;;
        Linux)  RAYLIB_LIBS="$RAYLIB_LIBS -lm -lpthread -ldl -lrt -lX11" ;;
        MINGW*|MSYS*|CYGWIN*) RAYLIB_LIBS="$RAYLIB_LIBS -lopengl32 -lgdi32 -lwinmm" ;;
      esac
    fi

    
    

    # Mac .icns
    if command -v iconutil >/dev/null 2>&1 && command -v sips >/dev/null 2>&1; then
      rm -rf bento.iconset
      mkdir -p bento.iconset
      cp icons/icon_16x16.png   bento.iconset/icon_16x16.png
      cp icons/icon_32x32.png   bento.iconset/icon_16x16@2x.png
      cp icons/icon_32x32.png   bento.iconset/icon_32x32.png
      cp icons/icon_64x64.png   bento.iconset/icon_32x32@2x.png
      cp icons/icon_128x128.png  bento.iconset/icon_128x128.png
      cp icons/icon_256x256.png  bento.iconset/icon_128x128@2x.png
      cp icons/icon_256x256.png  bento.iconset/icon_256x256.png
      cp icons/icon_512x512.png  bento.iconset/icon_256x256@2x.png
      cp icons/icon_512x512.png  bento.iconset/icon_512x512.png
      cp icons/icon_1024x1024.png bento.iconset/icon_512x512@2x.png
      iconutil -c icns bento.iconset -o bento.icns
      rm -rf bento.iconset
      echo "wrote bento.icns"
    fi

    # Windows .ico (multi-resolution) via ImageMagick if present.
    if command -v magick >/dev/null 2>&1; then
      magick icons/icon_16x16.png icons/icon_32x32.png icons/icon_64x64.png icons/icon_64x64.png icons/icon_128x128.png icons/icon_256x256.png bento.ico 2>/dev/null || \
        magick icons/icon_16x16.png icons/icon_32x32.png icons/icon_64x64.png icons/icon_128x128.png icons/icon_256x256.png bento.ico
      echo "wrote bento.ico"
    elif command -v convert >/dev/null 2>&1; then
      convert icons/icon_16x16.png icons/icon_32x32.png icons/icon_64x64.png icons/icon_128x128.png icons/icon_256x256.png bento.ico
      echo "wrote bento.ico"
    fi

    # Linux desktop icon: the 256px PNG is the conventional size for
    # ~/.local/share/icons/hicolor/256x256/apps/bento.png.
    cp icons/icon_256x256.png bento.png
    echo "wrote bento.png (use for .desktop icon)"
    ;;
  bundle-mac)
    # Wrap bento-gui into a clickable bento.app with the .icns icon embedded.
    [ -f bento-gui ] || { echo "build bento-gui first: ./build.sh gui" >&2; exit 2; }
    [ -f bento.icns ] || { echo "build bento.icns first: ./build.sh icons" >&2; exit 2; }
    rm -rf bento.app
    mkdir -p bento.app/Contents/MacOS bento.app/Contents/Resources
    cp bento-gui bento.app/Contents/MacOS/bento
    cp bento.icns bento.app/Contents/Resources/bento.icns
    cat > bento.app/Contents/Info.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
 "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Bento</string>
  <key>CFBundleDisplayName</key><string>Bento</string>
  <key>CFBundleIdentifier</key><string>dev.bento.editor</string>
  <key>CFBundleVersion</key><string>VERSION_PLACEHOLDER</string>
  <key>CFBundleShortVersionString</key><string>VERSION_PLACEHOLDER</string>
  <key>CFBundleExecutable</key><string>bento</string>
  <key>CFBundleIconFile</key><string>bento.icns</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
    sed "s/VERSION_PLACEHOLDER/$VERSION/g" bento.app/Contents/Info.plist > bento.app/Contents/Info.plist.tmp
    mv bento.app/Contents/Info.plist.tmp bento.app/Contents/Info.plist
    echo "wrote bento.app (drag to /Applications)"
    ;;
  *)
    echo "Usage: $0 [terminal|raylib|test|icons|bundle-mac]" >&2
    exit 2
    ;;
esac
