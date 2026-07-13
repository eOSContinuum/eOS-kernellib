#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Measure the platform's baseline timings on this machine.

Boots the platform cold, grows the in-memory image in steps by parking
integer arrays in a scratch object's dataspace, and records at
each step the client-observed snapshot pause (the window between sending
the `snapshot` verb and the console answering again -- the operationally
relevant number, since the driver blocks during the dump). Then stops the
driver cold, times a restore boot against the final snapshot, and drives
sequential HTTP requests against the deployed http-app for a throughput
figure.

With --tls it adds a second capacity figure beside the plain-HTTP one: a
dedicated boot of the reference HTTPS application over the native TLS 1.3
stack, reporting TLS handshake latency and sequential HTTPS throughput on
the labeled https port. That stack runs in interpreted LPC, so the number
is the honest cost of native termination on this machine -- the figure the
retired-reverse-proxy posture stands or falls on.

Every number is wall-clock as observed from outside the driver, on the
machine the report names. Nothing here is a guarantee; it is one measured
run of one workload shape.

Usage:
    DGD_BIN=/path/to/dgd scripts/measure-baseline.py [--sizes 4,12,28]
                                                     [--requests 200]
    LPC_EXT_CRYPTO=/path/to/crypto DGD_BIN=/path/to/dgd \
        scripts/measure-baseline.py --tls [--tls-requests 200]
                                          [--handshakes 50]

  --sizes     cumulative `code` growth calls per step (each call parks
              about 8 MB of integer arrays in the scratch object; the
              default steps land near +34 MB, +100 MB, +235 MB over the
              base image; the snapshot file size printed per step is the
              ground truth)
  --requests  sequential GET /health requests for the throughput figure
  --tls       add the native-TLS capacity figure (needs LPC_EXT_CRYPTO and
              a DGD build carrying the crypto extension ABI)
  --tls-requests  sequential HTTPS GET /health requests for the TLS
                  throughput figure (default 200)
  --handshakes    fresh TLS 1.3 handshakes to time for the latency figure
                  (default 50)

