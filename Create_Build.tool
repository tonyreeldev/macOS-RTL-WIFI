#!/bin/bash
# Create_Build.tool — build macOS-RTL-WIFI: kext + installer package.
# This tree's own command, written fresh.
#
#   ./Create_Build.tool           release build + Wireless USB installer
#   ./Create_Build.tool debug     debug build + Wireless USB installer
#   ./Create_Build.tool -v        show full compiler output
#
# Needs only Xcode on the machine — no downloads, no external SDK.
# The packaging phase after the kext build uses pkgbuild/pkgutil,
# which ship with macOS.
set -euo pipefail
PARENTDIR=$(dirname "$0")
cd "$PARENTDIR"

APP_NAME="RTLWiFiBar"
SRC_DIR="./App/RTLWiFiBar"
BUILD_DIR="./Build/App"
BUNDLE="$BUILD_DIR/$APP_NAME.app"

P=macOS-RTL-WIFI.xcodeproj
T=macOS-RTL-WIFI
C=Release
VERBOSE=0
for a in "$@"; do
  case "$a" in
    debug)    C=Debug ;;
    -v|--verbose) VERBOSE=1 ;;
  esac
done
# SYMROOT is the build ROOT: xcodebuild appends $(CONFIGURATION) itself
# (TARGET_BUILD_DIR = $(SYMROOT)/$(CONFIGURATION)), so SYMROOT=Build puts the
# kext exactly at Build/Release/macOS-RTL-WIFI.kext.
S=Build
B=Build/$C

echo "[1/3] xcodebuild $C"
rm -rf "$S"
mkdir -p "$S"
if [ "$VERBOSE" = 1 ]; then
  xcodebuild -project "$P" -target "$T" -configuration "$C" build SYMROOT="$S" OBJROOT="$S/obj"
else
  R=$(xcodebuild -project "$P" -target "$T" -configuration "$C" build SYMROOT="$S" OBJROOT="$S/obj" 2>&1) || {
    printf '%s\n' "$R" | grep -E 'error:' || printf '%s\n' "$R" | tail -15
    exit 1
  }
fi

K="$B/$T.kext"
echo "[2/3] $K"
[ -d "$K" ] || K="$(find Build -type d -name "$T.kext" 2>/dev/null | head -n 1 || true)"
[ -n "$K" ] && [ -d "$K" ] || { echo "no kext produced"; exit 1; }
[ -s "$K/Contents/MacOS/$T" ] || { echo "no binary in kext Contents/MacOS"; exit 1; }
echo "ok"

# [3/3] Build the menu bar app and zip it for the statusbarapp payload.
# The app build is inline here (Build_App.tool is retired); the SDK is
# resolved below and the build fails loudly if the app cannot be produced -
# the package must never ship a stale app zip.
echo "[3/3] RTLWiFiBar.app"
# ---------------------------------------------------------------- SDK resolve
SDK="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
  SDK=""
  for CAND in \
    "$(xcodebuild -sdk macosx -version Path 2>/dev/null || true)" \
    /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk \
    /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk \
    /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk \
    /Library/Developer/CommandLineTools/SDKs/MacOSX*.sdk; do
    [ -n "$CAND" ] && [ -d "$CAND" ] && { SDK="$CAND"; break; }
  done
fi
if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
  echo "error: no macOS SDK found (install Xcode or the command-line tools)" >&2
  exit 1
fi
echo "[app] SDK: $SDK"

CLANG="$(xcrun -f clang 2>/dev/null || command -v clang)"
echo "[app] clang: $CLANG"

# ---------------------------------------------------------------- flags
ARCHS="-arch x86_64 -arch arm64"   # universal; Intel is the primary target
MIN="-mmacosx-version-min=11.0"
FLAGS=(-fobjc-arc -fno-modules -Wall -Wno-deprecated-declarations
       $ARCHS $MIN -isysroot "$SDK" -I"$SRC_DIR")
FRAMEWORKS="-framework Cocoa -framework IOKit -framework Security -framework SystemConfiguration"

SOURCES=(main.m AppDelegate.m DriverClient.m ProfileStore.m RTWLog.m SignalIcon.m RTWCrashReporter.m)

