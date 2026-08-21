#!/bin/sh
#
# Doc-drift regression for the four-tutorial getting-started path:
# docs/first-hour.md, docs/first-application.md,
# docs/first-http-endpoint.md and docs/first-vault-entity.md. Parses the
# command/expected-output pairs out of every one of those docs' fenced
# transcripts AT RUN TIME
# (scripts/tutorial-replay.py does the parsing and driving -- no
# generated mirror of the transcripts exists anywhere, because a mirror
# would drift out of sync with the docs, which is exactly the failure
# mode this guard exists to catch) and replays them against a
# clean-slate boot, including each doc's in-tutorial `reboot` +
# snapshot-restore cycle. first-hour.md's interactive telnet-connect and
# login-banner blocks, and its initial cold-boot invocation line, are
# named SKIPs rather than assertions (tutorial-replay.py's module
# docstring names the three).
#
# Usage:
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/tutorial-smoke.sh
#
# Exits non-zero if any documented output mismatches, if either doc
# parses to zero command/expected-output blocks (the empty-parse guard --
# a doc restructure that renames a fence marker or anchor sentence this
# parser keys on must not silently vacuum the guard), or if a boot/
# restart step fails.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "tutorial-smoke.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi

# Dedicated smoke-tier ports so this harness coexists with a live
# default-port instance on the same machine (scripts/README.md Port
# allocation on a shared machine). The tutorials teach on the stock
# 8023/8080; scripts/tutorial-replay.py rewrites any parsed command or
# expected line embedding those literal ports to these before use.
: "${SMOKE_TELNET_PORT:=48023}"
: "${SMOKE_BINARY_PORT:=48080}"

# Generated config path, named for this script: it keys the leftover
# guard and the cleanup sweep, so only this script's own instances are
# matched -- an unrelated dgd never blocks the run and can never be
# killed by it. A port probe then catches anything else holding the
# ports, with a clearer message than the eventual bind failure.
CONFIG=state/tutorial-smoke.dgd
if pgrep -f "dgd .*$CONFIG" >/dev/null 2>&1; then
    echo "tutorial-smoke.sh: a leftover tutorial-smoke dgd instance is running; stop it first:" >&2
    pgrep -fl "dgd .*$CONFIG" >&2
    exit 2
fi
for _port in "$SMOKE_TELNET_PORT" "$SMOKE_BINARY_PORT"; do
    if python3 -c "import socket; socket.create_connection(('127.0.0.1', $_port), 0.5).close()" 2>/dev/null; then
        echo "tutorial-smoke.sh: port $_port is already in use (another dgd or service holds it); free it first" >&2
        exit 2
    fi
done

echo "== clean slate =="
# The tutorials assume fresh Pet/KV/WWW grants and a first-connect
# console claim (first-hour.md sections 1-2, which first-application.md
# points back to); the WWW mount first-http-endpoint.md builds also
# collides with the harness's own reserved WWW mount (first-http-
# endpoint.md Cleaning up).
rm -rf src/usr/Pet src/usr/KV src/usr/WWW
rm -f state/snapshot state/snapshot.old state/swap state/tutorial-smoke-boot.log
rm -f src/kernel/data/access.data src/kernel/data/admin.pwd

# example.dgd ships a placeholder base directory; localize it under
# state/ at the config path the guard above is scoped to.
sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
    -e "s|:[[:space:]]*8023[[:space:]]*\]|: $SMOKE_TELNET_PORT ]|" \
    -e "s|^binary_port[[:space:]]*=[[:space:]]*8080|binary_port	= $SMOKE_BINARY_PORT|" \
    example.dgd > "$CONFIG"
if ! grep -q "$SMOKE_TELNET_PORT" "$CONFIG" || ! grep -q "binary_port.*$SMOKE_BINARY_PORT" "$CONFIG"; then
    echo "tutorial-smoke.sh: port rewrite did not land in $CONFIG (sed pattern miss?)" >&2
    exit 2
fi

# scripts/tutorial-replay.py owns the dgd process lifecycle from here on
# (the initial boot and the two in-tutorial reboot/restart cycles) --
# only the console-driving layer knows when a documented `reboot` fires,
# so it is the one that must react to it synchronously. This trap is the
# backstop: it fires regardless of how the python driver exits.
trap 'pkill -9 -f "dgd .*$CONFIG" 2>/dev/null || true' EXIT INT TERM

DGD_BIN="$DGD_BIN" TUTORIAL_CONFIG="$CONFIG" \
SMOKE_TELNET_PORT="$SMOKE_TELNET_PORT" SMOKE_BINARY_PORT="$SMOKE_BINARY_PORT" \
REPO_ROOT="$REPO_ROOT" \
    python3 scripts/tutorial-replay.py
