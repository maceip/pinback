#!/usr/bin/env bash
# Functional smoke tests for platform shells + optional backend deep smoke.
#
# Usage:
#   platform/smoke-test.sh [targets...]
#
# Targets (default: all):
#   backend   make test + pinback-server /healthz
#   macos     PinbackShell + Pinback.app self-host
#   ios       Simulator launch against host server
#   linux     Orb VM: GTK shell self-host + PINBACK_URL override
#   windows   SSH: remote PINBACK_URL shell on Windows host
#   android   SSH: Gradle APK build + artifact verify (+ optional runtime)
#   all       every target above
#
# Environment:
#   WINDOWS_HOST              SSH target (default: mac@192.168.0.180)
#   WINDOWS_PINBACK_ROOT        Repo path on Windows (default: C:/Users/mac/pinback)
#   ORB_MACHINE                 Orb Linux VM (default: webkitium-ci)
#   MAC_LAN_IP                  LAN IP for Windows→Mac (auto: en0)
#   LINUX_SERVER_BUILD          Isolated Linux ELF build dir in Orb (default: /tmp/pinback-linux-test)
#   LINUX_STARTUP_TIMEOUT       Seconds to wait for Linux self-host (default: 35)
#   SMOKE_ANDROID_RUNTIME=1     Install/launch APK on adb device + WSL server
#   SMOKE_DEEP=1                Run scripts/qa/pinback-smoke after macOS self-host
#   SMOKE_SKIP_BUILD=1          Skip remote Gradle build (verify existing APK only)
#
# Pitfall: never run `make clean` inside Orb against the shared /Users/mac mount.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM="$(cd "$(dirname "$0")" && pwd)"

WINDOWS_HOST="${WINDOWS_HOST:-mac@192.168.0.180}"
WINDOWS_PINBACK_ROOT="${WINDOWS_PINBACK_ROOT:-C:/Users/mac/pinback}"
ORB_MACHINE="${ORB_MACHINE:-webkitium-ci}"
MAC_LAN_IP="${MAC_LAN_IP:-$(ipconfig getifaddr en0 2>/dev/null || true)}"
LINUX_SERVER_BUILD="${LINUX_SERVER_BUILD:-/tmp/pinback-linux-test}"
LINUX_STARTUP_TIMEOUT="${LINUX_STARTUP_TIMEOUT:-35}"
PINBACK_PORT="${PINBACK_PORT:-8088}"

IOS_BUNDLE_ID="${IOS_BUNDLE_ID:-dev.pinback.shell}"
IOS_SIM_DEVICE="${IOS_SIM_DEVICE:-iPhone 17}"
ANDROID_PACKAGE="${ANDROID_PACKAGE:-com.pinback.shell}"

BOLD=$'\033[1m'; RED=$'\033[31m'; GREEN=$'\033[32m'; YELLOW=$'\033[33m'; OFF=$'\033[0m'
FAILURES=0
WARNINGS=0

section() { printf '\n%s=== %s ===%s\n' "$BOLD" "$*" "$OFF"; }
pass()    { printf '%sPASS%s  %s\n' "$GREEN" "$OFF" "$*"; }
fail()    { printf '%sFAIL%s  %s\n' "$RED" "$OFF" "$*"; FAILURES=$((FAILURES + 1)); }
warn()    { printf '%sWARN%s  %s\n' "$YELLOW" "$OFF" "$*"; WARNINGS=$((WARNINGS + 1)); }
skip()    { printf '%sSKIP%s  %s\n' "$YELLOW" "$OFF" "$*"; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || { fail "missing required command: $1"; return 1; }
}

kill_local_pinback() {
    killall PinbackShell Pinback pinback-server 2>/dev/null || true
}

wait_http() {
    local url="$1" timeout="${2:-15}" label="${3:-$1}"
    local i code
    for i in $(seq 1 "$timeout"); do
        code=$(curl -s -o /dev/null -w '%{http_code}' "$url" 2>/dev/null || echo 000)
        if [ "$code" = "200" ]; then
            pass "$label (${i}s)"
            return 0
        fi
        sleep 1
    done
    fail "$label (no HTTP 200 within ${timeout}s, last=$code)"
    return 1
}

ensure_pinback_server() {
    if [ ! -x "$ROOT/build/pinback-server" ]; then
        make -C "$ROOT" pinback-server >/dev/null || { fail "make pinback-server"; return 1; }
    fi
    pass "pinback-server binary present"
}

