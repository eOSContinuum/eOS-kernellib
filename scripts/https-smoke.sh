#!/bin/sh
#
# End-to-end smoke for native TLS termination and its certificate
# surface: boot the platform with the crypto extension module loaded and
# a second binary port configured but NO certificate, prove the HTTPS
# bootstrap stands down, then supply a certificate and activate it with
# the tls-cert operator verb -- no restart -- and drive real HTTPS
# requests against the labeled "https" port. Finishes by proving the
# private key never persists: a statedump is taken twice (idle, and with
# a live established TLS connection) and scanned for the key in every
# in-memory representation (DER, PEM base64, raw scalar).
#
# The TLS stack needs the lpc-ext crypto module (the platform loads no
# extensions by default): pass its path via LPC_EXT_CRYPTO and this
# script generates a config whose modules line loads it. A throwaway
# self-signed P-256 certificate is generated per run into
# src/usr/System/data/tls/ and removed afterwards; nothing key-shaped
# survives the run.
#
# Usage:
#   LPC_EXT_CRYPTO=/path/to/lpc-ext/crypto.<ver> \
#   DGD_BIN=/path/to/dgd/bin/dgd scripts/https-smoke.sh
#
# Probes are driven with openssl s_client rather than curl: the stock
# macOS curl (SecureTransport backend) cannot speak TLS 1.3 and fails
# client-side before sending a byte; openssl is already required for the
# certificate generation and handshakes everywhere. Console phases are
# driven with scripts/drive-verbs.py against an ephemeral verbset
# written under state/.
#
# Exits non-zero on any failed phase.

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

DGDPID=""
SCLIENT_PID=""
cleanup() {
    kill "$SCLIENT_PID" 2>/dev/null || true
    kill "$DGDPID" 2>/dev/null || true
    rm -rf "$TLS_DATA_DIR"
}

echo "== clean slate (base boot + https-app deploy) =="
for mount in Cascade Chat MerryApp MyApp Reload SignalApp WebAuthn WWW testop; do
    rm -rf "src/usr/$mount"
done
rm -f state/snapshot state/snapshot.old state/swap state/https-smoke-boot.log
rm -f src/kernel/data/access.data
rm -rf src/usr/System/log src/usr/Merry/log src/usr/Merry/tmp "$TLS_DATA_DIR"

echo "== deploy https-app as the WWW domain =="
cp -R examples/https-app src/usr/WWW

# Localize example.dgd under state/: point directory at this checkout,
# add a second binary port for TLS, and load the crypto module (the
# optional-module hook -- the checked-in example.dgd stays module-less).
CONFIG=state/https-smoke.dgd
sed -e "s|^directory[	 ]*=.*|directory	= \"$REPO_ROOT/src\";|" \
    -e "s|^binary_port[	 ]*=.*|binary_port	= ([ \"*\" : 8080, \"*\" : $HTTPS_PORT ]);|" \
    example.dgd > "$CONFIG"
printf 'modules\t\t= ([ "%s" : "" ]);\n' "$LPC_EXT_CRYPTO" >> "$CONFIG"

echo "== boot (no certificate yet) =="
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

# drive <verbset-body> -- write an ephemeral verbset and drive it against
# the live console.
drive() {
    printf '%s\n' "$1" > state/https-smoke.verbset
    python3 scripts/drive-verbs.py state/https-smoke.verbset --host "$HOST" --port 8023
}

echo "== phase 1: bootstrap stood down without a certificate =="
if drive 'cmd: tls-cert
expect: certificate /usr/System/data/tls/cert\.pem \(missing\)
expect: TLS stack compiled
expect: https label declared
expect: manager not registered'; then
    echo "PASS: tls-cert reports missing certificate, manager not registered"
else
    echo "FAIL: tls-cert status did not report the certless stand-down" >&2
    rc=1
fi
if tls_request 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' | grep -q '200 OK'; then
    echo "FAIL: HTTPS answered before any certificate existed" >&2
    rc=1
else
    echo "PASS: no HTTPS service before the certificate exists"
fi

echo "== phase 2: generate certificate, activate via tls-cert reload =="
mkdir -p "$TLS_DATA_DIR"
openssl ecparam -name prime256v1 -genkey -noout -out "$TLS_DATA_DIR/key.pem"
openssl req -x509 -key "$TLS_DATA_DIR/key.pem" -out "$TLS_DATA_DIR/cert.pem" \
    -days 2 -subj "/CN=localhost"
if drive 'cmd: tls-cert reload
expect: registered as the https manager

