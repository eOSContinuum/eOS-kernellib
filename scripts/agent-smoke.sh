#!/bin/sh
#
# Agent-identity end-to-end with the statedump discipline proof. Boots
# the platform with the lpc-ext crypto module and drives, over the
# admin console:
#
#   - the key ceremony with a real foreign signer (OpenSSL Ed25519 via
#     the openssl CLI, so the platform verifies bytes it did not
#     produce): mint an agent with the raw public key, sign the
#     domain-separated challenge, authenticate, and prove the domain
#     tag is load-bearing (a signature over the bare challenge is
#     refused);
#   - the token ceremony including required expiry (a short-ttl token
#     authenticates, then expires and is refused);
#   - suspension killing live sessions and delegated grants while a
#     coexisting operator grant of the same capability survives on its
#     own source; resume restoring authentication but never grants;
#   - the load-bearing check: a statedump taken with a live agent token
#     and a live agent session is scanned for the token plaintext
#     (must be absent) with the token's SHA-256 hash and the agent
#     principal as positive controls (their absence would mean the
#     scan is vacuous).
#
# Usage:
#   LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
#       DGD_BIN=/path/to/dgd/bin/dgd scripts/agent-smoke.sh
#
# Needs python3 (stdlib only) and an openssl CLI with Ed25519 support
# (OpenSSL 1.1.1+; LibreSSL will not do). Exits non-zero on any phase
# failure; AGENT-SMOKE PASS is the pass signal.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "agent-smoke.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi
if [ -z "${LPC_EXT_CRYPTO:-}" ] || [ ! -f "$LPC_EXT_CRYPTO" ]; then
    echo "agent-smoke.sh: the agent ceremonies need the lpc-ext crypto" >&2
    echo "  module; set LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> (build" >&2
    echo "  with 'make crypto' in dworkin/lpc-ext)" >&2
    exit 2
fi
for tool in python3 openssl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "agent-smoke.sh: required tool not found: $tool" >&2
        exit 2
    fi
done
if ! openssl genpkey -algorithm ed25519 >/dev/null 2>&1; then
    echo "agent-smoke.sh: this openssl cannot generate Ed25519 keys;" >&2
    echo "  install OpenSSL 1.1.1+ (LibreSSL will not do)" >&2
    exit 2
fi

HOST=127.0.0.1

if pgrep -f 'dgd .*\.dgd' >/dev/null 2>&1; then
    echo "agent-smoke.sh: a dgd instance is already running (it holds the ports); stop it first:" >&2
    pgrep -fl 'dgd .*\.dgd' >&2
    exit 2
fi

DGDPID=""
cleanup() {
    kill "$DGDPID" 2>/dev/null || true
    rm -f state/agent-smoke-key.pem state/agent-smoke-badkey.pem \
          state/agent-smoke-msg state/agent-smoke-sig.bin
}

echo "== clean slate (base boot) =="
for mount in AgentApp Cascade Chat Inventory MerryApp MyApp Reload SignalApp WebAuthn WWW testop; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap state/agent-smoke-boot.log
rm -f src/kernel/data/access.data
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp

CONFIG=state/agent-smoke.dgd
sed "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" example.dgd > "$CONFIG"
printf 'modules\t\t= ([ "%s" : "" ]);\n' "$LPC_EXT_CRYPTO" >> "$CONFIG"

echo "== boot (crypto module loaded) =="
"$DGD_BIN" "$CONFIG" > state/agent-smoke-boot.log 2>&1 &
DGDPID=$!
trap cleanup EXIT INT TERM

i=0
while ! python3 -c "import socket; socket.create_connection(('$HOST', 8023), 1).close()" 2>/dev/null; do
    if ! kill -0 "$DGDPID" 2>/dev/null; then
        echo "agent-smoke.sh: driver exited during boot; log:" >&2
        tail -20 state/agent-smoke-boot.log >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "agent-smoke.sh: console did not come up within 30s; log:" >&2
        tail -20 state/agent-smoke-boot.log >&2
        exit 1
    fi
    sleep 1
done

rc=0

