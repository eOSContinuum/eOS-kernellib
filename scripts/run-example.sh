#!/bin/sh
#
# Run a sentinel-bearing example end-to-end: clean-slate deploy, boot
# cycle, sentinel count. This is the executable form of each example
# README's Verify recipe; the README keeps the manual sequence as the
# explained fallback.
#
# Usage:
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh <example>
#   scripts/run-example.sh chat-app          # if dgd is on PATH
#   EXPECTED_OK=21 scripts/run-example.sh chat-app
#
# Each example's profile lives in example_profile() below:
#
#   deploy   the src/usr/<Name> domain the example deploys as. A
#            multi-domain example joins its parts with '+' (e.g.
#            "WWW+Inventory"): the example directory then carries one
#            subdirectory per part, each deployed as its own domain,
#            and sentinels are read from the LAST part's data/.
#   boots    1 = cold only; 2 = cold + restore; 3 = cold + restore +
#            cold-again (the no-snapshot negative case)
#   boot1    selfexit = the driver dumps a snapshot and exits on its
#            own (waited on, 30s cap); timed = boot runs for a fixed
#            window, then is stopped; probe = the driver stays up while
#            this script drives live HTTP requests against it, then
#            stops it (http-app's profile: there is no self-testing
#            driver to selfexit, so the four documented routes are the
#            proof, written as sentinel lines this script generates)
#   ok       expected " OK" sentinel count (EXPECTED_OK overrides);
#            bump when a test-driver phase adds a sentinel
#
# Sentinels are read from the deployed domain's data/test-result.log;
# for a "probe" profile this script writes that file itself from the
# HTTP responses it observes, so the tail of the script (sentinel count,
# PASS/FAIL) is unchanged either way. Boot output is captured under
# state/run-<example>-bootN.log.
#
# atomic-demo and hot-reload-demo verify both ways -- a headless
# sentinel profile below, plus each example's bundled HTTP smoke.
#
# Reruns start from a clean slate: the deployed domain, any snapshot,
# and prior boot logs are removed first, so state never carries across
# runs. Exits non-zero on any FAIL sentinel or a sentinel-count
# mismatch.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

# example_profile <example> -> "deploy boots boot1 ok", or "" if unknown
example_profile() {
    case "$1" in
        agent-app)         echo "AgentApp 1 timed 10" ;;
                           # needs LPC_EXT_CRYPTO=...; the operator
                           # continuation (4 more sentinels) is driven
                           # by scripts/verbsets/agent-app.verbset --
                           # see the example README
        atomic-demo)       echo "WWW 1 timed 3" ;;
        chat-app)          echo "Chat 3 selfexit 20" ;;
        composite-app)     echo "WWW+Inventory 2 selfexit 5" ;;
                           # 5 = transport-only subset; with the crypto
                           # module: LPC_EXT_CRYPTO=... EXPECTED_OK=53
        hot-reload-demo)   echo "WWW 1 timed 2" ;;
        hot-reload-master) echo "Reload 1 timed 3" ;;
        http-app)          echo "WWW 1 probe 4" ;;
                           # 4 = the four routes README.md Verify
                           # documents: /health, /status, /echo, 404
        merry-app)         echo "MerryApp 2 selfexit 30" ;;
        signal-app)        echo "SignalApp 1 timed 1" ;;
        upgrade-cascade)   echo "Cascade 1 timed 7" ;;
        vault-app)         echo "MyApp 1 timed 10" ;;
        webauthn-app)      echo "WebAuthn 1 selfexit 13" ;;
        *)                 echo "" ;;
    esac
}

EXAMPLE="${1:-}"
if [ -z "$EXAMPLE" ]; then
    echo "usage: scripts/run-example.sh <example>" >&2
    echo "known examples: agent-app atomic-demo chat-app composite-app hot-reload-demo hot-reload-master http-app merry-app signal-app upgrade-cascade vault-app webauthn-app" >&2
    exit 2
fi
PROFILE=$(example_profile "$EXAMPLE")
if [ -z "$PROFILE" ]; then
    echo "run-example.sh: no profile for '$EXAMPLE'; add one to example_profile()" >&2
    echo "known examples: agent-app atomic-demo chat-app composite-app hot-reload-demo hot-reload-master http-app merry-app signal-app upgrade-cascade vault-app webauthn-app" >&2
    exit 2
fi
set -- $PROFILE
DEPLOY_NAME=$1; BOOTS=$2; BOOT1_MODE=$3; DEFAULT_OK=$4
EXPECTED_OK="${EXPECTED_OK:-$DEFAULT_OK}"

# Resolve the DGD binary: env override first, then PATH.
: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "run-example.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi

# Dedicated smoke-tier ports so this harness coexists with a live
# default-port instance on the same machine (scripts/README.md Port
# allocation on a shared machine).
: "${SMOKE_TELNET_PORT:=48023}"
: "${SMOKE_BINARY_PORT:=48080}"

LOG_PREFIX="state/run-$EXAMPLE-boot"

