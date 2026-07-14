#!/bin/sh
#
# Session-daemon end-to-end with the statedump discipline proof. Boots
# the platform with the lpc-ext crypto module, drives the session
# lifecycle over the admin console (mint, validate, revoke, TTL expiry,
# revoke-principal), and -- the load-bearing check -- takes a statedump
# while a session is live and scans the image for the plaintext token.
# The token's plaintext exists only in the mint response; what persists
# is its SHA-256 hash, so the plaintext bytes must be absent from the
# snapshot while the hash and principal are present (the positive
# control: their absence would mean the scan itself is broken).
#
# The crypto module is required; the lpc-ext crypto extension is built
# with 'make crypto' in dworkin/lpc-ext (docs/operations.md,
# Host-driver extensions).
#
# Usage:
#   LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
#       DGD_BIN=/path/to/dgd/bin/dgd scripts/session-smoke.sh
#
# Exits non-zero on any phase failure; SESSION-SMOKE PASS is the pass
# signal.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "session-smoke.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi
if [ -z "${LPC_EXT_CRYPTO:-}" ] || [ ! -f "$LPC_EXT_CRYPTO" ]; then
    echo "session-smoke.sh: the session daemon needs the lpc-ext crypto" >&2
    echo "  module; set LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> (build" >&2
    echo "  with 'make crypto' in dworkin/lpc-ext)" >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "session-smoke.sh: required tool not found: python3" >&2
    exit 2
fi

HOST=127.0.0.1

if pgrep -f 'dgd .*\.dgd' >/dev/null 2>&1; then
    echo "session-smoke.sh: a dgd instance is already running (it holds the ports); stop it first:" >&2
    pgrep -fl 'dgd .*\.dgd' >&2
    exit 2
fi

DGDPID=""
cleanup() {
    kill "$DGDPID" 2>/dev/null || true
}

echo "== clean slate (base boot) =="
for mount in Cascade Chat MerryApp MyApp Reload SignalApp WebAuthn WWW testop; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap state/session-smoke-boot.log
rm -f src/kernel/data/access.data
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp

CONFIG=state/session-smoke.dgd
sed "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" example.dgd > "$CONFIG"
printf 'modules\t\t= ([ "%s" : "" ]);\n' "$LPC_EXT_CRYPTO" >> "$CONFIG"

echo "== boot (crypto module loaded) =="
"$DGD_BIN" "$CONFIG" > state/session-smoke-boot.log 2>&1 &
DGDPID=$!
trap cleanup EXIT INT TERM

i=0
while ! python3 -c "import socket; socket.create_connection(('$HOST', 8023), 1).close()" 2>/dev/null; do
    if ! kill -0 "$DGDPID" 2>/dev/null; then
        echo "session-smoke.sh: driver exited during boot; log:" >&2
        tail -20 state/session-smoke-boot.log >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "session-smoke.sh: console did not come up within 30s; log:" >&2
        tail -20 state/session-smoke-boot.log >&2
        exit 1
    fi
    sleep 1
done

rc=0

# drive <verbset-body> [transcript] -- write an ephemeral verbset and
# drive it against the live console, optionally keeping the transcript.
drive() {
    printf '%s\n' "$1" > state/session-smoke.verbset
    if [ -n "${2:-}" ]; then
        python3 scripts/drive-verbs.py state/session-smoke.verbset \
            --host "$HOST" --port 8023 --transcript "$2"
    else
        python3 scripts/drive-verbs.py state/session-smoke.verbset \
            --host "$HOST" --port 8023
    fi
}

PRINCIPAL="identity:11111111-2222-3333-4444-555555555555"

echo "== phase 1: mint a session, capture the token =="
if drive "cmd: session mint $PRINCIPAL
expect: session: minted for $PRINCIPAL
capture: token session: token (\\S+)

cmd: session validate %{token}
expect: session: valid, principal $PRINCIPAL" state/session-smoke.transcript; then
    echo "PASS: minted and validated a live session"
else
    echo "FAIL: mint/validate lifecycle failed" >&2
    rc=1
fi

# recover the plaintext token from the transcript for the statedump scan
TOKEN=$(sed -n 's/^session: token \([A-Za-z0-9_-][A-Za-z0-9_-]*\).*/\1/p' \
    state/session-smoke.transcript | head -1)