ensure_macos_shell() {
    if [ ! -x "$PLATFORM/macos/.build/release/PinbackShell" ]; then
        "$PLATFORM/macos/build.sh" nobundle >/dev/null 2>&1 || "$PLATFORM/macos/build.sh" >/dev/null || {
            fail "macos shell build"
            return 1
        }
    fi
    pass "macOS PinbackShell built"
}

ensure_linux_shell() {
    if ! orb -m "$ORB_MACHINE" test -x /Users/mac/pinback/platform/linux/build/pinback-shell 2>/dev/null; then
        orb -m "$ORB_MACHINE" bash -lc \
            'cd /Users/mac/pinback/platform/linux && meson setup build 2>/dev/null || true && meson compile -C build' \
            >/dev/null || { fail "linux shell build in Orb"; return 1; }
    fi
    pass "Linux pinback-shell built in Orb"
}

ensure_linux_server_elf() {
    # Build pinback-server as Linux ELF in an isolated dir — never on the shared Mac mount.
    orb -m "$ORB_MACHINE" bash -lc "
        set -euo pipefail
        BUILD='$LINUX_SERVER_BUILD'
        MARKER=\"\$BUILD/.pinback-server.ok\"
        SERVER=\"\$BUILD/build/pinback-server\"
        if [ -f \"\$MARKER\" ] && [ -x \"\$SERVER\" ]; then exit 0; fi
        rm -rf \"\$BUILD\"
        mkdir -p \"\$BUILD/src\" \"\$BUILD/ui\" \"\$BUILD/scripts/embed\" \"\$BUILD/tests/support\"
        cp '$ROOT/Makefile' \"\$BUILD/\"
        cp '$ROOT'/src/*.c '$ROOT'/src/*.h \"\$BUILD/src/\"
        cp -R '$ROOT/ui/app' \"\$BUILD/ui/\"
        cp '$ROOT/scripts/embed/gen-static-assets.sh' \"\$BUILD/scripts/embed/\"
        cp '$ROOT/tests/support/fake-ds4-agent.c' \"\$BUILD/tests/support/\"
        rm -f \"\$BUILD/src/\"*.o
        cd \"\$BUILD\"
        make pinback-server CC=cc >/dev/null
        touch \"\$MARKER\"
    " || { fail "linux ELF pinback-server build in Orb"; return 1; }
    pass "Linux ELF pinback-server ready in Orb"
}

ssh_windows() {
    ssh -o BatchMode=yes -o ConnectTimeout=15 "$WINDOWS_HOST" "$@"
}

test_backend() {
    section "backend"
    need_cmd make || return 1
    need_cmd curl || return 1
    if make -C "$ROOT" test >/dev/null; then pass "make test (5 suites)"
    else fail "make test"; return 1; fi

    ensure_pinback_server || return 1
    kill_local_pinback
    "$ROOT/build/pinback-server" --bind "127.0.0.1:$PINBACK_PORT" --quiet &
    local srv=$!
    sleep 1
    if curl -sf "http://127.0.0.1:$PINBACK_PORT/healthz" | grep -q ok; then
        pass "/healthz on loopback"
    else
        fail "/healthz on loopback"
    fi
    kill "$srv" 2>/dev/null || true
    wait "$srv" 2>/dev/null || true
}

test_macos() {
    section "macos"
    need_cmd curl || return 1
    ensure_pinback_server || return 1
    ensure_macos_shell || return 1

    kill_local_pinback
    sleep 1

    # Binary self-host
    PINBACK_SERVER_BIN="$ROOT/build/pinback-server" "$PLATFORM/macos/.build/release/PinbackShell" \
        >/tmp/pinback-smoke-macos-bin.log 2>&1 &
    local sp=$!
    wait_http "http://127.0.0.1:$PINBACK_PORT/healthz" 15 "macos PinbackShell self-host /healthz" || true
    wait_http "http://127.0.0.1:$PINBACK_PORT/" 5 "macos PinbackShell UI /" || true
    kill "$sp" 2>/dev/null || killall PinbackShell 2>/dev/null || true
    sleep 1

    if [ "${SMOKE_DEEP:-0}" = "1" ]; then
        PINBACK_SERVER_BIN="$ROOT/build/pinback-server" "$PLATFORM/macos/.build/release/PinbackShell" \
            >/tmp/pinback-smoke-macos-deep.log 2>&1 &
        sp=$!
        if wait_http "http://127.0.0.1:$PINBACK_PORT/healthz" 15 "deep smoke: server up"; then
            if "$ROOT/scripts/qa/pinback-smoke" "http://127.0.0.1:$PINBACK_PORT"; then
                pass "scripts/qa/pinback-smoke against self-hosted shell"
            else
                fail "scripts/qa/pinback-smoke against self-hosted shell"
            fi
        fi
        kill "$sp" 2>/dev/null || killall PinbackShell 2>/dev/null || true
        sleep 1
    fi

    # App bundle
    if [ ! -d "$PLATFORM/macos/Pinback.app" ]; then
        "$PLATFORM/macos/build.sh" >/dev/null || { fail "Pinback.app bundle"; return 1; }
    fi
    kill_local_pinback
    open -a "$PLATFORM/macos/Pinback.app" >/dev/null 2>&1 || true
    wait_http "http://127.0.0.1:$PINBACK_PORT/healthz" 15 "macos Pinback.app self-host /healthz" || true
    wait_http "http://127.0.0.1:$PINBACK_PORT/" 5 "macos Pinback.app UI /" || true
    killall Pinback 2>/dev/null || true
    sleep 1
    pgrep -f "pinback-server.*$PINBACK_PORT" >/dev/null && warn "macos: pinback-server still running after shell exit"
}

test_ios() {
    section "ios"
    need_cmd xcrun || return 1
    need_cmd curl || return 1

    local ios_app
    ios_app=$(find "$HOME/Library/Developer/Xcode/DerivedData" -path '*/Debug-iphonesimulator/Pinback.app' -type d 2>/dev/null | head -1)
    if [ -z "$ios_app" ]; then
        "$PLATFORM/ios/build.sh" >/dev/null || { fail "ios simulator build"; return 1; }
        ios_app=$(find "$HOME/Library/Developer/Xcode/DerivedData" -path '*/Debug-iphonesimulator/Pinback.app' -type d 2>/dev/null | head -1)
    fi
    if [ ! -d "$ios_app" ]; then fail "ios Pinback.app not found in DerivedData"; return 1; fi
    pass "ios simulator app: $ios_app"

    ensure_pinback_server || return 1
    kill_local_pinback
    "$ROOT/build/pinback-server" --bind "127.0.0.1:$PINBACK_PORT" --quiet &
    local srv=$!
    sleep 1

    local udid
    udid=$(xcrun simctl list devices available | awk -F '[()]' -v name="$IOS_SIM_DEVICE" '$0 ~ name { print $2; exit }')
    if [ -z "$udid" ]; then fail "ios simulator device not found: $IOS_SIM_DEVICE"; kill "$srv" 2>/dev/null; return 1; fi

    xcrun simctl boot "$udid" 2>/dev/null || true
    xcrun simctl install "$udid" "$ios_app" >/dev/null || { fail "ios simctl install"; kill "$srv" 2>/dev/null; return 1; }
    if xcrun simctl launch "$udid" "$IOS_BUNDLE_ID" >/tmp/pinback-smoke-ios-launch.log 2>&1; then
        pass "ios simctl launch $IOS_BUNDLE_ID"
    else
        fail "ios simctl launch $IOS_BUNDLE_ID"; cat /tmp/pinback-smoke-ios-launch.log
    fi
    sleep 3
    curl -sf "http://127.0.0.1:$PINBACK_PORT/healthz" >/dev/null && pass "ios backend still healthy after launch" \
        || fail "ios backend not reachable after launch"

    xcrun simctl terminate "$udid" "$IOS_BUNDLE_ID" 2>/dev/null || true
    kill "$srv" 2>/dev/null || true
    wait "$srv" 2>/dev/null || true
}

test_linux() {
    section "linux (Orb: $ORB_MACHINE)"
    need_cmd orb || { skip "linux (orb CLI not installed)"; return 0; }
    ensure_linux_shell || return 1
    ensure_linux_server_elf || return 1

    orb -m "$ORB_MACHINE" bash -lc "
        set -uo pipefail
        killall -9 pinback-shell pinback-server 2>/dev/null || true
        sleep 1
        SERVER='$LINUX_SERVER_BUILD/build/pinback-server'
        cd /Users/mac/pinback/platform/linux
        env PINBACK_SERVER_BIN=\"\$SERVER\" xvfb-run -a ./build/pinback-shell &
        SP=\$!
        ok=0
        for i in \$(seq 1 $LINUX_STARTUP_TIMEOUT); do
            h=\$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PINBACK_PORT/healthz 2>/dev/null || echo 000)
            if [ \"\$h\" = 200 ]; then ok=1; echo \"SELFHOST_HEALTHZ \${i}s\"; break; fi
            sleep 1
        done
        [ \"\$ok\" = 1 ] || echo SELFHOST_HEALTHZ FAIL
        u=\$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:$PINBACK_PORT/ 2>/dev/null || echo 000)
        echo \"SELFHOST_UI \$u\"
        kill \$SP 2>/dev/null || true
        killall -9 pinback-shell pinback-server 2>/dev/null || true
        sleep 1
        \$SERVER --bind 127.0.0.1:8099 --quiet &
        SRV=\$!
        sleep 1
        env PINBACK_URL=http://127.0.0.1:8099 xvfb-run -a ./build/pinback-shell &
        SP=\$!
        sleep 6
        if ps -eo args | grep -F '127.0.0.1:8088' | grep pinback-server | grep -v grep >/dev/null; then
            echo OVERRIDE_8088 FAIL
        else
            echo OVERRIDE_8088 OK
        fi
        curl -sf http://127.0.0.1:8099/healthz >/dev/null && echo OVERRIDE_HEALTHZ OK || echo OVERRIDE_HEALTHZ FAIL
        kill \$SP \$SRV 2>/dev/null || true
        killall -9 pinback-shell pinback-server 2>/dev/null || true
    " | while read -r line; do
        case "$line" in
            SELFHOST_HEALTHZ\ FAIL) fail "linux self-host /healthz" ;;
            SELFHOST_HEALTHZ\ *) pass "linux self-host /healthz (${line#SELFHOST_HEALTHZ })" ;;
            SELFHOST_UI\ 200) pass "linux self-host UI /" ;;
            SELFHOST_UI\ *) fail "linux self-host UI / (HTTP ${line#SELFHOST_UI })" ;;
            OVERRIDE_8088\ OK) pass "linux PINBACK_URL override (no :8088 spawn)" ;;
            OVERRIDE_8088\ FAIL) fail "linux PINBACK_URL override spawned :8088 server" ;;
            OVERRIDE_HEALTHZ\ OK) pass "linux PINBACK_URL override /healthz" ;;
            OVERRIDE_HEALTHZ\ FAIL) fail "linux PINBACK_URL override /healthz" ;;
        esac
    done
}

