#!/bin/sh
#
# One-command bring-up for the composite-app browser demo: deploy the
# interactive shape (WWW + Inventory with the self-exiting test driver
# stripped, plus the demo-only System-tier provisioner), generate a
# certificate the browser genuinely trusts (mkcert), boot with native
# TLS on the labeled https port, run the two bring-up console verbs
# (compile the provisioner, flag the demo capability delegable), and
# leave the instance RUNNING for the walk at:
#
#   https://localhost:8443/demo
#
# This is the executable form of the example README's browser-path
# recipe (examples/composite-app/README.md, The browser path).
#
# Usage:
#   LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
#       DGD_BIN=/path/to/dgd/bin/dgd scripts/demo-composite.sh
#
# Prerequisites: mkcert (with its CA installed: `mkcert -install`,
# once per machine), python3, openssl. The identity ceremonies need
# the crypto module, so LPC_EXT_CRYPTO is required.
#
# Unlike the smoke scripts, success leaves state behind on purpose:
# a running driver, the deployed mounts, the provisioner copy, and
# the generated certificate. Teardown when done:
#   kill <printed pid>
#   rm -rf src/usr/WWW src/usr/Inventory src/usr/System/data/tls
#   rm -f src/usr/System/sys/demo_provisiond.c
#   rm -f state/snapshot state/snapshot.old state/swap \
#         state/demo-composite.dgd state/demo-composite-boot.log
#   rm -f src/kernel/data/access.data
#
# Exits non-zero (and cleans up the partial boot) on any failed phase;
# DEMO READY is the success signal.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "demo-composite.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi
if [ -z "${LPC_EXT_CRYPTO:-}" ] || [ ! -f "$LPC_EXT_CRYPTO" ]; then
    echo "demo-composite.sh: the identity ceremonies need the lpc-ext" >&2
    echo "  crypto module; set LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver>" >&2
    exit 2
fi
for tool in mkcert python3 openssl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "demo-composite.sh: required tool not found: $tool" >&2
        exit 2
    fi
done
if pgrep -f 'dgd .*\.dgd' >/dev/null 2>&1; then
    echo "demo-composite.sh: a dgd instance is already running (it holds the ports); stop it first:" >&2
    pgrep -fl 'dgd .*\.dgd' >&2
    exit 2
fi

HOST=127.0.0.1
HTTP_PORT=8080
HTTPS_PORT=8443
TLS_DATA_DIR=src/usr/System/data/tls
CONFIG=state/demo-composite.dgd
BOOT_LOG=state/demo-composite-boot.log

DGDPID=""
READY=""
cleanup() {
    if [ -z "$READY" ]; then
        kill "$DGDPID" 2>/dev/null || true
        rm -rf src/usr/WWW src/usr/Inventory "$TLS_DATA_DIR"
        rm -f src/usr/System/sys/demo_provisiond.c
        rm -f state/snapshot state/snapshot.old state/swap \
              "$CONFIG" "$BOOT_LOG" state/demo-composite.verbset
    fi
}
trap cleanup EXIT INT TERM

echo "== clean slate =="
for mount in AgentApp Cascade Chat Inventory MerryApp MyApp Reload SignalApp WebAuthn WWW testop; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap "$BOOT_LOG"
rm -f src/kernel/data/access.data
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp "$TLS_DATA_DIR"

echo "== deploy the interactive demo shape =="
cp -R examples/composite-app/WWW src/usr/WWW
cp -R examples/composite-app/Inventory src/usr/Inventory
rm -f src/usr/Inventory/sys/test.c
sed '/compile_object("sys\/test")/d' src/usr/Inventory/initd.c \
    > src/usr/Inventory/initd.c.tmp
mv src/usr/Inventory/initd.c.tmp src/usr/Inventory/initd.c
cp examples/composite-app/System/demo_provisiond.c src/usr/System/sys/

echo "== certificate (mkcert) =="
mkdir -p "$TLS_DATA_DIR"
mkcert -cert-file "$TLS_DATA_DIR/cert.pem" \
       -key-file  "$TLS_DATA_DIR/key.pem"  localhost 127.0.0.1

echo "== config and boot =="
sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
    -e "s|^binary_port[	 ]*=.*|binary_port	= ([ \"*\" : $HTTP_PORT, \"*\" : $HTTPS_PORT ]);|" \
    example.dgd > "$CONFIG"
printf 'modules\t\t= ([ "%s" : "" ]);\n' "$LPC_EXT_CRYPTO" >> "$CONFIG"

"$DGD_BIN" "$CONFIG" > "$BOOT_LOG" 2>&1 &
DGDPID=$!

i=0
while ! python3 -c "import socket; socket.create_connection(('$HOST', $HTTP_PORT), 1).close()" 2>/dev/null; do
    if ! kill -0 "$DGDPID" 2>/dev/null; then
        echo "demo-composite.sh: driver exited during boot; log:" >&2
        tail -20 "$BOOT_LOG" >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "demo-composite.sh: port $HTTP_PORT did not come up within 30s; log:" >&2
        tail -20 "$BOOT_LOG" >&2
        exit 1
    fi
    sleep 1
done

echo "== bring-up console verbs =="
cat > state/demo-composite.verbset <<'VERBS'
cmd: tls-cert reload
absent: missing

cmd: tls-cert
expect: \(present\)
expect: manager registered

cmd: compile /usr/System/sys/demo_provisiond.c
absent: Failed
absent: error

cmd: status /usr/System/sys/demo_provisiond
expect: /usr/System/sys/demo_provisiond

cmd: capability delegable example:delegation-demo on
expect: delegable on
VERBS
python3 scripts/drive-verbs.py state/demo-composite.verbset \
    --host "$HOST" --port 8023
rm -f state/demo-composite.verbset

echo "== probe /demo over TLS 1.3 =="
response=$({ printf 'GET /demo HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n'; sleep 3; } | \
    openssl s_client -connect "$HOST:$HTTPS_PORT" -tls1_3 -quiet -no_ign_eof 2>/dev/null | head -8)
case "$response" in
    "HTTP/1.1 200 OK"*) echo "PASS: /demo answers 200 over TLS 1.3" ;;
    *) echo "FAIL: /demo answered: $response" >&2; exit 1 ;;
esac
case "$response" in
    *"Cache-Control: no-store"*) echo "PASS: no-store header present" ;;
    *) echo "FAIL: Cache-Control: no-store missing" >&2; exit 1 ;;
esac

READY=1
echo ""
echo "DEMO READY: https://localhost:$HTTPS_PORT/demo  (driver pid $DGDPID)"
echo "The instance keeps running; teardown commands are in this script's header."
