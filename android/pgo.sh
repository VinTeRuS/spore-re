#!/bin/bash
# Profile-guided optimization of libmain.so, second step.
#   1. make -C src/native PLATFORM=android PGO=gen OUT=../../build/android-pgo-gen/arm64-v8a
#      ANDROID_BUILD=$PWD/build/android-pgo-gen APK_NAME=spore-pgo-gen.apk android/build-apk.sh
#      install build/apk/spore-pgo-gen.apk, play every stage for a while (the counters are written every minute)
#   2. android/pgo.sh [adb serial]: pulls the .profraw files, merges them into build/pgo/spore.profdata and builds
#      the optimized libmain.so (build/android) and APK (build/apk/spore.apk).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
PROFDATA=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-profdata
ADB=(adb)
[ $# -gt 0 ] && ADB=(adb -s "$1")
mkdir -p "$ROOT/build/pgo"
rm -f "$ROOT/build/pgo"/*.profraw
for f in $("${ADB[@]}" shell 'ls /sdcard/Android/data/com.sporere.game/files/ | grep "^spore-pgo-.*\.profraw$"'); do
    "${ADB[@]}" pull "/sdcard/Android/data/com.sporere.game/files/$f" "$ROOT/build/pgo/"
done
ls "$ROOT/build/pgo"/*.profraw >/dev/null
"$PROFDATA" merge -o "$ROOT/build/pgo/spore.profdata" "$ROOT/build/pgo"/*.profraw
"$PROFDATA" show "$ROOT/build/pgo/spore.profdata" | tail -3
# every object depends on the profile: rebuild all of them
rm -rf "$ROOT/build/android/arm64-v8a"
make -C "$ROOT/src/native" -j"${JOBS:-3}" PLATFORM=android PGO="$ROOT/build/pgo/spore.profdata"
"$ROOT/android/build-apk.sh"
