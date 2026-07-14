#!/bin/sh
#
# Base-boot storm guard: boot the platform with NO example deployed and
# assert the boot log and the logd sink (system.log) stay under a small
# line bound.
#
# A daemon that writes a file from inside an atomic function hits DGD's
# "write_file() within atomic function" prohibition; because the driver
# notifies the error manager of even caught errors, an error-path file
# write can re-enter and storm -- a clean boot once produced a log of
# hundreds of thousands of lines this way. The logging facility defers
# its writes out of the atomic via call_out to avoid exactly this. This
# guard pins the property the example smokes cannot: a no-example base
# boot stays quiet. A storm shows up as a log size explosion, orders of
# magnitude over the bound, so the exact threshold is not delicate.
#
# Usage:
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/base-boot-guard.sh
#   MAX_LINES=400 scripts/base-boot-guard.sh        # override the bound
#
# Exits non-zero if the driver dies during boot or either log exceeds the
# bound.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "base-boot-guard.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi

MAX_LINES="${MAX_LINES:-400}"
BOOT_LOG=state/base-boot-guard.log
SYSLOG=src/usr/System/log/system.log

if pgrep -f 'dgd .*\.dgd' >/dev/null 2>&1; then
    echo "base-boot-guard.sh: a dgd instance is already running; stop it first:" >&2
    pgrep -fl 'dgd .*\.dgd' >&2
    exit 2
fi

echo "== clean slate (no example deployed) =="
for mount in Cascade Chat MerryApp MyApp Reload SignalApp WebAuthn WWW; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap "$BOOT_LOG"
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp

CONFIG=state/run-example.dgd
sed "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" example.dgd > "$CONFIG"

echo "== boot (no example; timed window) =="
"$DGD_BIN" "$CONFIG" > "$BOOT_LOG" 2>&1 &
PID=$!
sleep 6
if ! kill -0 "$PID" 2>/dev/null; then
    echo "base-boot-guard.sh: driver exited during the boot window; log:" >&2
    tail -20 "$BOOT_LOG" >&2
    exit 1
fi
kill "$PID" 2>/dev/null || true

boot_lines=$(wc -l < "$BOOT_LOG" | tr -d ' ')
sys_lines=0
if [ -f "$SYSLOG" ]; then
    sys_lines=$(wc -l < "$SYSLOG" | tr -d ' ')
fi

echo "== boot log: $boot_lines lines; system.log: $sys_lines lines (bound $MAX_LINES) =="

rc=0
if [ "$boot_lines" -gt "$MAX_LINES" ]; then
    echo "GUARD FAIL: boot log $boot_lines lines exceeds $MAX_LINES (atomic-write storm?)" >&2
    rc=1
fi
if [ "$sys_lines" -gt "$MAX_LINES" ]; then
    echo "GUARD FAIL: system.log $sys_lines lines exceeds $MAX_LINES (atomic-write storm?)" >&2
    rc=1
fi
if [ "$rc" -eq 0 ]; then
    echo "GUARD PASS"
fi
exit "$rc"
