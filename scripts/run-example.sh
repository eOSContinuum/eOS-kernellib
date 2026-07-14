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
#   deploy   the src/usr/<Name> domain the example deploys as
#   boots    1 = cold only; 2 = cold + restore; 3 = cold + restore +
#            cold-again (the no-snapshot negative case)
#   boot1    selfexit = the driver dumps a snapshot and exits on its
#            own (waited on, 30s cap); timed = boot runs for a fixed
#            window, then is stopped
#   ok       expected " OK" sentinel count (EXPECTED_OK overrides);
#            bump when a test-driver phase adds a sentinel
#
# Sentinels are read from the deployed domain's data/test-result.log.
# Boot output is captured under state/run-<example>-bootN.log.
#
# atomic-demo and http-app have no profile here: they verify against a
# running server via live HTTP probes (see each example's README and
# bundled smoke script). hot-reload-demo verifies both ways -- a headless
# sentinel profile below, plus its bundled HTTP smoke.
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
        chat-app)          echo "Chat 3 selfexit 20" ;;
        hot-reload-demo)   echo "WWW 1 timed 2" ;;
        hot-reload-master) echo "Reload 1 timed 3" ;;
        merry-app)         echo "MerryApp 2 selfexit 28" ;;
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
    echo "known examples: chat-app hot-reload-demo hot-reload-master merry-app signal-app upgrade-cascade vault-app webauthn-app" >&2
    exit 2
fi
PROFILE=$(example_profile "$EXAMPLE")
if [ -z "$PROFILE" ]; then
    echo "run-example.sh: no profile for '$EXAMPLE'; add one to example_profile()" >&2
    echo "known examples: chat-app hot-reload-demo hot-reload-master merry-app signal-app upgrade-cascade vault-app webauthn-app" >&2
    echo "(atomic-demo and http-app verify via live HTTP probes; see their READMEs)" >&2
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

LOG_PREFIX="state/run-$EXAMPLE-boot"

# example.dgd ships with a placeholder base directory (getting-started
# has you localize it by hand). Generate a localized copy under state/
# so this script works unedited from any checkout location.
CONFIG="state/run-example.dgd"
sed "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" example.dgd > "$CONFIG"

# A leftover DGD instance holds the telnet/binary ports; the new boot
# then dies on "bind: Address already in use" and the failure surfaces
# confusingly as "result log not written". Fail fast with the cause.
if pgrep -f 'dgd .*\.dgd' >/dev/null 2>&1; then
    echo "run-example.sh: a dgd instance is already running (it holds the ports); stop it first:" >&2
    pgrep -fl 'dgd .*\.dgd' >&2
    exit 2
fi

echo "== clean slate =="
# Remove EVERY example deploy mount, not just this run's. A leftover mount
# from a prior run is picked up by the System initd's /usr/[A-Z]*/initd.c
# iteration and re-runs on this boot; if it is a selfexit example (it calls
# shutdown() when its driver finishes) it tears the driver down before this
# example's driver completes, truncating the result. Isolation requires a
# single deployed example per boot.
for mount in Cascade Chat MerryApp MyApp Reload SignalApp WebAuthn WWW; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap "$LOG_PREFIX"1.log "$LOG_PREFIX"2.log "$LOG_PREFIX"3.log

echo "== deploy $EXAMPLE as the $DEPLOY_NAME domain =="
cp -R "examples/$EXAMPLE" "src/usr/$DEPLOY_NAME"

if [ "$BOOT1_MODE" = "selfexit" ]; then
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
RESULT="src/usr/$DEPLOY_NAME/data/test-result.log"
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
    echo "FAIL: sentinel count $OK_COUNT != expected $EXPECTED_OK" >&2
    exit 1
fi
echo "PASS"