cmd: tls-cert
expect: certificate /usr/System/data/tls/cert\.pem \(present\)
expect: manager registered'; then
    echo "PASS: tls-cert reload activated the manager without a restart"
else
    echo "FAIL: tls-cert reload did not activate the manager" >&2
    rc=1
fi

echo "== phase 3: GET /health over TLS 1.3 =="
response=$(tls_request 'GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' || true)
case "$response" in
    "HTTP/1.1 200 OK"*ok*) echo "PASS: /health answered 200 with \"ok\"" ;;
    *) echo "FAIL: /health answered: $response" >&2; rc=1 ;;
esac

echo "== phase 4: negotiated TLS version =="
tlsline=$( (echo; sleep 2) | openssl s_client -connect "$HOST:$HTTPS_PORT" -tls1_3 -brief -no_ign_eof 2>&1 | grep 'Protocol version' || true)
case "$tlsline" in
    *TLSv1.3*) echo "PASS: $tlsline" ;;
    *) echo "FAIL: TLSv1.3 not negotiated: $tlsline" >&2; rc=1 ;;
esac

echo "== phase 5: POST /echo round-trip =="
response=$(tls_request 'POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: 14\r\nConnection: close\r\n\r\nhello-over-tls' || true)
case "$response" in
    "HTTP/1.1 200 OK"*hello-over-tls) echo "PASS: /echo round-tripped the body" ;;
    *) echo "FAIL: /echo answered: $response" >&2; rc=1 ;;
esac

echo "== phase 6: unknown route answers 404 =="
response=$(tls_request 'GET /no-such-route HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n' || true)
case "$response" in
    "HTTP/1.1 404 Not Found"*) echo "PASS: unknown route answered 404" ;;
    *) echo "FAIL: unknown route answered: $response" >&2; rc=1 ;;
esac

echo "== phase 7: cleartext HTTP against the TLS port fails =="
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

# snapshot_and_scan <label> -- drive the console snapshot verb, wait for
# the dump to land, and scan it for the private key in every in-memory
# representation. The "https" port label is the positive control: it
# lives in the port registry's persistent state, so its absence means
# the scan itself is broken, not that the image is clean.
snapshot_and_scan() {
    if [ -f state/snapshot ]; then
        before=$(wc -c < state/snapshot)
    else
        before=0
    fi
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
            echo "FAIL: snapshot did not land within 30s ($1)" >&2
            return 1
        fi
    done
    python3 - "$1" <<'PYEOF'
import base64, sys
label = sys.argv[1]
lines = open('src/usr/System/data/tls/key.pem').read().splitlines()
b64 = ''.join(l for l in lines if not l.startswith('-----'))
der = base64.b64decode(b64)
i = der.find(b'\x02\x01\x01\x04\x20')
scalar = der[i + 5:i + 37] if i >= 0 else None
snap = open('state/snapshot', 'rb').read()
fails = []
if der in snap:
    fails.append('key DER present in statedump')
if b64.encode() in snap:
    fails.append('key PEM base64 present in statedump')
if scalar and scalar in snap:
    fails.append('key private scalar present in statedump')
if b'https' not in snap:
    fails.append('positive control missing: scan mechanism broken')
if fails:
    print('FAIL (%s): %s' % (label, '; '.join(fails)))
    sys.exit(1)
print('PASS: private key absent from statedump (%s; %d bytes scanned)'
      % (label, len(snap)))
PYEOF
}

echo "== phase 8: statedump scan, idle =="
if ! snapshot_and_scan "idle"; then
    rc=1
fi

echo "== phase 9: statedump scan, live established TLS connection =="
{ printf 'GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n'; sleep 45; } | \
    openssl s_client -connect "$HOST:$HTTPS_PORT" -tls1_3 -quiet \
        -no_ign_eof >/dev/null 2>&1 &
SCLIENT_PID=$!
sleep 3
if ! kill -0 "$SCLIENT_PID" 2>/dev/null; then
    echo "FAIL: live TLS connection did not stay up for the dump" >&2
    rc=1
elif ! snapshot_and_scan "live connection"; then
    rc=1
fi
kill "$SCLIENT_PID" 2>/dev/null || true
SCLIENT_PID=""

kill "$DGDPID" 2>/dev/null || true
DGDPID=""
rm -rf "$TLS_DATA_DIR" src/usr/WWW state/https-smoke.verbset

if [ "$rc" -eq 0 ]; then
    echo "HTTPS-SMOKE PASS"
else
    echo "HTTPS-SMOKE FAIL" >&2
fi
exit "$rc"
