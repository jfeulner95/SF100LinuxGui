#!/bin/bash
# Make build/gui/sf100gui.app self-contained (no Homebrew/Qt needed on the target Mac) and pack a DMG.
# Usage: ci/macos-deploy.sh <app> <dmg>
set -euo pipefail
APP=$1
DMG=$2
BREW=$(brew --prefix)
QT=$(brew --prefix qt)
FW="$APP/Contents/Frameworks"
PLUGINS="$APP/Contents/PlugIns"

cp build/dpcmd "$APP/Contents/MacOS/"
cp LICENSE "$APP/Contents/Resources/"

# Only the plugins the app uses. Deploying all of Homebrew's plugins (pdf, svg, webp, virtual keyboard)
# pulls in dependencies macdeployqt cannot resolve.
QT_PLUGINS=$("$QT/bin/qmake" -query QT_INSTALL_PLUGINS 2>/dev/null || "$QT/bin/qtpaths" --plugin-dir)
mkdir -p "$PLUGINS/platforms" "$PLUGINS/styles"
cp "$QT_PLUGINS/platforms/libqcocoa.dylib" "$PLUGINS/platforms/"
cp "$QT_PLUGINS/styles/libqmacstyle.dylib" "$PLUGINS/styles/"
chmod u+w "$PLUGINS"/*/*.dylib
printf '[Paths]\nPlugins = PlugIns\n' > "$APP/Contents/Resources/qt.conf"

"$QT/bin/macdeployqt" "$APP" -no-plugins -always-overwrite \
    -executable="$APP/Contents/MacOS/dpcmd" \
    -executable="$PLUGINS/platforms/libqcocoa.dylib" \
    -executable="$PLUGINS/styles/libqmacstyle.dylib"

# Safety net: copy any library still referenced from Homebrew into the bundle and rewrite the reference.
machos() { find "$APP" -type f \( -perm +111 -o -name '*.dylib' \) -print0; }
changed=1
while [ $changed = 1 ]; do
    changed=0
    while IFS= read -r -d '' bin; do
        file -b "$bin" | grep -q Mach-O || continue
        for dep in $(otool -L "$bin" | tail -n +2 | awk '{print $1}' | grep -E "^($BREW|/opt/homebrew|/usr/local)/" || true); do
            name=$(basename "$dep")
            if [ ! -f "$FW/$name" ]; then
                cp -L "$dep" "$FW/$name"
                chmod u+w "$FW/$name"
                install_name_tool -id "@rpath/$name" "$FW/$name"
                changed=1
            fi
            install_name_tool -change "$dep" "@executable_path/../Frameworks/$name" "$bin"
        done
    done < <(machos)
done

# install_name_tool invalidated the signatures; Apple Silicon refuses unsigned code, so sign ad hoc.
codesign --force --deep --sign - "$APP"

if machos | xargs -0 otool -L 2>/dev/null | grep -E "^[[:space:]]+($BREW|/opt/homebrew|/usr/local)/"; then
    echo "::error::bundle still references Homebrew libraries"
    exit 1
fi

# Real test: hide Homebrew's Cellar, then start the bundled app and CLI.
sudo mv "$BREW/Cellar" "$BREW/Cellar.hidden"
trap 'sudo mv "$BREW/Cellar.hidden" "$BREW/Cellar"' EXIT
"$APP/Contents/MacOS/sf100gui" --smoke
"$APP/Contents/MacOS/dpcmd" > /dev/null
sudo mv "$BREW/Cellar.hidden" "$BREW/Cellar"
trap - EXIT

mkdir -p "$(dirname "$DMG")"
hdiutil create -volname "SF100 GUI" -srcfolder "$APP" -ov -format UDZO "$DMG"