# example.dgd ships with a placeholder base directory (getting-started
# has you localize it by hand). Generate a localized copy under state/
# so this script works unedited from any checkout location.
CONFIG="state/run-example.dgd"
sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
    -e "s|:[[:space:]]*8023[[:space:]]*\]|: $SMOKE_TELNET_PORT ]|" \
    -e "s|^binary_port[[:space:]]*=[[:space:]]*8080|binary_port	= $SMOKE_BINARY_PORT|" \
    example.dgd > "$CONFIG"
if ! grep -q "$SMOKE_TELNET_PORT" "$CONFIG" || ! grep -q "binary_port.*$SMOKE_BINARY_PORT" "$CONFIG"; then
    echo "run-example.sh: port rewrite did not land in $CONFIG (sed pattern miss?)" >&2
    exit 2
fi

# Optional: load the lpc-ext crypto module (same knob as the smoke
# scripts). webauthn-app's ceremony phases need it and skip without it;
# pair with EXPECTED_OK for the with-module sentinel count.
if [ -n "${LPC_EXT_CRYPTO:-}" ]; then
    if [ ! -f "$LPC_EXT_CRYPTO" ]; then
        echo "run-example.sh: LPC_EXT_CRYPTO not found: $LPC_EXT_CRYPTO" >&2
        exit 2
    fi
    printf 'modules\t\t= ([ "%s" : "" ]);\n' "$LPC_EXT_CRYPTO" >> "$CONFIG"
fi

# A leftover DGD instance holds the telnet/binary ports; the new boot
# then dies on "bind: Address already in use" and the failure surfaces
# confusingly as "result log not written". Fail fast with the cause,
# scoped so only this script's own leftovers block the run: first a
# leftover booted from this script's generated config, then a port
# probe for anything else holding the default ports.
if pgrep -f "dgd .*$CONFIG" >/dev/null 2>&1; then
    echo "run-example.sh: a leftover run-example dgd instance is running; stop it first:" >&2
    pgrep -fl "dgd .*$CONFIG" >&2
    exit 2
fi
if command -v python3 >/dev/null 2>&1; then
    for _port in "$SMOKE_TELNET_PORT" "$SMOKE_BINARY_PORT"; do
        if python3 -c "import socket; socket.create_connection(('127.0.0.1', $_port), 0.5).close()" 2>/dev/null; then
            echo "run-example.sh: port $_port is already in use (another dgd or service holds it); free it first" >&2
            exit 2
        fi
    done
fi

# Backstop for interrupted runs: a boot killed with the script keeps the
# generated-config path in its command line, so the sweep is scoped to
# this script's own instances.
cleanup() {
    pkill -9 -f "dgd .*$CONFIG" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "== clean slate =="
# Remove EVERY example deploy mount, not just this run's. A leftover mount
# from a prior run is picked up by the System initd's /usr/[A-Z]*/initd.c
# iteration and re-runs on this boot; if it is a selfexit example (it calls
# shutdown() when its driver finishes) it tears the driver down before this
# example's driver completes, truncating the result. Isolation requires a
# single deployed example per boot.
for mount in AgentApp Cascade Chat ConsoleExt Inventory KV MerryApp MyApp Reload SignalApp WebAuthn WWW; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap "$LOG_PREFIX"1.log "$LOG_PREFIX"2.log "$LOG_PREFIX"3.log

echo "== deploy $EXAMPLE as the $DEPLOY_NAME domain(s) =="
case "$DEPLOY_NAME" in
    *+*)
        # multi-domain example: one subdirectory per '+'-joined part
        for part in $(echo "$DEPLOY_NAME" | tr '+' ' '); do
            cp -R "examples/$EXAMPLE/$part" "src/usr/$part"
        done
        ;;
    *)
        cp -R "examples/$EXAMPLE" "src/usr/$DEPLOY_NAME"
        ;;