# drive <verbset-body> [transcript] -- write an ephemeral verbset and
# drive it against the live console, optionally keeping the transcript.
drive() {
    printf '%s\n' "$1" > state/agent-smoke.verbset
    if [ -n "${2:-}" ]; then
        python3 scripts/drive-verbs.py state/agent-smoke.verbset \
            --host "$HOST" --port 8023 --transcript "$2"
    else
        python3 scripts/drive-verbs.py state/agent-smoke.verbset \
            --host "$HOST" --port 8023
    fi
}

# sig_lpc <file> -- a file's bytes as an octal-escaped LPC string literal
sig_lpc() {
    python3 -c "import sys; sys.stdout.write(''.join('\\\\%03o' % b for b in open(sys.argv[1], 'rb').read()))" "$1"
}

echo "== phase 0: console escape sanity =="
if drive 'cmd: code "\101\102\103"
expect: \$\d+ = "ABC"'; then
    echo "PASS: octal-escaped literals reach the console intact"
else
    echo "FAIL: octal escape probe failed; the signature transport is broken" >&2
    rc=1
fi

echo "== phase 1: controller, delegable capability, agent with real key =="
openssl genpkey -algorithm ed25519 -out state/agent-smoke-key.pem
PUBB64=$(openssl pkey -in state/agent-smoke-key.pem -pubout -outform DER |
    tail -c 32 |
    python3 -c "import base64, sys; sys.stdout.write(base64.urlsafe_b64encode(sys.stdin.buffer.read()).decode().rstrip('='))")

if drive "cmd: identity mint 1
expect: minted identity:[0-9a-f-]+

cmd: identity mint-agent not-yet token
expect: identity: no such identity" state/agent-smoke-p1.transcript; then
    echo "PASS: controller minted"
else
    echo "FAIL: controller mint failed" >&2
    rc=1
fi
CONTROLLER=$(sed -n 's/^identity: minted identity:\([0-9a-f-]*\) with.*/\1/p' \
    state/agent-smoke-p1.transcript | head -1)
if [ -z "$CONTROLLER" ]; then
    echo "FAIL: could not recover the controller uuid" >&2
    exit 1
fi

if drive "cmd: identity grant $CONTROLLER platform.smoke
expect: identity: granted platform.smoke

cmd: capability delegable platform.smoke on
expect: capability: platform.smoke delegable on

cmd: identity mint-agent $CONTROLLER key ak-smoke-1 Ed25519 $PUBB64
expect: minted agent identity:[0-9a-f-]+" state/agent-smoke-p2.transcript; then
    echo "PASS: delegable capability set; agent minted with the real public key"
else
    echo "FAIL: agent key mint failed" >&2
    rc=1
fi
AGENT=$(sed -n 's/^identity: minted agent identity:\([0-9a-f-]*\).*/\1/p' \
    state/agent-smoke-p2.transcript | head -1)
if [ -z "$AGENT" ]; then
    echo "FAIL: could not recover the agent uuid" >&2
    exit 1
fi

echo "== phase 2: key ceremony -- domain-separated signature authenticates =="
drive 'cmd: code "/usr/System/sys/agentauthd"->issue_challenge()
expect: \$\d+ = "[A-Za-z0-9_-]+"' state/agent-smoke-p3.transcript || rc=1
CHAL=$(sed -n 's/^\$[0-9]* = "\([A-Za-z0-9_-]*\)".*/\1/p' \
    state/agent-smoke-p3.transcript | head -1)
if [ -z "$CHAL" ]; then
    echo "FAIL: could not recover a challenge" >&2
    exit 1
fi

printf 'eos-agent-auth-v1:%s' "$CHAL" > state/agent-smoke-msg
openssl pkeyutl -sign -inkey state/agent-smoke-key.pem -rawin \
    -in state/agent-smoke-msg -out state/agent-smoke-sig.bin
SIG=$(sig_lpc state/agent-smoke-sig.bin)

if drive "cmd: code \"/usr/System/sys/authd\"->authenticate_agent_key(\"$CHAL\", \"ak-smoke-1\", \"$SIG\")
expect: \"identity:$AGENT\", \"[A-Za-z0-9_-]+\"" state/agent-smoke-p4.transcript; then
    echo "PASS: foreign-signed domain-separated assertion authenticates"