if [ -z "$TOKEN" ]; then
    echo "FAIL: could not recover the minted token from the transcript" >&2
    rc=1
fi

echo "== phase 2: statedump with the session live, scan for the token =="
before=0
[ -f state/snapshot ] && before=$(wc -c < state/snapshot)
drive 'cmd: snapshot' >/dev/null || true
i=0
while :; do
    sleep 1
    now=$(wc -c < state/snapshot 2>/dev/null || echo 0)
    if [ "$now" != "0" ] && [ "$now" != "$before" ]; then
        sleep 1
        settled=$(wc -c < state/snapshot 2>/dev/null || echo 0)
        [ "$settled" = "$now" ] && break
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "FAIL: snapshot did not land within 30s" >&2
        rc=1
        break
    fi
done

if [ -f state/snapshot ]; then
    if TOKEN="$TOKEN" PRINCIPAL="$PRINCIPAL" python3 - <<'PYEOF'
import hashlib, os, sys
token = os.environ["TOKEN"].encode()
principal = os.environ["PRINCIPAL"].encode()
snap = open("state/snapshot", "rb").read()
tok_hash = hashlib.sha256(token).hexdigest().encode()
fails = []
# the plaintext token must NOT appear, in raw or base64url-of-raw form
if token in snap:
    fails.append("plaintext token present in statedump")
# the stored hash and the principal MUST appear -- the positive control:
# their absence would mean the session never persisted and the scan is
# vacuous
if tok_hash not in snap:
    fails.append("positive control missing: token hash absent (scan vacuous)")
if principal not in snap:
    fails.append("positive control missing: principal absent (scan vacuous)")
if fails:
    print("FAIL: " + "; ".join(fails))
    sys.exit(1)
print("PASS: plaintext token absent; hash + principal present (%d bytes scanned)"
      % len(snap))
PYEOF
    then
        :
    else
        rc=1
    fi
else
    echo "FAIL: no snapshot to scan" >&2
    rc=1
fi

echo "== phase 3: revoke, then validation fails =="
if drive "cmd: session mint $PRINCIPAL
expect: session: token
capture: token2 session: token (\\S+)

cmd: session revoke %{token2}
expect: session: revoked

cmd: session validate %{token2}
expect: session: no live session for that token"; then
    echo "PASS: a revoked token no longer validates"
else
    echo "FAIL: revoke lifecycle failed" >&2
    rc=1
fi

echo "== phase 4: TTL expiry drops the session =="
# Captures do not cross drive() invocations (separate processes), and the
# verbset format has no sleep -- so mint with a 1s TTL, recover the token
# from the transcript, sleep past expiry, then validate the literal.
if drive "cmd: session mint $PRINCIPAL 1
expect: session: token
capture: token3 session: token (\\S+)

cmd: session validate %{token3}
expect: session: valid, principal $PRINCIPAL" state/session-smoke.ttl; then
    TOKEN3=$(sed -n 's/^session: token \([A-Za-z0-9_-][A-Za-z0-9_-]*\).*/\1/p' \
        state/session-smoke.ttl | head -1)
    sleep 2
    if [ -n "$TOKEN3" ] && drive "cmd: session validate $TOKEN3
absent: valid, principal
expect: no live session for that token"; then
        echo "PASS: an expired (1s TTL) session no longer validates"
    else
        echo "FAIL: expiry validation phase failed" >&2
        rc=1
    fi
else
    echo "FAIL: TTL mint phase failed" >&2
    rc=1
fi

echo "== phase 5: revoke-principal drops every session for a principal =="
if drive "cmd: session mint $PRINCIPAL
expect: session: token

cmd: session mint $PRINCIPAL
expect: session: token

cmd: session revoke-principal $PRINCIPAL
expect: session: revoked [1-9][0-9]* session"; then
    echo "PASS: revoke-principal cleared the principal's sessions"
else
    echo "FAIL: revoke-principal phase failed" >&2
    rc=1
fi

drive 'cmd: halt' >/dev/null 2>&1 || true
sleep 1

if [ "$rc" -eq 0 ]; then
    echo "SESSION-SMOKE PASS"
else
    echo "SESSION-SMOKE FAIL" >&2
fi
exit "$rc"