# ---------------------------------------------------------------- compile
rm -rf "$BUILD_DIR"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
OBJS=()
for SRC in "${SOURCES[@]}"; do
  OBJ="$BUILD_DIR/${SRC%.m}.o"
  echo "[app] compile $SRC"
  "$CLANG" "${FLAGS[@]}" -c "$SRC_DIR/$SRC" -o "$OBJ"
  OBJS+=("$OBJ")
done

# ---------------------------------------------------------------- link
echo "[app] link $APP_NAME"
"$CLANG" "${FLAGS[@]}" $FRAMEWORKS -o "$BUNDLE/Contents/MacOS/$APP_NAME" "${OBJS[@]}"

# ---------------------------------------------------------------- bundle
cp -f "$SRC_DIR/Info.plist" "$BUNDLE/Contents/Info.plist"
cp -f "$SRC_DIR/$APP_NAME.icns" "$BUNDLE/Contents/Resources/$APP_NAME.icns"
# v8.8.0: UI translations (one <lang>.lproj/Localizable.strings per language).
for LPROJ in "$SRC_DIR"/*.lproj; do
  [ -d "$LPROJ" ] && cp -R "$LPROJ" "$BUNDLE/Contents/Resources/"
done
# v8.7.8: enable-boot.sh no longer ships inside the app - "Enable Driver at
# Boot" and its osascript setup were removed; the PKG installer owns boot
# loading.

# Ad-hoc sign so Gatekeeper/amfid accept the binary locally. Optional:
# unsigned binaries also load, but signing costs nothing here.
codesign --force --sign - "$BUNDLE" 2>/dev/null || echo "[app] codesign unavailable - skipped"

echo "[app] built: $BUNDLE"
ls -la "$BUNDLE/Contents/MacOS/$APP_NAME"

mkdir -p ./PKG/statusbarapp
rm -f ./PKG/statusbarapp/RTLWiFiBar.zip
( cd ./Build/App && ditto -c -k --keepParent RTLWiFiBar.app ./../../PKG/statusbarapp/RTLWiFiBar.zip ) 2>/dev/null \
  || ( cd ./Build/App && zip -r -q -y ../../PKG/statusbarapp/RTLWiFiBar.zip RTLWiFiBar.app )
[ -s ./PKG/statusbarapp/RTLWiFiBar.zip ] || { echo "error: RTLWiFiBar.zip was not produced"; exit 1; }
echo "RTLWiFiBar.zip ready ($(du -h ./PKG/statusbarapp/RTLWiFiBar.zip | cut -f1))"

echo "Create Package"
echo "= = = = = = = = = = = = = = = = = = = = = = = = =  "
echo "Wireless USB macOS-RTL-WIFI "
echo "= = = = = = = = = = = = = = = = = = = = = = = = =  "
sleep 1
# Remove old build
rm -rf ./InstallerWIFI
rm -rf /Private/tmp/Package-DIR
rm -rf ./*.pkg
rm -rf ./PKG/efifolderclover
rm -rf ./PKG/efifolderoc
sleep 1
# Create necessary folder
mkdir -p ./InstallerWIFI
mkdir -p ./InstallerWIFI/BUILD-PACKAGE
mkdir -p /Private/tmp/Package-DIR
mkdir -p ./PKG/uninstall
mkdir -p ./PKG/statusbarapp
mkdir -p ./PKG/efifolderclover/EFI/CLOVER/kexts/Other
mkdir -p ./PKG/efifolderoc/EFI/OC/Kexts
sleep 1
# Copy kext to bootloader
cp -Rp ./Build/Release/macOS-RTL-WIFI.kext ./PKG/efifolderclover/EFI/CLOVER/kexts/Other
cp -Rp ./Build/Release/macOS-RTL-WIFI.kext ./PKG/efifolderoc/EFI/OC/Kexts
sleep 1
# Create the Packages with pkgbuild
pkgbuild --root ./PKG/statusbarapp --scripts ./PKG/Script/"script statusbarapp" --identifier com.109Driver.WirelessUSBmacOS-RTL-WIFI.statusbarapp.pkg --version 1 --install-location /Private/tmp/ ./InstallerWIFI/BUILD-PACKAGE/statusbarapp.pkg
pkgbuild --root ./PKG/agents --identifier com.109Driver.WirelessUSBmacOS-RTL-WIFI.Agents.pkg --version 1 --install-location /Library/LaunchAgents ./InstallerWIFI/BUILD-PACKAGE/agents.pkg
pkgbuild --root ./PKG/efifolderoc --scripts ./PKG/Script/"script oc" --identifier com.109Driver.WirelessUSBmacOS-RTL-WIFI.EFIFOLDEROC.pkg --version 1 --install-location /Private/tmp ./InstallerWIFI/BUILD-PACKAGE/efifolderoc.pkg
pkgbuild --root ./PKG/efifolderclover --scripts ./PKG/Script/"script clover" --identifier com.109Driver.WirelessUSBmacOS-RTL-WIFI.EFIFOLDERCLOVER.pkg --version 1 --install-location /Private/tmp ./InstallerWIFI/BUILD-PACKAGE/efifolderclover.pkg
pkgbuild --root ./PKG/uninstall --scripts ./PKG/Script/"script uninstall" --identifier com.109Driver.WirelessUSBmacOS-RTL-WIFI.uninstall.pkg --version 1 --install-location / ./InstallerWIFI/BUILD-PACKAGE/uninstall.pkg
sleep 2
# Expend the Packages with pkgutil
pkgutil --expand ./InstallerWIFI/BUILD-PACKAGE/statusbarapp.pkg /tmp/Package-DIR/statusbarapp.pkg
pkgutil --expand ./InstallerWIFI/BUILD-PACKAGE/agents.pkg /tmp/Package-DIR/agents.pkg
pkgutil --expand ./InstallerWIFI/BUILD-PACKAGE/efifolderoc.pkg /tmp/Package-DIR/efifolderoc.pkg
pkgutil --expand ./InstallerWIFI/BUILD-PACKAGE/efifolderclover.pkg /tmp/Package-DIR/efifolderclover.pkg
pkgutil --expand ./InstallerWIFI/BUILD-PACKAGE/uninstall.pkg /tmp/Package-DIR/uninstall.pkg
echo "
= = = = = = = = = = = = = = = = = = = = = = = = =
Create the final Packages with Productbuild "
sleep 2
# Copy resources and distribution
cp -r ./PKG/Resources/Distribution /tmp/Package-DIR
cp -r ./PKG/Resources/Resources /tmp/Package-DIR
sleep 1
# Flatten the Packages with pkgutil
pkgutil --flatten /Private/tmp/Package-DIR ./Build/Release/"Wireless USB macOS-RTL-WIFI.pkg"
sleep 1
# Copy the PayPal donation page next to the package
mkdir -p ./Build/Release
if [ -d "./Build/App/RTLWiFiBar.app" ]; then
  rm -rf ./Build/Release/RTLWiFiBar.app
  cp -Rp "./Build/App/RTLWiFiBar.app" ./Build/Release/
  echo "RTLWiFiBar copied to ./Build/Release"
fi
if [ -f "./Support Donate.html" ]; then
  cp -f "./Support Donate.html" ./Build/Release/
  echo "Support Donate.html copied to ./Build/Release"
fi
# License notices that must accompany the distribution
for f in THIRD-PARTY-NOTICES.md LICENSE-GPL-2.0.txt; do
  [ -f "./$f" ] && cp -f "./$f" ./Build/Release/
done
# Set the PayPal custom icon on the donation page (osxutils seticon)
if [ -f "./Build/Release/Support Donate.html" ] && [ -x "./PKG/script/SETICON/seticon" ] && [ -f "./PKG/script/SETICON/Paypal.icns" ]; then
  ./PKG/script/SETICON/seticon -d ./PKG/script/SETICON/Paypal.icns ./Build/Release/"Support Donate.html"
  echo "PayPal custom icon set on ./Build/Release/Support Donate.html"
fi
echo "= = = = = = = = = = = = = = = = = = = = = = = = =  "
echo "Wireless USB macOS-RTL-WIFI.pkg
Completed Done!   "
echo "= = = = = = = = = = = = = = = = = = = = = = = = =  "
# Remove old build
rm -rf ./InstallerWIFI
rm -rf ./PKG/efifolderclover
rm -rf ./PKG/efifolderoc
# Open release (guarded: headless CI runners have no GUI session)
open ./Build/Release 2>/dev/null || true