else
    echo "FAIL: key ceremony failed" >&2
    rc=1
fi
KSESSION=$(sed -n 's/.*"identity:[0-9a-f-]*", "\([A-Za-z0-9_-]*\)".*/\1/p' \
    state/agent-smoke-p4.transcript | head -1)

# domain separation is load-bearing: a signature over the BARE
# challenge (no domain tag) must be refused
CHAL2=$(drive 'cmd: code "/usr/System/sys/agentauthd"->issue_challenge()
expect: \$\d+ = "[A-Za-z0-9_-]+"' state/agent-smoke-p5.transcript >/dev/null 2>&1; \
    sed -n 's/^\$[0-9]* = "\([A-Za-z0-9_-]*\)".*/\1/p' state/agent-smoke-p5.transcript | head -1)
printf '%s' "$CHAL2" > state/agent-smoke-msg
openssl pkeyutl -sign -inkey state/agent-smoke-key.pem -rawin \
    -in state/agent-smoke-msg -out state/agent-smoke-sig.bin
BARESIG=$(sig_lpc state/agent-smoke-sig.bin)
if drive "cmd: code \"/usr/System/sys/authd\"->authenticate_agent_key(\"$CHAL2\", \"ak-smoke-1\", \"$BARESIG\")
expect: Error: agentauth: signature invalid"; then
    echo "PASS: un-domain-separated signature refused"
else
    echo "FAIL: a bare-challenge signature was accepted" >&2
    rc=1
fi

# a different key must be refused outright
openssl genpkey -algorithm ed25519 -out state/agent-smoke-badkey.pem
printf 'eos-agent-auth-v1:%s' "$CHAL2" > state/agent-smoke-msg
openssl pkeyutl -sign -inkey state/agent-smoke-badkey.pem -rawin \
    -in state/agent-smoke-msg -out state/agent-smoke-sig.bin
BADSIG=$(sig_lpc state/agent-smoke-sig.bin)
if drive "cmd: code \"/usr/System/sys/authd\"->authenticate_agent_key(\"$CHAL2\", \"ak-smoke-1\", \"$BADSIG\")
expect: Error: agentauth: signature invalid"; then
    echo "PASS: wrong-key signature refused"
else
    echo "FAIL: a wrong-key signature was accepted" >&2
    rc=1
fi

echo "== phase 3: token ceremony -- required expiry enforced =="
if drive "cmd: identity mint-agent $CONTROLLER token 2
expect: minted agent identity:[0-9a-f-]+
expect: identity: token [A-Za-z0-9_-]+" state/agent-smoke-p6.transcript; then
    echo "PASS: short-ttl token minted"
else
    echo "FAIL: token mint failed" >&2
    rc=1
fi
SHORTTOK=$(sed -n 's/^identity: token \([A-Za-z0-9_-]*\).*/\1/p' \
    state/agent-smoke-p6.transcript | head -1)

if drive "cmd: code \"/usr/System/sys/authd\"->authenticate_agent_token(\"$SHORTTOK\")
expect: \"identity:[0-9a-f-]+\", \"[A-Za-z0-9_-]+\""; then
    echo "PASS: live token authenticates"
else
    echo "FAIL: live token refused" >&2
    rc=1
fi
sleep 3
if drive "cmd: code \"/usr/System/sys/authd\"->authenticate_agent_token(\"$SHORTTOK\")
expect: Error: agentauth: token expired"; then
    echo "PASS: expired token refused"
else
    echo "FAIL: an expired token authenticated" >&2
    rc=1
fi

echo "== phase 4: suspension -- sessions and delegated grants die, operator grant survives =="
if drive "cmd: identity delegate $CONTROLLER $AGENT platform.smoke
expect: identity: delegated platform.smoke

cmd: identity grant $AGENT platform.smoke
expect: identity: granted platform.smoke

cmd: code \"/kernel/sys/capabilityd\"->is_allowed(\"platform.smoke\", \"identity:$AGENT\")
expect: \\\$\\d+ = 1

cmd: identity suspend $AGENT
expect: identity: suspended; sessions revoked: [1-9]

cmd: code \"/kernel/sys/capabilityd\"->is_allowed(\"platform.smoke\", \"identity:$AGENT\")
expect: \\\$\\d+ = 1

cmd: identity undelegate $CONTROLLER $AGENT platform.smoke
expect: identity: no such delegation

cmd: session validate $KSESSION
expect: session: no live session for that token

cmd: identity ungrant $AGENT platform.smoke
expect: identity: ungranted platform.smoke

cmd: code \"/kernel/sys/capabilityd\"->is_allowed(\"platform.smoke\", \"identity:$AGENT\")
expect: \\\$\\d+ = 0

cmd: identity resume $AGENT
expect: identity: resumed"; then
    echo "PASS: suspend killed sessions and the delegation; the operator source survived it; resume restored nothing"
else
    echo "FAIL: suspension semantics failed" >&2
    rc=1
fi

# resume restores authentication: a fresh ceremony succeeds, grants stay gone
CHAL3=$(drive 'cmd: code "/usr/System/sys/agentauthd"->issue_challenge()
expect: \$\d+ = "[A-Za-z0-9_-]+"' state/agent-smoke-p7.transcript >/dev/null 2>&1; \
    sed -n 's/^\$[0-9]* = "\([A-Za-z0-9_-]*\)".*/\1/p' state/agent-smoke-p7.transcript | head -1)
printf 'eos-agent-auth-v1:%s' "$CHAL3" > state/agent-smoke-msg
openssl pkeyutl -sign -inkey state/agent-smoke-key.pem -rawin \
    -in state/agent-smoke-msg -out state/agent-smoke-sig.bin
SIG3=$(sig_lpc state/agent-smoke-sig.bin)
if drive "cmd: code \"/usr/System/sys/authd\"->authenticate_agent_key(\"$CHAL3\", \"ak-smoke-1\", \"$SIG3\")
expect: \"identity:$AGENT\", \"[A-Za-z0-9_-]+\"

cmd: code \"/kernel/sys/capabilityd\"->is_allowed(\"platform.smoke\", \"identity:$AGENT\")
expect: \\\$\\d+ = 0"; then
    echo "PASS: after resume the ceremony works and the grants stay gone"
else
    echo "FAIL: post-resume semantics failed" >&2
    rc=1
fi

echo "== phase 5: statedump scan -- agent token plaintext must not persist =="
if drive "cmd: identity mint-agent $CONTROLLER token
expect: identity: token [A-Za-z0-9_-]+" state/agent-smoke-p8.transcript; then
    :
else
    echo "FAIL: could not mint the scan token" >&2
    rc=1
fi
SCANTOK=$(sed -n 's/^identity: token \([A-Za-z0-9_-]*\).*/\1/p' \
    state/agent-smoke-p8.transcript | head -1)

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

if [ -f state/snapshot ] && [ -n "$SCANTOK" ]; then
    if SCANTOK="$SCANTOK" AGENT="$AGENT" python3 - <<'PYEOF'
import hashlib, os, sys
token = os.environ["SCANTOK"].encode()
principal = ("identity:" + os.environ["AGENT"]).encode()
snap = open("state/snapshot", "rb").read()
tok_hash = hashlib.sha256(token).hexdigest().encode()
fails = []
if token in snap:
    fails.append("agent token plaintext present in statedump")
if tok_hash not in snap:
    fails.append("positive control missing: token hash absent (scan vacuous)")
if principal not in snap:
    fails.append("positive control missing: agent principal absent (scan vacuous)")
if fails:
    print("FAIL: " + "; ".join(fails))
    sys.exit(1)
print("PASS: agent token plaintext absent; hash + principal present (%d bytes scanned)"
      % len(snap))
PYEOF
    then
        :
    else
        rc=1
    fi
else
    echo "FAIL: no snapshot or no token to scan" >&2
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "AGENT-SMOKE PASS"
else
    echo "AGENT-SMOKE FAIL" >&2
fi
exit "$rc"
