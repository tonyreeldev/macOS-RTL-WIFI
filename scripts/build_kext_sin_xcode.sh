#!/bin/bash
# Compila macOS-RTL-WIFI.kext sin Xcode (Command Line Tools), replicando el target Release del pbxproj.
# Uso: scripts/build_kext_sin_xcode.sh <carpeta-de-salida>. Validado el 24/11/2026: compilando el código original da el mismo código y símbolos que el kext oficial 1.1.2.
set -euo pipefail
ROOT="$HOME/Documents/macOS-RTL-WIFI"
OUT="$1"; mkdir -p "$OUT/obj"
SDK="$(xcrun --sdk macosx --show-sdk-path)"; K="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
CCKEXT="$(clang -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a"
cd "$ROOT"
SRCS=$(python3 - <<'PY'
import re,os
s=open('macOS-RTL-WIFI.xcodeproj/project.pbxproj').read()
names=sorted(set(re.findall(r'\w+ /\* (\S+\.(?:c|cpp)) in Sources \*/',s)))
paths=[]
for n in names:
    hits=[os.path.join(d,n) for d,_,f in os.walk('.') if n in f and '.git' not in d and 'Build' not in d]
    if len(hits)!=1: raise SystemExit(f"ambiguo/no encontrado: {n} {hits}")
    paths.append(hits[0])
print('\n'.join(paths))
PY
)
COMMON=(-arch x86_64 -mmacosx-version-min=11.0 -isysroot "$SDK" -I"$K" -Os -mkernel -fno-common
  -fvisibility=hidden -DKERNEL=1 -DKERNEL_PRIVATE=1 -DDRIVER_PRIVATE=1 -DIEEE80211_STA_ONLY=1 -D__PRIVATE_SPI__=1
  -IStack/itl80211/openbsd -IStack/itl80211/linux -IStack/itl80211 -IHeaders -IDriver -ICore/rtl8814b/halmac -IStack/rtwn)
CFLAGS=(-std=c11 -fapple-kext)
CXXFLAGS=(-std=gnu++14 -fapple-kext -nostdinc++ -isystem libs/kstl -fno-builtin -fno-exceptions -fno-rtti -fvisibility-inlines-hidden
  -Wno-deprecated-register -Wno-c++17-extensions -Wno-macro-redefined -Wno-format -Werror=return-type)
OBJS=(); n=0
for f in $SRCS; do
  o="$OUT/obj/$(echo "$f" | sed 's#^\./##; s#/#_#g').o"
  case "$f" in
    *.cpp) clang++ "${COMMON[@]}" "${CXXFLAGS[@]}" -c "$f" -o "$o" 2>>"$OUT/warnings.log" ;;
    *.c)   clang   "${COMMON[@]}" "${CFLAGS[@]}"   -c "$f" -o "$o" 2>>"$OUT/warnings.log" ;;
  esac
  OBJS+=("$o"); n=$((n+1))
done
echo "compilados: $n"
VER=1.0.0   # MODULE_VERSION del target en el pbxproj (igual que el kext oficial)
cat > "$OUT/obj/info.c" <<C
#include <mach/mach_types.h>
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.rtlwifi.macos.driver, "$VER", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
C
clang "${COMMON[@]}" -c "$OUT/obj/info.c" -o "$OUT/obj/info.o"
B="$OUT/macOS-RTL-WIFI.kext/Contents"; mkdir -p "$B/MacOS"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -isysroot "$SDK" -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
  "$OUT/obj/info.o" "${OBJS[@]}" -lkmodc++ -lkmod "$CCKEXT" -o "$B/MacOS/macOS-RTL-WIFI"
sed 's/\$(PRODUCT_NAME)/macOS-RTL-WIFI/g' Driver/Info.plist > "$B/Info.plist"
plutil -insert CFBundleSupportedPlatforms -json '["MacOSX"]' "$B/Info.plist"
plutil -insert LSMinimumSystemVersion -string 11.0 "$B/Info.plist"
plutil -lint "$B/Info.plist" >/dev/null
echo "OK: $OUT/macOS-RTL-WIFI.kext"
