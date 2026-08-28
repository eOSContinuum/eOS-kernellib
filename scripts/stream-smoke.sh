#!/bin/sh
#
# End-to-end smoke for the streaming-connection contract: deploy
# composite-app without its self-exiting test driver, boot plain HTTP on
# the smoke-tier ports, and assert against the live driver that a
# streaming connection releases when its peer departs, survives while
# its peer stays connected, and disconnects a client that sends bytes on
# it. scripts/stream-probe.py makes the assertions; this script owns the
# runtime and the clean slate.
#
# Why a dedicated harness rather than another run-example profile: two
# of the three assertions are timing assertions against the HTTP layer's
# 60-second inactivity backstop -- one bounded well under it, one run
# well past it -- so the run takes minutes rather than a boot, and the
# example harness's self-exiting sentinel boots cannot express it. The
# composite example's own SSE phases (run-example.sh composite-app) hold
# streams open for 8 seconds and never reach the backstop, which is why
# they stayed green through the defect this asserts against.
#
# Runs module-less: the audit stream at /inventory/events is public, so
# no crypto module is needed and this step has no documented skip.
#
# Usage:
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/stream-smoke.sh
#
# Exits non-zero if any assertion fails, if the presence controls do not
# pass (which would mean the probe cannot observe a release at all), or
# if the runtime stops serving mid-measurement.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "stream-smoke.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "stream-smoke.sh: required tool not found: python3" >&2
    exit 2
fi

# Dedicated smoke-tier ports so this harness coexists with a live
# default-port instance on the same machine (scripts/README.md Port
# allocation on a shared machine).
: "${SMOKE_TELNET_PORT:=48023}"
: "${SMOKE_BINARY_PORT:=48080}"

HOST=127.0.0.1
CONFIG=state/stream-smoke.dgd
BOOT_LOG=state/stream-smoke-boot.log

DGDPID=""
cleanup() {
    if [ -n "$DGDPID" ]; then
        kill "$DGDPID" 2>/dev/null || true
        sleep 1
        kill -9 "$DGDPID" 2>/dev/null || true
    fi
    rm -rf src/usr/WWW src/usr/Inventory
    rm -f src/usr/System/sys/demo_provisiond.c
    rm -f state/snapshot state/snapshot.old state/swap
    rm -f "$CONFIG" "$BOOT_LOG" state/stream-smoke-console.log
    rm -f src/kernel/data/access.data src/kernel/data/admin.pwd
    rm -rf src/usr/System/log
}
trap cleanup EXIT INT TERM

echo "== clean slate =="
for mount in AgentApp Cascade Chat ConsoleExt Inventory MerryApp MyApp \
             Reload SignalApp WebAuthn WWW testop; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap "$BOOT_LOG"
rm -f src/kernel/data/access.data src/kernel/data/admin.pwd
rm -rf src/usr/System/log

# The example's sys/test.c is a self-exiting boot driver: it runs the
# sentinel phases and shuts the runtime down. This harness needs the
# server to stay up and be probed from outside, so the driver is
# stripped from the deploy and from the initd that compiles it.
echo "== deploy composite-app without the self-exiting test driver =="
cp -R examples/composite-app/WWW src/usr/WWW
cp -R examples/composite-app/Inventory src/usr/Inventory
rm -f src/usr/Inventory/sys/test.c
sed '/compile_object("sys\/test")/d' src/usr/Inventory/initd.c \
    > src/usr/Inventory/initd.c.tmp
mv src/usr/Inventory/initd.c.tmp src/usr/Inventory/initd.c
if grep -q 'compile_object("sys/test")' src/usr/Inventory/initd.c; then
    echo "stream-smoke.sh: test-driver strip did not land" >&2
    exit 2
fi

echo "== config on the smoke-tier ports =="
sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
    -e "s|:[[:space:]]*8023[[:space:]]*\]|: $SMOKE_TELNET_PORT ]|" \
    -e "s|^binary_port[[:space:]]*=[[:space:]]*8080|binary_port	= $SMOKE_BINARY_PORT|" \
    example.dgd > "$CONFIG"
grep -q "$SMOKE_TELNET_PORT" "$CONFIG" || {
    echo "stream-smoke.sh: telnet port rewrite missed" >&2; exit 2; }
grep -q "binary_port.*$SMOKE_BINARY_PORT" "$CONFIG" || {
    echo "stream-smoke.sh: http port rewrite missed" >&2; exit 2; }

echo "== boot =="
"$DGD_BIN" "$CONFIG" > "$BOOT_LOG" 2>&1 &
DGDPID=$!
i=0
while ! python3 -c "import socket; socket.create_connection(('$HOST', $SMOKE_BINARY_PORT), 1).close()" 2>/dev/null; do
    if ! kill -0 "$DGDPID" 2>/dev/null; then
        echo "stream-smoke.sh: driver exited during boot; log:" >&2
        tail -30 "$BOOT_LOG" >&2
        echo "STREAM-SMOKE FAIL" >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "stream-smoke.sh: http port never came up; log:" >&2
        tail -30 "$BOOT_LOG" >&2
        echo "STREAM-SMOKE FAIL" >&2
        exit 1
    fi
    sleep 1
done
echo "   driver pid $DGDPID, http $SMOKE_BINARY_PORT, console $SMOKE_TELNET_PORT"

echo "== assertions =="
set +e
python3 "$SCRIPT_DIR/stream-probe.py" \
    --host "$HOST" --http-port "$SMOKE_BINARY_PORT" \
    --console-port "$SMOKE_TELNET_PORT" --pid "$DGDPID" \
    --scripts-dir "$REPO_ROOT/scripts" \
    --transcript state/stream-smoke-console.log
RC=$?
set -e

if ! kill -0 "$DGDPID" 2>/dev/null; then
    echo "FAIL: the driver was gone by the end of the measurement" >&2
    echo "STREAM-SMOKE FAIL" >&2
    exit 1
fi
echo "PASS: driver still up and serving at the end of the measurement"

if [ "$RC" -ne 0 ]; then
    echo "STREAM-SMOKE FAIL" >&2
    exit "$RC"
fi
echo "STREAM-SMOKE PASS"
