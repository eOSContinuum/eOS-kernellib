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

Two standalone shapes measure the platform under parallel clients instead
of the sequential loop. Each boots clean-slate instances with the
http-app deployed and skips the growth/snapshot phases; giving either
flag runs only the requested shapes:

    --concurrent  for each client count, that many parallel clients each
                  drive a serial loop of GET /health requests; reports
                  aggregate requests/second and per-request latency.
    --headline    head-of-line probe: one background client samples
                  /health latency at a modest fixed rate while the admin
                  console injects a burst of busy tasks, each an
                  empty-loop calibrated live against the default 20M-tick
                  budget so it completes rather than erroring; reports
                  the latency the probing client observes before, during,
                  and after the burst -- the serialization price
                  docs/execution-model.md describes.

Both parallel shapes budget their TOTAL request count per boot, and
--concurrent boots a fresh instance per client count. The budget is a
regression detector, not a workaround: the example servers release each
connection's user slot when the response completes (doneRequest), so
slots recycle -- but a release regression would otherwise walk the run
into the config `users` cap, where the driver stops servicing ALL ports
silently (new connections are accepted by the kernel but never
answered). Bounding the per-boot request count keeps that failure mode
loud (this rig refusing to exceed its budget) instead of silent, and
the users= evidence line printed beside each measurement is the live
check that slots are being reclaimed.

Every number is wall-clock as observed from outside the driver, on the
machine the report names. Nothing here is a guarantee; it is one measured
run of one workload shape.

Usage:
    DGD_BIN=/path/to/dgd scripts/measure-baseline.py [--sizes 4,12,28]
                                                     [--requests 200]
    LPC_EXT_CRYPTO=/path/to/crypto DGD_BIN=/path/to/dgd \
        scripts/measure-baseline.py --tls [--tls-requests 200]
                                          [--handshakes 50]
    DGD_BIN=/path/to/dgd scripts/measure-baseline.py --concurrent 2,8,32
                                            [--concurrent-requests 100]
    DGD_BIN=/path/to/dgd scripts/measure-baseline.py --headline

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
  --concurrent    comma-separated parallel client counts (e.g. 2,8,32);
                  each client is a serial loop of fresh-connection
                  requests, so the count is the number of connections in
                  flight; each count gets its own boot
  --concurrent-requests  GET /health requests per client per count;
                         default 0 = auto, splitting the per-boot user
                         slot budget across the clients
  --headline      run the head-of-line probe

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
import threading
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
BURN = "/usr/admin/burn"
BURN_SRC = "int burn(int n) { int i; for (i = 0; i < n; i++) { } return i; }"
HEALTH_URL = "http://%s:%d/health" % (HOST, HTTP_PORT)
STATUS_URL = "http://%s:%d/status" % (HOST, HTTP_PORT)
PROBE_INTERVAL = 0.08		# head-of-line probe pacing between samples
HEADLINE_QUIET = 3.0		# quiet sampling before and after the burst
HEADLINE_WINDOW = 2.0		# target injection-burst duration
USER_BUDGET = 220		# per-boot request bound: a slot-release
				# regression in the deployed example would
				# wedge the driver at the 255-slot users
				# cap (see the docstring); bounding here
				# keeps that failure loud, with margin for
				# warm-up, status reads, and the console
WARM_REQUESTS = 4		# warm-up requests (each spends a slot)

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
              "measure-boot2.log", "measure.dgd", "measure-transcript.log",
              "measure-concurrent-boot.log", "measure-headline-boot.log",
              "measure-headline-transcript.log"):
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


