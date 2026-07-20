#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Drive admin-console verbs over telnet and check responses.

Connects to a running DGD instance's telnet port, logs in (auto-detecting
the first-connect set-a-password flow vs the returning password flow from
the prompt text), drives each entry in a verbset file, checks the
response against the entry's expectations, and prints a PASS/FAIL
summary. The full session transcript is written to a log file for
forensics.

The session user defaults to admin; a verbset that must run as a
registered non-admin operator declares its user with file-level
directives (see below). The login flow is the same either way -- the
System user object walks every registered user through the identical
password states -- but a non-admin name must already be on the kernel
access list when the session connects (cold boots register no one:
provision first via the admin console's `grant <user> access`, e.g.
scripts/verbsets/operator-provision.verbset).

Usage:
    scripts/drive-verbs.py <verbset-file> [options]

Verbset file format -- optional file-level directives first, then blocks
separated by blank lines, one verb per block; '#' lines are comments:

    user: testop
    password: drive-verbs

    cmd: cascade-depth
    expect: cascade-depth: \\d+
    absent: usage:

  user:     (optional, before the first cmd) session login name;
            --user overrides, default admin
  password: (optional, before the first cmd) session password;
            --password overrides, default drive-verbs
  cmd:      (required) the verb line to send
  expect:   (repeatable) regex that must match the response (re.search)
  absent:   (repeatable) regex that must NOT match the response
  capture:  (repeatable) '<name> <regex-with-one-group>'; on match, the
            group's text is stored under <name> and later cmd/expect/
            absent/capture lines may reference it as %{name} (values are
            regex-escaped when substituted into patterns, inserted raw
            into cmd lines). A capture that does not match FAILs the
            block. Lets a verbset thread runtime values (a minted id, a
            generated secret) through later verbs.

A block with only expect/absent failures reports FAIL; the run exits
non-zero if any entry fails. Entries run in file order, so a verbset can
express mutate -> verify -> undo -> verify-undo sequences as consecutive
entries.