test_windows() {
    section "windows (SSH: $WINDOWS_HOST)"
    ssh_windows "cd /d C:\\Users\\mac\\pinback && git pull --ff-only 2>nul" >/dev/null || true
    if [ -z "$MAC_LAN_IP" ]; then fail "MAC_LAN_IP not set (could not detect en0)"; return 1; fi
    pass "MAC_LAN_IP=$MAC_LAN_IP"

    ensure_pinback_server || return 1
    kill_local_pinback
    "$ROOT/build/pinback-server" --bind "0.0.0.0:$PINBACK_PORT" --quiet &
    local srv=$!
    sleep 2

    local exe='C:\Users\mac\pinback\platform\windows\build\Release\pinback-shell.exe'
    ssh_windows "powershell -NoProfile -ExecutionPolicy Bypass -File C:/Users/mac/pinback/platform/windows/smoke-test.ps1 -MacHost $MAC_LAN_IP -Port $PINBACK_PORT" \
        | while read -r line; do
        case "$line" in
            EXE_OK) pass "windows pinback-shell.exe present" ;;
            EXE_MISSING*) fail "windows pinback-shell.exe missing: $line" ;;
            REMOTE_HEALTHZ\ ok) pass "windows reaches Mac /healthz" ;;
            REMOTE_HEALTHZ\ *) fail "windows remote /healthz: $line" ;;
            SHELL_OK\ *) pass "windows shell runs with PINBACK_URL (${line#SHELL_OK })" ;;
            SHELL_CLEANUP\ OK) pass "windows shell cleanup" ;;
            SHELL_FAIL\ *) fail "windows shell exited early (${line#SHELL_FAIL })" ;;
        esac
    done

    kill "$srv" 2>/dev/null || true
    wait "$srv" 2>/dev/null || true
}

