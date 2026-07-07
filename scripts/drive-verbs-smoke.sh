#!/bin/sh
#
# Boot the platform and drive admin-console verbset(s) against the live
# telnet console, then shut down. The headless companion to
# run-example.sh for the admin-console (operator-verb) surface: where
# run-example.sh asserts an example's data/test-result.log sentinels,
# this asserts the console's verb responses via scripts/drive-verbs.py.
#
# Usage:
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/drive-verbs-smoke.sh [verbset ...]
#   scripts/drive-verbs-smoke.sh                       # default verbsets
#   DEPLOY=<example>:<Mount> scripts/drive-verbs-smoke.sh <verbset>
#
# Defaults to the admin-baseline, logging-verbs, and dispatcher-verbs
# verbsets, deploying vault-app as the MyApp domain first: the
# dispatcher-verbs clone-addressing cycle needs the named
# property-bearing clone (MyApp:core:item1) the vault-app boot driver
# creates, and vault-app does not self-exit, so the console stays up.
# With explicit verbset arguments no example is deployed unless
# DEPLOY=<example>:<Mount> asks for one; DEPLOY also overrides the
# default run's deployment.
#
# Caveat: a SELFEXIT example -- one whose boot-time driver calls shutdown()
# when it finishes (e.g. merry-app, chat-app) -- tears the server down
# before verbs can be driven. Drive against a non-selfexit deployment, or
# rely on that example's own in-application test phases.
#
# Exits non-zero if the console never accepts telnet or any verbset fails.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "drive-verbs-smoke.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi

HOST=127.0.0.1
PORT=8023

VERBSETS=$*
if [ -z "$VERBSETS" ]; then
    VERBSETS="scripts/verbsets/admin-baseline.verbset scripts/verbsets/logging-verbs.verbset scripts/verbsets/schema-verbs.verbset scripts/verbsets/dispatcher-verbs.verbset"
    # The dispatcher-verbs clone-addressing cycle drives the named clone
    # the vault-app boot driver creates; honor an explicit DEPLOY over
    # this default.
    DEPLOY=${DEPLOY:-vault-app:MyApp}
fi

if pgrep -f 'dgd .*\.dgd' >/dev/null 2>&1; then
    echo "drive-verbs-smoke.sh: a dgd instance is already running (it holds the ports); stop it first:" >&2
    pgrep -fl 'dgd .*\.dgd' >&2
    exit 2
fi

echo "== clean slate (base boot) =="
# A single coherent base boot: no leftover example deploy mount may run.
for mount in Cascade Chat MerryApp MyApp Reload SignalApp WWW; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap state/drive-verbs-boot.log
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp

if [ -n "${DEPLOY:-}" ]; then
    ex=${DEPLOY%%:*}
    mount=${DEPLOY##*:}
    if [ ! -d "examples/$ex" ]; then
        echo "drive-verbs-smoke.sh: example not found: examples/$ex" >&2
        exit 2
    fi
    echo "== deploy $ex as the $mount domain =="
    cp -R "examples/$ex" "src/usr/$mount"
fi

# example.dgd ships a placeholder base directory; localize it under state/.
CONFIG=state/run-example.dgd
sed "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" example.dgd > "$CONFIG"

echo "== boot =="
"$DGD_BIN" "$CONFIG" > state/drive-verbs-boot.log 2>&1 &
DGDPID=$!
trap 'kill "$DGDPID" 2>/dev/null || true' EXIT INT TERM

# Wait for the telnet console to accept connections.
i=0
while ! python3 -c "import socket; socket.create_connection(('$HOST', $PORT), 1).close()" 2>/dev/null; do
    if ! kill -0 "$DGDPID" 2>/dev/null; then
        echo "drive-verbs-smoke.sh: driver exited during boot; log:" >&2
        tail -20 state/drive-verbs-boot.log >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "drive-verbs-smoke.sh: telnet $PORT did not come up within 30s; log:" >&2
        tail -20 state/drive-verbs-boot.log >&2
        exit 1
    fi
    sleep 1
done

rc=0
for vs in $VERBSETS; do
    echo "== drive $vs =="
    if ! python3 scripts/drive-verbs.py "$vs" --host "$HOST" --port "$PORT"; then
        rc=1
    fi
done

kill "$DGDPID" 2>/dev/null || true
if [ "$rc" -eq 0 ]; then
    echo "DRIVE-VERBS PASS"
else
    echo "DRIVE-VERBS FAIL" >&2
fi
exit "$rc"