def latency_stats(times):
    """(median, p95, max) over per-request wall-clock seconds."""
    s = sorted(times)
    return (s[len(s) // 2], s[min(len(s) - 1, (len(s) * 95) // 100)], s[-1])


def warm_http(proc, deadline=30.0):
    """Wait until the deployed http-app answers GET /health (the binary
    port accepts before the WWW domain finishes compiling on a cold boot),
    then run a short sequential warm-up so the measured window never pays
    first-request compilation."""
    start = time.monotonic()
    while True:
        if proc.poll() is not None:
            raise SystemExit("driver exited during warm-up; read the"
                             " boot log")
        try:
            with urllib.request.urlopen(HEALTH_URL, timeout=2) as r:
                if r.status == 200:
                    break
        except OSError:
            pass
        if time.monotonic() - start > deadline:
            raise SystemExit("http-app not answering within %ds" % deadline)
        time.sleep(0.1)
    for _ in range(WARM_REQUESTS - 1):
        urllib.request.urlopen(HEALTH_URL, timeout=5).close()


def users_line():
    """The deployed app's users=used/cap headroom line, printed beside
    each measurement as live evidence that connection slots recycle."""
    with urllib.request.urlopen(STATUS_URL, timeout=5) as r:
        body = r.read().decode("ascii", "replace")
    for ln in body.splitlines():
        if ln.startswith("users="):
            return ln
    return "users=?"


def concurrent_load(n, per_client):
    """n parallel clients, each a serial loop of per_client GET /health
    requests on a fresh connection (the server closes per response, so n
    is the number of connections in flight). Returns (ok, errors, wall
    seconds, per-request latency list)."""
    barrier = threading.Barrier(n + 1)
    lat = [[] for _ in range(n)]
    err = [0] * n

    def client(k):
        barrier.wait()
        for _ in range(per_client):
            t0 = time.monotonic()
            try:
                with urllib.request.urlopen(HEALTH_URL, timeout=15) as r:
                    if r.status == 200:
                        lat[k].append(time.monotonic() - t0)
                    else:
                        err[k] += 1
            except OSError:
                err[k] += 1

    threads = [threading.Thread(target=client, args=(k,)) for k in range(n)]
    for t in threads:
        t.start()
    barrier.wait()
    t0 = time.monotonic()
    for t in threads:
        t.join()
    wall = time.monotonic() - t0
    times = [x for per in lat for x in per]
    return len(times), sum(err), wall, times


def measure_concurrent(dgd, root, counts, per_client):
    """Drive parallel GET /health load at each client count, reporting
    aggregate throughput and per-request latency. Each count gets its own
    clean-slate boot, and the request total stays inside USER_BUDGET (the
    slot-release regression bound; docstring)."""
    for n in counts:
        clients = per_client or max(1, (USER_BUDGET - WARM_REQUESTS) // n)
        print("== concurrent load, %d clients: clean slate, http-app"
              " deployed as WWW ==" % n)
        clean_slate(root)
        config = write_config(root)

        boot_log = open(os.path.join(root,
                                     "state/measure-concurrent-boot.log"),
                        "w")
        proc = subprocess.Popen([dgd, config], stdout=boot_log,
                                stderr=boot_log, cwd=root)
        try:
            t_boot = wait_port(TELNET_PORT, proc)
            print("cold boot to console-ready: %.2fs" % t_boot)
            warm_http(proc)

            ok, errors, wall, times = concurrent_load(n, clients)
            if not times:
                print("  %2d clients: no successful requests (%d errors)"
                      % (n, errors))
                continue
            med, p95, worst = latency_stats(times)
            line = ("  %2d clients x %d requests: %d ok in %.2fs"
                    " (%.0f req/s aggregate; latency median %.1f ms,"
                    " p95 %.1f ms, max %.1f ms)"
                    % (n, clients, ok, wall, ok / wall,
                       med * 1e3, p95 * 1e3, worst * 1e3))
            if errors:
                line += " [%d ERRORS]" % errors
            print(line)
            print("  %2d clients: post-run %s" % (n, users_line()))
        finally:
            if proc.poll() is None:
                proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
            boot_log.close()


def calibrate_burn(sess):
    """Size the busy task against the documented default budget of
    20,000,000 ticks per execution: probe rising empty-loop iteration
    counts through the console `code` verb until the driver answers Out
    of ticks, and keep the largest count that completed. Returns
    (iterations, round-trip seconds) for the chosen size."""
    sess.send_line('code compile_object("%s", "%s")' % (BURN, BURN_SRC))
    sess.read_until([r"\$\d+ = "], timeout=15.0)

    chosen = None
    hit_ceiling = False
    print("burn calibration against the default 20M-tick budget:")
    for n in (100000, 300000, 1000000, 1500000, 2000000, 2500000,
              3000000, 4000000, 6000000, 10000000):
        t0 = time.monotonic()
        sess.send_line('code "%s"->burn(%d)' % (BURN, n))
        # a budget hit surfaces as a bare "Out of ticks" plus a stack
        # trace, not through cmd_code's Error: prefix -- the whole input
        # task is aborted, so the catch never runs
        text = sess.read_until([r"\$\d+ = \d+", r"Out of ticks",
                                r"Error:"], timeout=60.0)
        dt = time.monotonic() - t0
        if "Out of ticks" in text or "Error" in text:
            print("  %8d iterations: out of ticks (task aborted after"
                  " %.1f ms of driver time)" % (n, dt * 1e3))
            hit_ceiling = True
            break
        print("  %8d iterations: completes in %.1f ms" % (n, dt * 1e3))
        chosen = (n, dt)
    if chosen is None:
        raise SystemExit("even the smallest burn ran out of ticks")
    if not hit_ceiling:
        print("  (budget ceiling not reached in the probed range)")
    # drain one round trip so a trailing error trace or prompt never
    # bleeds into the burst's reads
    sess.send_line("code 1")
    sess.read_until([r"\$\d+ = 1"], timeout=15.0)
    print("  -> burn size %d iterations, ~%.0f ms per task"
          % (chosen[0], chosen[1] * 1e3))
    return chosen


def measure_headline(dgd, root):
    """Boot a clean slate with the http-app deployed, then measure what a
    steadily-probing HTTP client observes while the admin console runs a
    burst of near-budget busy tasks back to back: the head-of-line
    latency price docs/execution-model.md describes, reported before /
    during / after the burst."""
    print("== head-of-line probe: clean slate, http-app deployed as WWW ==")
    clean_slate(root)
    config = write_config(root)

    boot_log = open(os.path.join(root, "state/measure-headline-boot.log"),
                    "w")
    proc = subprocess.Popen([dgd, config], stdout=boot_log, stderr=boot_log,
                            cwd=root)
    try:
        t_boot = wait_port(TELNET_PORT, proc)
        print("cold boot to console-ready: %.2fs" % t_boot)
        warm_http(proc)

        sess = drive_verbs.Session(
            HOST, TELNET_PORT,
            os.path.join(root, "state/measure-headline-transcript.log"))
        drive_verbs.login(sess, "admin", "drive-verbs")
        burn_n, burn_wall = calibrate_burn(sess)

        samples = []		# (start, duration, ok) per probe request
        stop = threading.Event()

        def probe():
            # bounded like every shape: a slot-release regression should
            # stop the rig loudly, never wedge the driver silently
            while not stop.is_set() and len(samples) < USER_BUDGET - 20:
                t0 = time.monotonic()
                ok = 0
                try:
                    with urllib.request.urlopen(HEALTH_URL, timeout=30) as r:
                        ok = int(r.status == 200)
                except OSError:
                    pass
                samples.append((t0, time.monotonic() - t0, ok))
                stop.wait(PROBE_INTERVAL)

        prober = threading.Thread(target=probe)
        prober.start()
        time.sleep(HEADLINE_QUIET)

        win_start = time.monotonic()
        burn_walls = []
        while time.monotonic() - win_start < HEADLINE_WINDOW:
            t0 = time.monotonic()
            sess.send_line('code "%s"->burn(%d)' % (BURN, burn_n))
            text = sess.read_until([r"\$\d+ = \d+", r"Out of ticks",
                                    r"Error:"], timeout=60.0)
            if "Out of ticks" in text or "Error" in text:
                raise SystemExit("burn errored mid-burst: "
                                 + text.strip()[-200:])
            burn_walls.append(time.monotonic() - t0)
        win_end = time.monotonic()
        time.sleep(HEADLINE_QUIET)
        stop.set()
        prober.join()
        slots = users_line()

        sess.send_line("shutdown")
        time.sleep(1.0)
        sess.close()

        bw = sorted(burn_walls)
        print("injected %d busy tasks of %d iterations over %.2fs"
              " (per-task wall median %.1f ms, max %.1f ms)"
              % (len(bw), burn_n, win_end - win_start,
                 bw[len(bw) // 2] * 1e3, bw[-1] * 1e3))
        buckets = (
            ("before", [s for s in samples if s[0] + s[1] < win_start]),
            ("during", [s for s in samples
                        if s[0] <= win_end and s[0] + s[1] >= win_start]),
            ("after", [s for s in samples if s[0] > win_end]))
        for label, bucket in buckets:
            times = [s[1] for s in bucket if s[2]]
            errs = len(bucket) - len(times)
            if not times:
                print("  %-6s: no successful samples (%d errors)"
                      % (label, errs))
                continue
            med, p95, worst = latency_stats(times)
            line = ("  %-6s: %3d samples, latency median %.1f ms,"
                    " p95 %.1f ms, max %.1f ms"
                    % (label, len(times), med * 1e3, p95 * 1e3, worst * 1e3))
            if errs:
                line += " [%d ERRORS]" % errs
            print(line)
        print("  post-run %s" % slots)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        boot_log.close()


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
    ap.add_argument("--concurrent", default=None,
                    help="comma-separated parallel client counts, e.g."
                         " 2,8,32; runs the concurrent shape only")
    ap.add_argument("--concurrent-requests", type=int, default=0,
                    help="requests per client (default 0 = split the"
                         " per-boot user slot budget across the clients)")
    ap.add_argument("--headline", action="store_true",
                    help="run the head-of-line probe only")
    args = ap.parse_args()
    steps = [int(x) for x in args.sizes.split(",")]

    dgd = os.environ.get("DGD_BIN") or shutil.which("dgd")
    if not dgd or not os.access(dgd, os.X_OK):
        raise SystemExit("set DGD_BIN=/path/to/dgd")
    root = os.getcwd()

    if args.concurrent or args.headline:
        if args.concurrent:
            counts = [int(x) for x in args.concurrent.split(",")]
            measure_concurrent(dgd, root, counts, args.concurrent_requests)
        if args.headline:
            measure_headline(dgd, root)
        print("== done; transcript and boot logs under state/ ==")
        return

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
