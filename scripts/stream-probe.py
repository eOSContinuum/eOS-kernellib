#!/usr/bin/env python3
"""Assert the streaming-connection contract against a live driver.

Driven by scripts/stream-smoke.sh, which deploys examples/composite-app
without its self-exiting test driver and boots a plain-HTTP runtime.
This script makes the assertions; the shell script owns the runtime.

Three properties, each measured mid-run with the driver verified alive:

  departure   a stream whose peer departs (FIN, and separately RST) is
              released PROMPTLY -- the peer's close is observed, not
              waited out. Paired with a non-streaming presence control
              on the same runtime, so an absence result cannot pass for
              an observation.
  survival    a healthy stream, peer connected and readable and sending
              nothing, is STILL ALIVE well past the inactivity backstop.
  contract    a client that sends bytes on a streaming connection is
              DISCONNECTED, promptly and deliberately, rather than
              having them ignored or parsed as a new request.

WINDOW SIZING IS LOAD-BEARING IN BOTH DIRECTIONS. The HTTP layer applies
a 60-second inactivity backstop (Server1.c inactivityTimeout). A
departure bound above it would pass on the backstop firing instead of on
the close being seen, and a survival window below it would pass without
ever reaching the thing it tests. So DEPART_MAX_S sits well under the
backstop and SURVIVE_S well over it, and neither is a tuning knob: move
either toward 60 and the assertion stops meaning what its name says.

The connection object's lifetime is read from the kernel `users` count
via the admin console `status` verb, not from the application's view --
an application-side subscription and the connection object it rides on
have different lifetimes, and only the latter holds a `users` slot.
"""

import argparse
import importlib.util
import os
import re
import socket
import struct
import sys
import time

# The HTTP layer's inactivity backstop, seconds (Server1.c
# inactivityTimeout). Every bound below is stated relative to it.
BACKSTOP_S = 60.0

# A departed peer must be released within this many seconds. Well under
# BACKSTOP_S so the assertion cannot be satisfied by the backstop.
DEPART_MAX_S = 20.0

# A healthy silent stream must still be alive after this long. Well over
# BACKSTOP_S so the assertion actually reaches the backstop.
SURVIVE_S = 90.0

# After a client sends bytes on a stream, the server must close within
# this many seconds. Well under BACKSTOP_S for the same reason as
# DEPART_MAX_S.
CONTRACT_MAX_S = 20.0

POLL_S = 2.0