Writes the report to stdout and the raw transcript beside the boot logs
under state/. Run from the repository root, like the other scripts here.
"""

import argparse
import ssl
import sys

sys.dont_write_bytecode = True
import importlib.util
import os
import shutil
import signal
import socket
import subprocess
import time
import urllib.request

HOST = "127.0.0.1"
TELNET_PORT = 8023
HTTP_PORT = 8080
HTTPS_PORT = 8443
TLS_DATA_DIR = "src/usr/System/data/tls"
TLS_CERT = "/usr/System/data/tls/cert.pem"
PARK = "/usr/admin/park"
PARK_SRC = "mixed *d; void park(mixed v) { d += ({ v }); }" \
           " void create() { d = ({ }); }"
CHUNK_EXPR = ("(" + " ".join(
    ['"%s"->park(allocate_int(32767)),' % PARK] * 32) + " 1)")

_dv_spec = importlib.util.spec_from_file_location(
    "drive_verbs", os.path.join(os.path.dirname(__file__), "drive-verbs.py"))
drive_verbs = importlib.util.module_from_spec(_dv_spec)
_dv_spec.loader.exec_module(drive_verbs)


def clean_slate(root):
    for mount in ("Cascade", "Chat", "MerryApp", "MyApp", "Reload",
                  "SignalApp", "WWW", "testop"):
        shutil.rmtree(os.path.join(root, "src/usr", mount),
                      ignore_errors=True)
    for f in ("snapshot", "snapshot.old", "swap", "measure-boot1.log",
              "measure-boot2.log", "measure.dgd", "measure-transcript.log"):
        try:
            os.remove(os.path.join(root, "state", f))
        except FileNotFoundError:
            pass
    try:
        os.remove(os.path.join(root, "src/kernel/data/access.data"))
    except FileNotFoundError:
        pass
    for d in ("src/usr/System/log", "src/usr/Merry/log", "src/usr/Merry/tmp"):
        shutil.rmtree(os.path.join(root, d), ignore_errors=True)
    shutil.copytree(os.path.join(root, "examples/http-app"),
                    os.path.join(root, "src/usr/WWW"))


def write_config(root):
    src = os.path.join(root, "example.dgd")
    dst = os.path.join(root, "state", "measure.dgd")
    with open(src) as f, open(dst, "w") as out:
        for line in f:
            if line.startswith("directory"):
                line = 'directory\t= "%s";\n' % os.path.join(root, "src")
            elif line.startswith("sector_size"):
                line = "sector_size\t= 8192;\t/* raised for measurement" \
                       " runs: the stock build caps swap_size at 65535" \
                       " sectors, so capacity scales via sector size */\n"
            out.write(line)
    return dst


def wait_port(port, proc, deadline=30.0):
    start = time.monotonic()
    while time.monotonic() - start < deadline:
        if proc.poll() is not None:
            raise SystemExit("driver exited during boot; read the boot log")
        try:
            socket.create_connection((HOST, port), 1).close()
            return time.monotonic() - start
        except OSError:
            time.sleep(0.05)
    raise SystemExit("port %d not accepting within %ds" % (port, deadline))


def roundtrip(sess, timeout=600.0):
    """One code round trip; the driver answers only after any pending
    end-of-task work (a queued dump) completes."""
    t0 = time.monotonic()
    sess.send_line("code 1")
    sess.read_until([r"\$\d+ = 1"], timeout=timeout)
    return time.monotonic() - t0


def snapshot_pause(sess, root):
    """Client-observed dump cost: issue snapshot, then time the next
    round trip (which the dump blocks), minus a no-dump baseline."""
    base = roundtrip(sess)
    sess.send_line("snapshot")
    sess.read_until([r"#"], timeout=15.0)
    blocked = roundtrip(sess)
    size = os.path.getsize(os.path.join(root, "state/snapshot"))
    return max(0.0, blocked - base), size


def grow(sess, calls):
    for _ in range(calls):
        sess.send_line("code " + CHUNK_EXPR)
        text = sess.read_until([r"\$\d+ = 1"], timeout=30.0)
        if "Error" in text:
            raise SystemExit("growth call failed: " + text.strip()[-200:])


def status_line(sess, label):
    sess.send_line("status")
    text = sess.read_until([r"Uptime"], timeout=15.0)
    for ln in text.splitlines():
        if "dynamic:" in ln:
            print("  [%s] %s" % (label, ln.strip()))


def http_throughput(n):
    t0 = time.monotonic()
    ok = 0
    for _ in range(n):
        with urllib.request.urlopen(
                "http://%s:%d/health" % (HOST, HTTP_PORT), timeout=5) as r:
            if r.status == 200:
                ok += 1
    dt = time.monotonic() - t0
    return ok, n, dt


def tls_client_context():
    """A TLS 1.3 client that accepts the run's self-signed cert. The point
    is to time the server's handshake and request handling, not to verify
    a chain, so hostname and trust checks are off by design."""
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    return ctx


def write_tls_config(root):
    """example.dgd localized for a TLS boot: a second binary port for the
    https label and the crypto module loaded (the checked-in example.dgd
    stays module-less; the module path comes from LPC_EXT_CRYPTO)."""
    module = os.environ.get("LPC_EXT_CRYPTO")
    if not module or not os.path.isfile(module):
        raise SystemExit("--tls needs LPC_EXT_CRYPTO=/path/to/crypto module"
                         " (build with `make crypto` in the lpc-ext worktree)")
    src = os.path.join(root, "example.dgd")
    dst = os.path.join(root, "state", "measure-tls.dgd")
    with open(src) as f, open(dst, "w") as out:
        for line in f:
            if line.startswith("directory"):
                line = 'directory\t= "%s";\n' % os.path.join(root, "src")
            elif line.startswith("binary_port"):
                line = ('binary_port\t= ([ "*" : %d, "*" : %d ]);\n'
                        % (HTTP_PORT, HTTPS_PORT))
            out.write(line)
        out.write('modules\t\t= ([ "%s" : "" ]);\n' % module)
    return dst


def deploy_https(root):
    """Deploy the reference HTTPS application as the WWW domain."""
    shutil.rmtree(os.path.join(root, "src/usr/WWW"), ignore_errors=True)
    shutil.copytree(os.path.join(root, "examples/https-app"),
                    os.path.join(root, "src/usr/WWW"))


def generate_cert(root):
    """A short-lived self-signed ECDSA P-256 cert at the configured paths,
    exactly as https-smoke.sh provisions one."""
    d = os.path.join(root, TLS_DATA_DIR)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    key = os.path.join(d, "key.pem")
    cert = os.path.join(d, "cert.pem")
    subprocess.run(["openssl", "ecparam", "-name", "prime256v1", "-genkey",
                    "-noout", "-out", key], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["openssl", "req", "-x509", "-key", key, "-out", cert,
                    "-days", "2", "-subj", "/CN=localhost"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def tls_reload(sess):
    """Drive the operator tls-cert reload verb; the https manager registers
    on the labeled port without a restart."""
    sess.send_line("tls-cert reload")
    sess.read_until([r"registered as the https manager"], timeout=15.0)


def tls_handshake_latency(n, ctx):
    """Time n fresh TLS 1.3 handshakes (new TCP connection each), returning
    the per-handshake wall-clock seconds."""
    times = []
    for _ in range(n):
        raw = socket.create_connection((HOST, HTTPS_PORT), 5)
        t0 = time.monotonic()
        tls = ctx.wrap_socket(raw, server_hostname=HOST)
        times.append(time.monotonic() - t0)
        tls.close()
    return times


def https_throughput(n, ctx):
    t0 = time.monotonic()
    ok = 0
    for _ in range(n):
        with urllib.request.urlopen(
                "https://%s:%d/health" % (HOST, HTTPS_PORT),
                timeout=5, context=ctx) as r:
            if r.status == 200:
                ok += 1
    dt = time.monotonic() - t0
    return ok, n, dt


def measure_tls(dgd, root, tls_requests, handshakes):
    """Boot the HTTPS reference app over native TLS, activate the cert via
    the operator reload verb, and report handshake latency and throughput
    on the labeled https port, beside the plain-HTTP figure above."""
    print("== native TLS: clean slate, https-app deployed as WWW ==")
    clean_slate(root)
    deploy_https(root)
    config = write_tls_config(root)

    boot_log = open(os.path.join(root, "state/measure-tls-boot.log"), "w")
    proc = subprocess.Popen([dgd, config], stdout=boot_log, stderr=boot_log,
                            cwd=root)
    try:
        t_boot = wait_port(TELNET_PORT, proc)
        print("cold boot to console-ready (TLS build): %.2fs" % t_boot)

        sess = drive_verbs.Session(
            HOST, TELNET_PORT,
            os.path.join(root, "state/measure-tls-transcript.log"))
        drive_verbs.login(sess, "admin", "drive-verbs")

        # The https manager stands down until a certificate is present;
        # provision one and activate it through hot reload.
        generate_cert(root)
        tls_reload(sess)
        wait_port(HTTPS_PORT, proc)

        ctx = tls_client_context()
        hs = tls_handshake_latency(handshakes, ctx)
        hs_sorted = sorted(hs)
        mean = sum(hs) / len(hs)
        median = hs_sorted[len(hs) // 2]
        print("TLS 1.3 handshake latency over %d handshakes: mean %.1f ms,"
              " median %.1f ms, min %.1f ms, max %.1f ms"
              % (len(hs), mean * 1e3, median * 1e3,
                 hs_sorted[0] * 1e3, hs_sorted[-1] * 1e3))

        ok, n, dt = https_throughput(tls_requests, ctx)
        print("https-app GET /health over TLS 1.3: %d/%d ok in %.2fs"
              " (%.0f req/s, sequential one-connection-per-request)"
              % (ok, n, dt, n / dt))

        sess.send_line("shutdown")
        time.sleep(1.0)
        sess.close()
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        boot_log.close()
        shutil.rmtree(os.path.join(root, TLS_DATA_DIR), ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", default="4,12,28")
    ap.add_argument("--requests", type=int, default=200)
    ap.add_argument("--tls", action="store_true")
    ap.add_argument("--tls-requests", type=int, default=200)
    ap.add_argument("--handshakes", type=int, default=50)
    args = ap.parse_args()
    steps = [int(x) for x in args.sizes.split(",")]

    dgd = os.environ.get("DGD_BIN") or shutil.which("dgd")
    if not dgd or not os.access(dgd, os.X_OK):
        raise SystemExit("set DGD_BIN=/path/to/dgd")
    root = os.getcwd()

    print("== measure-baseline: clean slate, http-app deployed as WWW ==")
    clean_slate(root)
    config = write_config(root)

    boot_log = open(os.path.join(root, "state/measure-boot1.log"), "w")
    proc = subprocess.Popen([dgd, config], stdout=boot_log, stderr=boot_log)
    t_boot = wait_port(TELNET_PORT, proc)
    print("cold boot to console-ready: %.2fs" % t_boot)

    sess = drive_verbs.Session(HOST, TELNET_PORT,
                               os.path.join(root,
                                            "state/measure-transcript.log"))
    drive_verbs.login(sess, "admin", "drive-verbs")

    sess.send_line('code compile_object("%s", "%s")' % (PARK, PARK_SRC))
    sess.read_until([r"\$\d+ = "], timeout=15.0)

    status_line(sess, "base image")
    pause, size = snapshot_pause(sess, root)
    print("snapshot pause, base image: %.3fs (snapshot file %.1f MB)"
          % (pause, size / 1e6))

    done = 0
    for cum in steps:
        grow(sess, cum - done)
        done = cum
        status_line(sess, "+%d chunks (~%d MB parked)" % (cum, cum * 8))
        pause, size = snapshot_pause(sess, root)
        print("snapshot pause at +%d chunks: %.3fs (snapshot file %.1f MB)"
              % (cum, pause, size / 1e6))

    sess.send_line("shutdown")
    time.sleep(1.0)
    sess.close()
    proc.wait(timeout=30)
    boot_log.close()

    boot_log2 = open(os.path.join(root, "state/measure-boot2.log"), "w")
    t0 = time.monotonic()
    proc2 = subprocess.Popen([dgd, config, "state/snapshot"],
                             stdout=boot_log2, stderr=boot_log2, cwd=root)
    t_restore = wait_port(TELNET_PORT, proc2)
    print("restore boot to console-ready (final image): %.2fs" % t_restore)

    ok, n, dt = http_throughput(args.requests)
    print("http-app GET /health: %d/%d ok in %.2fs (%.0f req/s, sequential"
          " one-connection-per-request)" % (ok, n, dt, n / dt))

    proc2.send_signal(signal.SIGINT)
    try:
        proc2.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc2.kill()
    boot_log2.close()

    if args.tls:
        measure_tls(dgd, root, args.tls_requests, args.handshakes)

    print("== done; transcript and boot logs under state/ ==")


if __name__ == "__main__":
    main()
