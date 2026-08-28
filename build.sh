#!/usr/bin/env bash
#
# AOAS build / install / debug driver.
#
# There is no Gradle wrapper in this tree on purpose: adding one means checking
# in a jar, and this project has exactly one developer with a system Gradle. If
# that ever stops being true, `gradle wrapper` here and switch GRADLE below.
#
#   ./build.sh                 build the debug APK
#   ./build.sh install         build, then install on the connected device
#   ./build.sh run             install, then launch the debug UI
#   ./build.sh logs            follow AOAS's logcat (native tag included)
#   ./build.sh test            run the desktop shm_ring test (ASan + UBSan)
#   ./build.sh clean           delete build outputs
#   ./build.sh release         build the release APK (unsigned)
#
# Everything after the subcommand is passed straight to Gradle, so
# `./build.sh install --info` works.

set -euo pipefail

cd "$(dirname "$0")"
ROOT="$PWD"

GRADLE="${GRADLE:-gradle}"
ADB="${ADB:-adb}"
PKG=io.nava.aoas
APK="$ROOT/app/build/outputs/apk/debug/app-debug.apk"

# The SDK carries the NDK and CMake this build pins; without it Gradle guesses.
export ANDROID_HOME="${ANDROID_HOME:-/opt/android-sdk}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\033[1;31m!! %s\033[0m\n' "$*" >&2; exit 1; }

need_device() {
    local n
    n=$("$ADB" devices | awk 'NR>1 && $2=="device"' | wc -l)
    [ "$n" -ge 1 ] || die "no device in 'adb devices' -- plug the phone in (or adb connect it)"
    [ "$n" -eq 1 ] || die "$n devices attached; set ANDROID_SERIAL to pick one"
}

cmd_build() {
    say "assembling debug APK"
    "$GRADLE" :app:assembleDebug "$@"
    say "APK: $APK"
}

cmd_release() {
    say "assembling release APK (unsigned)"
    "$GRADLE" :app:assembleRelease "$@"
}

cmd_install() {
    cmd_build "$@"
    need_device
    say "installing"
    # -r reinstall, -g pre-grant runtime permissions (POST_NOTIFICATIONS), so a
    # bring-up run does not stop on a dialog.
    "$ADB" install -r -g "$APK"
}

cmd_run() {
    cmd_install "$@"
    say "launching the debug UI"
    "$ADB" shell am start -n "$PKG/.AoasDebugActivity" >/dev/null
    say "following logcat (ctrl-c to stop)"
    cmd_logs
}

cmd_logs() {
    need_device
    "$ADB" logcat -v time AOAS:V AoasDebug:V UsbAudio:V AndroidRuntime:E '*:S'
}

cmd_test() {
    say "shm_ring test (desktop, ASan + UBSan)"
    local out="$ROOT/build/host-tests"
    mkdir -p "$out"
    # Host build on purpose: shm_ring.hh is plain C++ over a mapped region, so
    # the bit-exactness gate does not need a phone and the sanitizers are worth
    # more than the realism would be.
    c++ -std=c++17 -O1 -g -Wall -Wextra -Wconversion \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$ROOT/native" \
        "$ROOT/native/tests/shm_ring_test.cc" -o "$out/shm_ring_test"
    "$out/shm_ring_test"
}

cmd_clean() {
    say "cleaning"
    "$GRADLE" clean "$@" || true
    rm -rf "$ROOT/build" "$ROOT/app/build" "$ROOT/.cxx" "$ROOT/app/.cxx"
}

case "${1:-build}" in
    build)   shift || true; cmd_build "$@" ;;
    release) shift; cmd_release "$@" ;;
    install) shift; cmd_install "$@" ;;
    run)     shift; cmd_run "$@" ;;
    logs)    shift; cmd_logs ;;
    test)    shift; cmd_test ;;
    clean)   shift; cmd_clean "$@" ;;
    *)       die "unknown subcommand '$1' -- see the header of $0" ;;
esac