The DGD telnet port speaks real telnet: IAC negotiation sequences are
stripped from received data and not answered (the kernel telnet object
tolerates a silent client).
"""

import argparse
import re
import socket
import sys
import time

IAC = 0xFF

# the kernel admin console prompts "# "; plain users get "> "
PROMPT = r"[>#] $"


def strip_iac(data: bytes) -> bytes:
    """Drop telnet IAC command sequences from a byte stream."""
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        if b != IAC:
            out.append(b)
            i += 1
            continue
        if i + 1 >= len(data):
            break
        cmd = data[i + 1]
        if cmd == IAC:        # escaped 0xFF data byte
            out.append(IAC)
            i += 2
        elif cmd in (0xFB, 0xFC, 0xFD, 0xFE):  # WILL/WONT/DO/DONT <opt>
            i += 3
        else:                 # other two-byte command
            i += 2
    return bytes(out)


class Session:
    """A telnet session with quiet-timeout reads and a transcript."""

    def __init__(self, host, port, transcript_path, quiet=0.4, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=5.0)
        self.sock.settimeout(0.1)
        self.quiet = quiet
        self.timeout = timeout
        self.transcript = None
        if transcript_path:
            self.transcript = open(transcript_path, "w", encoding="utf-8",
                                   errors="replace")

    def close(self):
        try:
            self.sock.close()
        finally:
            if self.transcript:
                self.transcript.close()

    def send_line(self, line: str):
        if self.transcript:
            self.transcript.write(f">>> {line}\n")
        self.sock.sendall(line.encode("ascii") + b"\r\n")

    def read_until(self, patterns, timeout=None):
        """Read until any regex in `patterns` matches the accumulated text,
        until the connection goes quiet for `self.quiet` seconds, or until
        `timeout` (then return what arrived). Returns the text."""
        deadline = time.monotonic() + (timeout or self.timeout)
        buf = b""
        last_data = time.monotonic()
        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
                last_data = time.monotonic()
            except socket.timeout:
                if buf and time.monotonic() - last_data > self.quiet:
                    break
                continue
            text = strip_iac(buf).decode("ascii", errors="replace")
            if any(re.search(p, text) for p in patterns):
                break
        text = strip_iac(buf).decode("ascii", errors="replace")
        if self.transcript:
            self.transcript.write(text)
            if not text.endswith("\n"):
                self.transcript.write("\n")
        return text


def login(sess: Session, user: str, password: str) -> None:
    """Handle both the first-connect and the returning login flows."""
    text = sess.read_until([r"login: $"])
    if "login:" not in text:
        raise RuntimeError(f"no login prompt; got: {text!r}")
    sess.send_line(user)
    text = sess.read_until([r"Pick a new password:", r"Password:", r"[>#] $"])
    if "Pick a new password:" in text:
        sess.send_line(password)
        text = sess.read_until([r"Retype new password:"])
        if "Retype new password:" not in text:
            raise RuntimeError(f"no retype prompt; got: {text!r}")
        sess.send_line(password)
        sess.read_until([PROMPT])
    elif "Password:" in text:
        sess.send_line(password)
        text = sess.read_until([PROMPT, r"login: "])
        if not re.search(PROMPT, text):
            raise RuntimeError(
                "password rejected (default is 'drive-verbs'; if the console"
                " was claimed with another password, delete"
                " src/kernel/data/admin.pwd and rerun -- the next connect"
                " re-claims it)")
    elif not re.search(PROMPT, text):
        raise RuntimeError(f"unexpected login response: {text!r}")


def parse_verbset(path: str):
    """Parse the block-per-entry verbset format.

    Returns (meta, entries): meta holds the file-level 'user'/'password'
    directives (empty when absent), entries the cmd blocks."""
    entries = []
    meta = {}
    block = {"cmd": None, "expect": [], "absent": [], "capture": []}

    def flush():
        nonlocal block
        if block["cmd"]:
            entries.append(block)
        block = {"cmd": None, "expect": [], "absent": [], "capture": []}

    with open(path, encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line.strip():
                flush()
                continue
            if line.lstrip().startswith("#"):
                continue
            key, sep, value = line.partition(":")
            key = key.strip()
            value = value.strip()
            if sep and key in ("user", "password"):
                if entries or block["cmd"]:
                    raise ValueError(f"{path}:{lineno}: '{key}:' is a "
                                     f"file-level directive; it must precede "
                                     f"the first cmd block")
                meta[key] = value
                continue
            if not sep or key not in ("cmd", "expect", "absent", "capture"):
                raise ValueError(f"{path}:{lineno}: expected "
                                 f"'cmd:'/'expect:'/'absent:'/'capture:', "
                                 f"got: {line}")
            if key == "cmd":
                if block["cmd"]:
                    flush()
                block["cmd"] = value
            elif key == "capture":
                name, _, pattern = value.partition(" ")
                if not name.isidentifier() or not pattern.strip():
                    raise ValueError(f"{path}:{lineno}: capture wants "
                                     f"'capture: <name> <regex-with-group>'")
                block["capture"].append((name, pattern.strip()))
            else:
                block[key].append(value)
    flush()
    if not entries:
        raise ValueError(f"{path}: no entries")
    return meta, entries


def substitute(text: str, variables: dict, escape: bool) -> str:
    """Replace %{name} references with captured values; escape=True
    regex-escapes the value (for expect/absent patterns), False inserts
    it raw (for cmd lines). An unknown name is an error."""
    def repl(m):
        name = m.group(1)
        if name not in variables:
            raise ValueError(f"%{{{name}}} referenced before capture")
        value = variables[name]
        return re.escape(value) if escape else value
    return re.sub(r"%\{(\w+)\}", repl, text)


def drive(sess: Session, entries) -> int:
    failures = 0
    variables = {}
    for n, entry in enumerate(entries, 1):
        try:
            cmd = substitute(entry["cmd"], variables, escape=False)
        except ValueError as e:
            # an earlier capture failed; skip the send, fail the block
            failures += 1
            print(f"FAIL [{n:2}] {entry['cmd']}")
            print(f"        {e}")
            continue
        sess.send_line(cmd)
        response = sess.read_until([PROMPT])
        # trim the trailing prompt; the rest is the verbatim response
        # (the kernel console does not echo commands -- telnet echo is
        # client-side -- so no echo stripping: a response can legally
        # equal the command text, e.g. "dispatch-trace on")
        body = re.sub(r"\n?[>#] $", "", response).strip("\r\n")

        problems = []
        for pat in entry["expect"]:
            try:
                if not re.search(substitute(pat, variables, escape=True),
                                 body, re.MULTILINE):
                    problems.append(f"expect failed: /{pat}/")
            except ValueError as e:
                problems.append(f"expect unusable: {e}")
        for pat in entry["absent"]:
            try:
                if re.search(substitute(pat, variables, escape=True),
                             body, re.MULTILINE):
                    problems.append(f"absent matched: /{pat}/")
            except ValueError as e:
                problems.append(f"absent unusable: {e}")
        for name, pat in entry["capture"]:
            try:
                m = re.search(substitute(pat, variables, escape=True),
                              body, re.MULTILINE)
            except ValueError as e:
                problems.append(f"capture unusable: {e}")
                continue
            if not m or m.lastindex is None:
                problems.append(f"capture failed: {name} /{pat}/")
            else:
                variables[name] = m.group(1)

        if problems:
            failures += 1
            print(f"FAIL [{n:2}] {cmd}")
            for p in problems:
                print(f"        {p}")
            for line in body.splitlines():
                print(f"        | {line}")
        else:
            print(f"PASS [{n:2}] {cmd}")
    return failures


def main():
    ap = argparse.ArgumentParser(
        description="Drive admin-console verbs from a verbset file.")
    ap.add_argument("verbset", help="verbset file (see module docstring)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8023)
    ap.add_argument("--user", default=None,
                    help="session login name (overrides the verbset's "
                         "user: directive; default admin)")
    ap.add_argument("--password", default=None,
                    help="session password (overrides the verbset's "
                         "password: directive; default drive-verbs)")
    ap.add_argument("--transcript", default=None,
                    help="optional path to write the full session transcript")
    args = ap.parse_args()

    meta, entries = parse_verbset(args.verbset)
    user = args.user or meta.get("user") or "admin"
    password = args.password or meta.get("password") or "drive-verbs"
    sess = Session(args.host, args.port, args.transcript)
    try:
        login(sess, user, password)
        failures = drive(sess, entries)
    finally:
        sess.close()

    total = len(entries)
    note = f" (transcript: {args.transcript})" if args.transcript else ""
    print(f"== {total - failures}/{total} verbs PASS{note} ==")
    if failures:
        print("DRIVE-VERBS FAIL", file=sys.stderr)
        return 1
    print("DRIVE-VERBS PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