test_android() {
    section "android (SSH: $WINDOWS_HOST)"
    local win_root="${WINDOWS_PINBACK_ROOT//\\/\/}"

    # Keep Windows checkout in sync so verify/runtime scripts are present.
    ssh_windows "cd /d C:\\Users\\mac\\pinback && git pull --ff-only 2>nul" >/dev/null || true

    if [ "${SMOKE_SKIP_BUILD:-0}" != "1" ]; then
        if ssh_windows "cd /d C:\\Users\\mac\\pinback\\platform\\android && gradlew.bat assembleDebug --no-daemon" >/tmp/pinback-smoke-android-build.log 2>&1; then
            pass "android assembleDebug on Windows"
        else
            fail "android assembleDebug on Windows"; tail -20 /tmp/pinback-smoke-android-build.log
            return 1
        fi
    else
        skip "android Gradle build (SMOKE_SKIP_BUILD=1)"
    fi

    ssh_windows "powershell -NoProfile -ExecutionPolicy Bypass -File C:/Users/mac/pinback/platform/android/verify-apk.ps1" \
        | while read -r line; do
        case "$line" in
            APK_SIZE\ 0) fail "android APK empty" ;;
            APK_SIZE\ *) pass "android APK size ${line#APK_SIZE } bytes" ;;
            APK_BADGING*com.pinback.shell*) pass "android APK package com.pinback.shell" ;;
            APK_BADGING\ *) fail "android APK badging unexpected: $line" ;;
            APK_MISSING*) fail "android APK not found: $line" ;;
            AAPT_MISSING) fail "android aapt not found in SDK build-tools" ;;
            APK_PACKAGE_FAIL) fail "android APK package name mismatch" ;;
            APK_VERIFY_OK) pass "android APK verify" ;;
        esac
    done

    if [ "${SMOKE_ANDROID_RUNTIME:-0}" != "1" ]; then
        skip "android runtime (set SMOKE_ANDROID_RUNTIME=1 to install/launch on adb device)"
        return 0
    fi

    ssh_windows "powershell -NoProfile -ExecutionPolicy Bypass -File C:/Users/mac/pinback/platform/android/smoke-runtime.ps1" \
        | while read -r line; do
        case "$line" in
            ADB_MISSING) fail "android adb not found on Windows SDK" ;;
            ADB_NO_DEVICE) skip "android runtime (no adb device/emulator)" ;;
            ADB_DEVICE\ OK) pass "android adb device connected" ;;
            WSL_SERVER_OK) pass "android WSL pinback-server on :$PINBACK_PORT" ;;
            WSL_SERVER_FAIL) fail "android WSL pinback-server failed" ;;
            ADB_INSTALL_OK) pass "android adb install" ;;
            ADB_INSTALL_FAIL) fail "android adb install" ;;
            ADB_ACTIVITY_OK) pass "android MainActivity running" ;;
            ADB_ACTIVITY_FAIL) fail "android MainActivity not in foreground" ;;
        esac
    done
}

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \?//'
    exit 0
}

TARGETS=("$@")
if [ ${#TARGETS[@]} -eq 0 ]; then TARGETS=(all); fi
if [ "${TARGETS[0]}" = "-h" ] || [ "${TARGETS[0]}" = "--help" ]; then usage; fi

run_target() {
    case "$1" in
        backend) test_backend ;;
        macos)   test_macos ;;
        ios)     test_ios ;;
        linux)   test_linux ;;
        windows) test_windows ;;
        android) test_android ;;
        all)
            test_backend
            test_macos
            test_ios
            test_linux
            test_windows
            test_android
            ;;
        *)
            echo "unknown target: $1" >&2
            usage
            ;;
    esac
}

section "pinback platform smoke-test"
echo "ROOT=$ROOT"
echo "WINDOWS_HOST=$WINDOWS_HOST ORB_MACHINE=$ORB_MACHINE"

for t in "${TARGETS[@]}"; do
    run_target "$t"
done

kill_local_pinback 2>/dev/null || true

section "summary"
if [ "$FAILURES" -eq 0 ]; then
    pass "all smoke checks passed (${WARNINGS} warnings)"
    exit 0
fi
fail "$FAILURES check(s) failed (${WARNINGS} warnings)"
exit 1
