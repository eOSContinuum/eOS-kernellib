#!/bin/sh
#
# End-to-end smoke for the /usr/-callable dump-only surface
# (persist_helper->trigger_dump): boot the platform, drive the
# persist-dump verbset (default-deny, operator grant, dump, revoke,
# deny again), then -- while the driver is still up and serving --
# assert the snapshot actually landed and the console still answers.
#
# The mid-run file assertion is the load-bearing half: the kernel
# driver's interrupt hook writes its own snapshot on SIGTERM
# (src/kernel/sys/driver.c interrupt()), so a snapshot found after
# teardown proves nothing about trigger_dump. Here the file is checked
# between the verbset and teardown, with the driver process verified
# alive on both sides of the check, so the only dump that can have
# written it is the driven trigger_dump call. The clean-slate phase
# removes any prior snapshot first.
#
# Runs module-less against a base boot; no example deploy.
#
# Usage:
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/persist-dump-smoke.sh
#
# Exits non-zero if the verbset fails, no snapshot lands, or the
# runtime stops serving.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "persist-dump-smoke.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "persist-dump-smoke.sh: required tool not found: python3" >&2
    exit 2
fi

# Dedicated smoke-tier ports so this harness coexists with a live
# default-port instance on the same machine (scripts/README.md Port
# allocation on a shared machine).
: "${SMOKE_TELNET_PORT:=48023}"
: "${SMOKE_BINARY_PORT:=48080}"

HOST=127.0.0.1
VERBSET=${PERSIST_DUMP_VERBSET:-scripts/verbsets/persist-dump.verbset}

# Generated config path, named for this script: it keys the leftover
# guard and the cleanup sweep, so only this script's own instances are
# matched. A port probe then catches anything else holding the ports.
CONFIG=state/persist-dump-smoke.dgd
if pgrep -f "dgd .*$CONFIG" >/dev/null 2>&1; then
    echo "persist-dump-smoke.sh: a leftover persist-dump-smoke dgd instance is running; stop it first:" >&2
    pgrep -fl "dgd .*$CONFIG" >&2
    exit 2
fi
for _port in "$SMOKE_TELNET_PORT" "$SMOKE_BINARY_PORT"; do
    if python3 -c "import socket; socket.create_connection(('127.0.0.1', $_port), 0.5).close()" 2>/dev/null; then
        echo "persist-dump-smoke.sh: port $_port is already in use (another dgd or service holds it); free it first" >&2
        exit 2
    fi
done

DGDPID=""
cleanup() {
    kill "$DGDPID" 2>/dev/null || true
    sleep 1
    pkill -9 -f "dgd .*$CONFIG" 2>/dev/null || true
}

echo "== clean slate (base boot) =="
for mount in AgentApp Cascade Chat ConsoleExt Inventory MerryApp MyApp Reload SignalApp WebAuthn WWW testop; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap state/persist-dump-boot.log
rm -f src/kernel/data/access.data
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp

# Localize example.dgd under state/: point directory at this checkout
# and move both ports to the smoke tier.
sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
    -e "s|:[[:space:]]*8023[[:space:]]*\]|: $SMOKE_TELNET_PORT ]|" \
    -e "s|:[[:space:]]*8080[[:space:]]*\]|: $SMOKE_BINARY_PORT ]|" \
    example.dgd > "$CONFIG"
if ! grep -q "$SMOKE_TELNET_PORT" "$CONFIG"; then
    echo "persist-dump-smoke.sh: port rewrite did not land in $CONFIG (sed pattern miss?)" >&2
    exit 2
fi

echo "== boot =="
"$DGD_BIN" "$CONFIG" > state/persist-dump-boot.log 2>&1 &
DGDPID=$!
trap cleanup EXIT INT TERM

i=0
while ! python3 -c "import socket; socket.create_connection(('$HOST', $SMOKE_TELNET_PORT), 1).close()" 2>/dev/null; do
    if ! kill -0 "$DGDPID" 2>/dev/null; then
        echo "persist-dump-smoke.sh: driver exited during boot; log:" >&2
        tail -20 state/persist-dump-boot.log >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "persist-dump-smoke.sh: telnet $SMOKE_TELNET_PORT did not come up within 30s; log:" >&2
        tail -20 state/persist-dump-boot.log >&2
        exit 1
    fi
    sleep 1
done

echo "== drive $VERBSET =="
if ! python3 scripts/drive-verbs.py "$VERBSET" --host "$HOST" --port "$SMOKE_TELNET_PORT"; then
    echo "PERSIST-DUMP FAIL" >&2
    exit 1
fi

echo "== snapshot landed mid-run =="
if ! kill -0 "$DGDPID" 2>/dev/null; then
    echo "FAIL: driver not running after the verbset; teardown dumps prove nothing" >&2
    echo "PERSIST-DUMP FAIL" >&2
    exit 1
fi
i=0
while [ ! -s state/snapshot ]; do
    i=$((i + 1))
    if [ "$i" -ge 10 ]; then
        echo "FAIL: trigger_dump left no snapshot at state/snapshot with the driver still up" >&2
        echo "PERSIST-DUMP FAIL" >&2
        exit 1
    fi
    sleep 1
done
echo "PASS: snapshot present with the driver still up ($(wc -c < state/snapshot | tr -d ' ') bytes)"

echo "== still serving after the landed dump =="
printf 'cmd: status\nexpect: Server:\\s+DGD\n' > state/persist-dump-alive.verbset
if python3 scripts/drive-verbs.py state/persist-dump-alive.verbset \
        --host "$HOST" --port "$SMOKE_TELNET_PORT"; then
    echo "PASS: console still answers after the snapshot landed"
else
    echo "FAIL: console did not answer after the snapshot landed" >&2
    echo "PERSIST-DUMP FAIL" >&2
    exit 1
fi

echo "PERSIST-DUMP PASS"
