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

Every number is wall-clock as observed from outside the driver, on the
machine the report names. Nothing here is a guarantee; it is one measured
run of one workload shape.

Usage:
    DGD_BIN=/path/to/dgd scripts/measure-baseline.py [--sizes 4,12,28]
                                                     [--requests 200]

  --sizes     cumulative `code` growth calls per step (each call parks
              about 8 MB of integer arrays in the scratch object; the
              default steps land near +34 MB, +100 MB, +235 MB over the
              base image; the snapshot file size printed per step is the
              ground truth)
  --requests  sequential GET /health requests for the throughput figure

Writes the report to stdout and the raw transcript beside the boot logs
under state/. Run from the repository root, like the other scripts here.
"""

import argparse
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sizes", default="4,12,28")
    ap.add_argument("--requests", type=int, default=200)
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
    print("== done; transcript and boot logs under state/ ==")


if __name__ == "__main__":
    main()
