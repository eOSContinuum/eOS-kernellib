#!/bin/sh
#
# End-to-end smoke for native TLS termination: boot the platform with the
# crypto extension module loaded and a second binary port configured,
# deploy the reference HTTPS application, and drive real HTTPS requests
# against the labeled "https" port. The executable form of the
# examples/https-app README's Verify recipe.
#
# The TLS stack needs the lpc-ext crypto module (the platform loads no
# extensions by default): pass its path via LPC_EXT_CRYPTO and this script
# generates a config whose modules line loads it. A throwaway self-signed
# P-256 certificate is generated per run into src/usr/System/data/tls/
# and removed afterwards; nothing key-shaped survives the run.
#
# Usage:
#   LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/https-smoke.sh
#
# Asserts: GET /health answers "ok" over TLS 1.3, POST /echo round-trips
# a body, an unknown route answers 404, and the plain-HTTP probe against
# the same port fails (the port speaks TLS, not cleartext). Exits
# non-zero on any failed probe.
#
# Probes are driven with openssl s_client rather than curl: the stock
# macOS curl (SecureTransport backend) cannot speak TLS 1.3 and fails
# client-side before sending a byte; openssl is already required for the
# certificate generation and handshakes everywhere.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
cd "$REPO_ROOT"

: "${DGD_BIN:=$(command -v dgd || true)}"
if [ -z "$DGD_BIN" ] || [ ! -x "$DGD_BIN" ]; then
    echo "https-smoke.sh: DGD binary not found; set DGD_BIN=/path/to/dgd" >&2
    exit 2
fi
if [ -z "${LPC_EXT_CRYPTO:-}" ] || [ ! -f "$LPC_EXT_CRYPTO" ]; then
    echo "https-smoke.sh: the TLS stack needs the lpc-ext crypto module;" >&2
    echo "  set LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> (build with" >&2
    echo "  'make crypto' in dworkin/lpc-ext; see docs/operations.md," >&2
    echo "  Host-driver extensions)" >&2
    exit 2
fi
for tool in openssl python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "https-smoke.sh: required tool not found: $tool" >&2
        exit 2
    fi
done

HOST=127.0.0.1
HTTPS_PORT=8443
TLS_DATA_DIR=src/usr/System/data/tls

if pgrep -f 'dgd .*\.dgd' >/dev/null 2>&1; then
    echo "https-smoke.sh: a dgd instance is already running (it holds the ports); stop it first:" >&2
    pgrep -fl 'dgd .*\.dgd' >&2
    exit 2
fi

cleanup() {
    kill "$DGDPID" 2>/dev/null || true
    rm -rf "$TLS_DATA_DIR"
}
DGDPID=""

echo "== clean slate (base boot + https-app deploy) =="
for mount in Cascade Chat MerryApp MyApp Reload SignalApp WWW testop; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap state/https-smoke-boot.log
rm -f src/kernel/data/access.data
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp "$TLS_DATA_DIR"

echo "== deploy https-app as the WWW domain =="
cp -R examples/https-app src/usr/WWW

echo "== generate throwaway P-256 certificate =="
mkdir -p "$TLS_DATA_DIR"
openssl ecparam -name prime256v1 -genkey -noout -out "$TLS_DATA_DIR/key.pem"
openssl req -x509 -key "$TLS_DATA_DIR/key.pem" -out "$TLS_DATA_DIR/cert.pem" \
    -days 2 -subj "/CN=localhost"

# Localize example.dgd under state/: point directory at this checkout,
# add a second binary port for TLS, and load the crypto module (the
# optional-module hook -- the checked-in example.dgd stays module-less).
CONFIG=state/https-smoke.dgd
sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
    -e "s|^binary_port[	 ]*=.*|binary_port	= ([ \"*\" : 8080, \"*\" : $HTTPS_PORT ]);|" \
    example.dgd > "$CONFIG"
printf 'modules\t\t= ([ "%s" : "" ]);\n' "$LPC_EXT_CRYPTO" >> "$CONFIG"

echo "== boot =="
"$DGD_BIN" "$CONFIG" > state/https-smoke-boot.log 2>&1 &
DGDPID=$!
trap cleanup EXIT INT TERM

i=0
while ! python3 -c "import socket; socket.create_connection(('$HOST', $HTTPS_PORT), 1).close()" 2>/dev/null; do
    if ! kill -0 "$DGDPID" 2>/dev/null; then
        echo "https-smoke.sh: driver exited during boot; log:" >&2
        tail -20 state/https-smoke-boot.log >&2
        exit 1
    fi
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "https-smoke.sh: port $HTTPS_PORT did not come up within 30s; log:" >&2
        tail -20 state/https-smoke-boot.log >&2
        exit 1
    fi
    sleep 1
done

rc=0

# tls_request <request-with-CRLFs> -> response on stdout. The trailing
# sleep holds stdin open long enough for the response to arrive;
# -no_ign_eof makes s_client exit when stdin closes.
tls_request() {
    { printf '%b' "$1"; sleep 3; } | \
        openssl s_client -connect "$HOST:$HTTPS_PORT" -tls1_3 -quiet \
            -no_ign_eof 2>/dev/null
}

echo "== probe 1: GET /health over TLS 1.3 =="
response=$(tls_request 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' || true)
case "$response" in
    "HTTP/1.1 200 OK"*ok*) echo "PASS: /health answered 200 with \"ok\"" ;;
    *) echo "FAIL: /health answered: $response" >&2; rc=1 ;;
esac

echo "== probe 2: negotiated TLS version =="
tlsline=$( (echo; sleep 2) | openssl s_client -connect "$HOST:$HTTPS_PORT" -tls1_3 -brief -no_ign_eof 2>&1 | grep 'Protocol version' || true)
case "$tlsline" in
    *TLSv1.3*) echo "PASS: $tlsline" ;;
    *) echo "FAIL: TLSv1.3 not negotiated: $tlsline" >&2; rc=1 ;;
esac

echo "== probe 3: POST /echo round-trip =="
response=$(tls_request 'POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 14\r\nConnection: close\r\n\r\nhello-over-tls' || true)
case "$response" in
    "HTTP/1.1 200 OK"*hello-over-tls) echo "PASS: /echo round-tripped the body" ;;
    *) echo "FAIL: /echo answered: $response" >&2; rc=1 ;;
esac

echo "== probe 4: unknown route answers 404 =="
response=$(tls_request 'GET /no-such-route HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' || true)
case "$response" in
    "HTTP/1.1 404 Not Found"*) echo "PASS: unknown route answered 404" ;;
    *) echo "FAIL: unknown route answered: $response" >&2; rc=1 ;;
esac

echo "== probe 5: cleartext HTTP against the TLS port fails =="
cleartext=$(python3 -c "
import socket
s = socket.create_connection(('$HOST', $HTTPS_PORT), 5)
s.settimeout(5)
s.sendall(b'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n')
try:
    data = s.recv(4096)
except OSError:
    data = b''
print(data.decode('latin-1'), end='')
" || true)
case "$cleartext" in
    *"200 OK"*) echo "FAIL: the TLS port answered a cleartext HTTP request" >&2; rc=1 ;;
    *) echo "PASS: cleartext request refused" ;;
esac

kill "$DGDPID" 2>/dev/null || true
DGDPID=""
rm -rf "$TLS_DATA_DIR" src/usr/WWW

if [ "$rc" -eq 0 ]; then
    echo "HTTPS-SMOKE PASS"
else
    echo "HTTPS-SMOKE FAIL" >&2
fi
exit "$rc"