def load_console(scripts_dir):
    """Load drive-verbs.py, whose filename is not importable as a module."""
    spec = importlib.util.spec_from_file_location(
        "drive_verbs", os.path.join(scripts_dir, "drive-verbs.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Console:
    """The admin console, used only to read the kernel users count."""

    def __init__(self, dv, host, port, transcript):
        self.dv = dv
        self.sess = dv.Session(host, port, transcript)
        dv.login(self.sess, "admin", "drive-verbs")

    def users(self):
        self.sess.send_line("status")
        text = self.sess.read_until([self.dv.PROMPT])
        m = re.search(r"Users:\s+(\d+)\s*/\s*(\d+)", text)
        if not m:
            raise RuntimeError("no Users line in status output:\n" + text)
        return int(m.group(1))

    def close(self):
        try:
            self.sess.send_line("quit")
        except Exception:
            pass
        self.sess.close()


def driver_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def open_stream(host, port, path):
    """Open an SSE subscription and read its response head.

    Returns (sock, head). Raises if the response is not a stream, so a
    misrouted request fails loudly instead of being measured."""
    s = socket.create_connection((host, port), timeout=5.0)
    s.sendall(f"GET {path} HTTP/1.1\r\n"
              f"Host: {host}:{port}\r\n"
              f"Accept: text/event-stream\r\n\r\n".encode("ascii"))
    s.settimeout(5.0)
    head = b""
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline and b"\r\n\r\n" not in head:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        head += chunk
    text = head.decode("ascii", errors="replace")
    if "text/event-stream" not in text:
        s.close()
        raise RuntimeError(f"{path} did not answer with a stream:\n{text}")
    return s, text


def open_plain(host, port, path, rst):
    """Open a non-streaming connection that stays bound and unanswered.

    The request LINE is sent but the header block is never terminated, so
    the server has bound a user slot (the kernel binds on the first
    delivered line) and is waiting in line mode for the rest. That is
    what makes this a usable control: a completed request would be
    answered and closed before the users count could be sampled, leaving
    a release that was never observed to rise. Here the rise is
    observable, the connection is non-streaming, and its input is
    unblocked -- the state in which a peer's close is supposed to be
    noticed."""
    s = socket.create_connection((host, port), timeout=5.0)
    if rst:
        # onoff=1, linger=0 -> close() sends RST rather than FIN
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                     struct.pack("ii", 1, 0))
    s.sendall(f"GET {path} HTTP/1.1\r\n"
              f"Host: {host}:{port}\r\n".encode("ascii"))
    return s


def settle(console, pid, limit=30.0):
    """Wait for the users count to stop moving, and return it.

    Cases run back to back against one runtime, so a connection still
    draining from the previous case would be read as this case's
    baseline. Two consecutive equal samples is the settled condition."""
    prev = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit:
        if not driver_alive(pid):
            raise RuntimeError("settle: driver exited")
        n = console.users()
        if n == prev:
            return n
        prev = n
        time.sleep(POLL_S)
    return prev


def wait_for_release(console, pid, baseline, limit, label):
    """Poll until the users count returns to baseline, or limit expires.

    Returns the elapsed seconds at release, or None."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit:
        if not driver_alive(pid):
            raise RuntimeError(f"{label}: driver exited mid-measurement")
        if console.users() <= baseline:
            return round(time.monotonic() - t0, 1)
        time.sleep(POLL_S)
    return None


def case_departure(console, pid, host, port, path, rst, results):
    """A stream whose peer departs must be released promptly."""
    kind = "RST" if rst else "FIN"
    name = f"departure-{kind.lower()}"
    print(f"\n--- {name} ---")
    base = settle(console, pid)
    sock, _ = open_stream(host, port, path)
    if rst:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                        struct.pack("ii", 1, 0))
    held = console.users()
    print(f"  users {base} -> {held} with the stream held")
    if held <= base:
        results.append((name, False,
                        "opening the stream did not raise the users count;"
                        " the probe is not seeing this connection"))
        sock.close()
        return
    sock.close()
    print(f"  peer closed with {kind}; must be released within"
          f" {DEPART_MAX_S:.0f}s (backstop is {BACKSTOP_S:.0f}s)")
    when = wait_for_release(console, pid, base, DEPART_MAX_S, name)
    if when is None:
        results.append((name, False,
                        f"still held after {DEPART_MAX_S:.0f}s -- the close"
                        f" was not observed; only the {BACKSTOP_S:.0f}s"
                        f" backstop would release it"))
    else:
        results.append((name, True, f"released at ~{when}s, the close observed"))


def case_control(console, pid, host, port, path, rst, results):
    """Presence control: a non-streaming request reaps promptly."""
    kind = "RST" if rst else "FIN"
    name = f"control-{kind.lower()}"
    print(f"\n--- {name} (presence control) ---")
    base = settle(console, pid)
    sock = open_plain(host, port, path, rst)
    held = None
    t0 = time.monotonic()
    while time.monotonic() - t0 < 10.0:
        held = console.users()
        if held > base:
            break
        time.sleep(0.5)
    print(f"  users {base} -> {held} with the request line sent, headers open")
    if held is None or held <= base:
        results.append((name, False,
                        "a non-streaming connection never raised the users"
                        " count, so this control demonstrates nothing about"
                        " what the probe can observe"))
        sock.close()
        return
    sock.close()
    when = wait_for_release(console, pid, base, DEPART_MAX_S, name)
    if when is None:
        results.append((name, False,
                        "a NON-streaming connection was not released either;"
                        " the probe cannot observe a reap, so no absence"
                        " result in this run means anything"))
    else:
        results.append((name, True, f"rose then released at ~{when}s"))


def case_survival(console, pid, host, port, path, results):
    """A healthy silent stream must outlive the inactivity backstop."""
    name = "survival"
    print(f"\n--- {name} ---")
    base = settle(console, pid)
    sock, _ = open_stream(host, port, path)
    print(f"  peer stays connected, readable, and sends nothing;"
          f" must still be alive at {SURVIVE_S:.0f}s"
          f" (backstop is {BACKSTOP_S:.0f}s)")
    sock.settimeout(0.2)
    t0 = time.monotonic()
    closed_at = None
    pushed = 0
    while time.monotonic() - t0 < SURVIVE_S:
        if not driver_alive(pid):
            raise RuntimeError(f"{name}: driver exited mid-measurement")
        try:
            chunk = sock.recv(4096)
            if chunk == b"":
                closed_at = round(time.monotonic() - t0, 1)
                break
            pushed += len(chunk)
        except socket.timeout:
            pass
        except OSError:
            closed_at = round(time.monotonic() - t0, 1)
            break
        time.sleep(POLL_S)
    still_held = console.users() > base
    sock.close()
    if closed_at is not None:
        results.append((name, False,
                        f"the SERVER dropped a healthy stream at"
                        f" t+{closed_at}s ({pushed} bytes pushed first)"))
    elif not still_held:
        results.append((name, False,
                        "the connection object was released while the peer"
                        " was still connected"))
    else:
        results.append((name, True,
                        f"alive at {SURVIVE_S:.0f}s with the peer connected"))


def case_contract(console, pid, host, port, path, results):
    """Bytes from the client on a stream must disconnect the connection."""
    name = "contract"
    print(f"\n--- {name} ---")
    base = settle(console, pid)
    sock, _ = open_stream(host, port, path)
    held = console.users()
    print(f"  users {base} -> {held} with the stream held")
    if held <= base:
        results.append((name, False, "the probe is not seeing this connection"))
        sock.close()
        return
    # A well-formed pipelined request: the shape a client would actually
    # send, and the one the state machine would parse as a new request
    # line if the contract were not enforced.
    sock.sendall(f"GET /inventory/health HTTP/1.1\r\n"
                 f"Host: {host}:{port}\r\n\r\n".encode("ascii"))
    print(f"  client sent a request on the open stream; the server must"
          f" disconnect within {CONTRACT_MAX_S:.0f}s")
    sock.settimeout(0.2)
    t0 = time.monotonic()
    closed_at = None
    extra = b""
    while time.monotonic() - t0 < CONTRACT_MAX_S:
        if not driver_alive(pid):
            raise RuntimeError(f"{name}: driver exited mid-measurement")
        try:
            chunk = sock.recv(4096)
            if chunk == b"":
                closed_at = round(time.monotonic() - t0, 1)
                break
            extra += chunk
        except socket.timeout:
            pass
        except OSError:
            closed_at = round(time.monotonic() - t0, 1)
            break
        time.sleep(0.5)
    sock.close()
    if closed_at is None:
        results.append((name, False,
                        f"the server neither disconnected nor answered within"
                        f" {CONTRACT_MAX_S:.0f}s -- the bytes were ignored"
                        f" ({len(extra)} bytes came back)"))
    else:
        # Released promptly is the contract; the users count confirms the
        # connection object went with it rather than lingering.
        when = wait_for_release(console, pid, base, DEPART_MAX_S, name)
        if when is None:
            results.append((name, False,
                            f"the socket closed at t+{closed_at}s but the"
                            f" connection object was still held"
                            f" {DEPART_MAX_S:.0f}s later"))
        else:
            results.append((name, True,
                            f"disconnected at t+{closed_at}s, object released"
                            f" ~{when}s later"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--http-port", type=int, required=True)
    ap.add_argument("--console-port", type=int, required=True)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--scripts-dir", required=True)
    ap.add_argument("--transcript", default="")
    ap.add_argument("--stream-path", default="/inventory/events")
    ap.add_argument("--plain-path", default="/inventory/health")
    args = ap.parse_args()

    dv = load_console(args.scripts_dir)
    console = Console(dv, args.host, args.console_port,
                      args.transcript or None)
    results = []
    try:
        print(f"console attached; users {console.users()}"
              f" (this console session is one of them)")
        case_control(console, args.pid, args.host, args.http_port,
                     args.plain_path, False, results)
        case_control(console, args.pid, args.host, args.http_port,
                     args.plain_path, True, results)
        case_departure(console, args.pid, args.host, args.http_port,
                       args.stream_path, False, results)
        case_departure(console, args.pid, args.host, args.http_port,
                       args.stream_path, True, results)
        case_survival(console, args.pid, args.host, args.http_port,
                      args.stream_path, results)
        case_contract(console, args.pid, args.host, args.http_port,
                      args.stream_path, results)
    finally:
        console.close()

    print("\n=== ASSERTIONS ===")
    for name, ok, detail in results:
        print(f"  {'PASS' if ok else 'FAIL'}  {name:<14} {detail}")

    # The controls gate every other verdict in the run: if a NON-streaming
    # connection is not released either, the probe has not demonstrated it
    # can observe a release, and a streaming absence result proves nothing.
    controls = [r for r in results if r[0].startswith("control")]
    if not controls or not all(ok for _, ok, _ in controls):
        print("\nPROBE INVALID: the presence controls did not pass, so no"
              " absence result in this run supports a conclusion.")
        return 2

    failed = [name for name, ok, _ in results if not ok]
    if failed:
        print(f"\nSTREAM-PROBE FAIL ({', '.join(failed)})")
        return 1
    print("\nSTREAM-PROBE PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