esac
RESULT_DOMAIN=${DEPLOY_NAME##*+}

# Deployed copies of example-internal test clients hardcode the stock
# binary port ('# define PORT	8080'); rewrite them in the DEPLOYED
# artifact only (the canonical examples/ tree keeps the teaching
# default) so the loopback probes hit this run's own instance rather
# than whatever live service holds 8080. Backstop like the config
# rewrite: a deployed define that still says 8080 fails the run.
if [ "$SMOKE_BINARY_PORT" != "8080" ]; then
    for clientsrc in src/usr/*/obj/*.c; do
        [ -f "$clientsrc" ] || continue
        if grep -q '^#[[:space:]]*define[[:space:]]*PORT[[:space:]]*8080' "$clientsrc"; then
            sed "s|^#\([[:space:]]*\)define\([[:space:]]*\)PORT\([[:space:]]*\)8080|#\1define\2PORT\3$SMOKE_BINARY_PORT|" "$clientsrc" > "$clientsrc.tmp" && mv "$clientsrc.tmp" "$clientsrc"
            if grep -q '^#[[:space:]]*define[[:space:]]*PORT[[:space:]]*8080' "$clientsrc"; then
                echo "run-example.sh: PORT define rewrite did not land in $clientsrc" >&2
                exit 2
            fi
        fi
    done
fi

if [ "$BOOT1_MODE" = "probe" ]; then
    echo "== boot 1 (cold; live HTTP probes) =="
    "$DGD_BIN" "$CONFIG" > "${LOG_PREFIX}1.log" 2>&1 &
    B1=$!
    i=0
    READY=""
    while [ "$i" -lt 20 ]; do
        if curl -s -o /dev/null "http://127.0.0.1:$SMOKE_BINARY_PORT/health" 2>/dev/null; then
            READY=1
            break
        fi
        i=$((i + 1))
        sleep 0.5
    done
    RESULT_DIR="src/usr/$RESULT_DOMAIN/data"
    mkdir -p "$RESULT_DIR"
    PROBE_LOG="$RESULT_DIR/test-result.log"
    : > "$PROBE_LOG"
    if [ -z "$READY" ]; then
        echo "GET /health: FAIL (server did not become ready within 10s)" >> "$PROBE_LOG"
    else
        BODY=$(curl -s "http://127.0.0.1:$SMOKE_BINARY_PORT/health")
        if [ "$BODY" = "ok" ]; then
            echo "GET /health: OK" >> "$PROBE_LOG"
        else
            echo "GET /health: FAIL (body '$BODY')" >> "$PROBE_LOG"
        fi

        BODY=$(curl -s "http://127.0.0.1:$SMOKE_BINARY_PORT/status")
        if printf '%s' "$BODY" | grep -q '^objects='; then
            echo "GET /status: OK" >> "$PROBE_LOG"
        else
            echo "GET /status: FAIL (body '$BODY')" >> "$PROBE_LOG"
        fi

        BODY=$(curl -s -d 'hello' "http://127.0.0.1:$SMOKE_BINARY_PORT/echo")
        if [ "$BODY" = "hello" ]; then
            echo "POST /echo: OK" >> "$PROBE_LOG"
        else
            echo "POST /echo: FAIL (body '$BODY')" >> "$PROBE_LOG"
        fi

        CODE=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$SMOKE_BINARY_PORT/no-such-route")
        if [ "$CODE" = "404" ]; then
            echo "GET /no-such-route: OK" >> "$PROBE_LOG"
        else
            echo "GET /no-such-route: FAIL (status $CODE)" >> "$PROBE_LOG"
        fi
    fi
    kill "$B1" 2>/dev/null || true
    wait "$B1" 2>/dev/null || true
elif [ "$BOOT1_MODE" = "selfexit" ]; then
    echo "== boot 1 (cold; driver dumps + self-exits) =="
    "$DGD_BIN" "$CONFIG" > "${LOG_PREFIX}1.log" 2>&1 &
    B1=$!
    i=0
    while kill -0 "$B1" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -ge 30 ]; then
            echo "boot 1 did not self-exit within 30s; stopping it" >&2
            kill "$B1" 2>/dev/null || true
            break
        fi
        sleep 1
    done
else
    echo "== boot 1 (cold; timed window) =="
    "$DGD_BIN" "$CONFIG" > "${LOG_PREFIX}1.log" 2>&1 &
    B1=$!
    sleep 6
    kill "$B1" 2>/dev/null || true
fi

if [ "$BOOTS" -ge 2 ]; then
    echo "== boot 2 (restore from snapshot) =="
    "$DGD_BIN" "$CONFIG" state/snapshot > "${LOG_PREFIX}2.log" 2>&1 &
    B2=$!
    sleep 6
    kill "$B2" 2>/dev/null || true
fi

if [ "$BOOTS" -ge 3 ]; then
    echo "== boot 3 (cold, no snapshot: cold-boot negative) =="
    "$DGD_BIN" "$CONFIG" > "${LOG_PREFIX}3.log" 2>&1 &
    B3=$!
    sleep 4
    kill "$B3" 2>/dev/null || true
fi

echo "== sentinels =="
RESULT="src/usr/$RESULT_DOMAIN/data/test-result.log"
if [ ! -f "$RESULT" ]; then
    echo "FAIL: result log not written: $RESULT (boot logs: ${LOG_PREFIX}N.log)" >&2
    exit 1
fi
SENTINELS=$(cat "$RESULT")
printf '%s\n' "$SENTINELS"

OK_COUNT=$(printf '%s\n' "$SENTINELS" | grep -c " OK" || true)
echo "== $OK_COUNT \" OK\" sentinels (expected $EXPECTED_OK) =="

if printf '%s\n' "$SENTINELS" | grep -qi fail; then
    echo "FAIL: a FAIL sentinel is present" >&2
    exit 1
fi
if [ "$OK_COUNT" -ne "$EXPECTED_OK" ]; then
    echo "FAIL: sentinel count $OK_COUNT != expected $EXPECTED_OK (sentinels: $RESULT; boot logs: ${LOG_PREFIX}N.log)" >&2
    exit 1
fi
echo "PASS"
