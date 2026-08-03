#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Replay docs/first-hour.md, docs/first-application.md, and
docs/first-http-endpoint.md against a live boot, driven by
scripts/tutorial-smoke.sh.

Parses the three tutorials' fenced blocks AT RUN TIME -- no generated
mirror of the transcripts is kept, because a mirror would drift out of
sync with the docs, which is exactly the failure mode this guard exists
to catch. All three tutorials use exactly three fence languages (verified
by inspection of every ``` marker in all three files); ALLOWED_KINDS
below is that whitelist. Every fence in every document is classified --
a fence whose language is not in ALLOWED_KINDS is a named FAIL (file,
line, language), before any boot, not a quietly-ignored block: a renamed
fence marker must not be able to drop a whole class of assertions while
some other fence kind still parsed to a nonzero count. For each
recognized fence, in document order:

first-hour.md is a separate console session from the other two: it
covers its own fresh cold boot and does not chain into
first-application.md's console history (first-application.md's own
transcript starts back at `$0`, matching a fresh connection). DOCS below
marks first-application as needing a fresh boot immediately before its
actions run, discarding first-hour's snapshot the way a reader
re-booting a clean environment for the next tutorial would; first-hour
and first-http-endpoint need no such marker (first-hour is first, so the
harness's own initial boot already covers it, and first-http-endpoint
continues first-application's session with no boot in between, matching
its own transcript's continuing `$N` count). Within first-hour.md, three
kinds of block resist replay outright and are named SKIPs rather than
assertions: the cold-boot invocation line (the harness performs that
boot itself, once, before any doc's actions run), the interactive
`telnet`/`nc` connect lines (the harness drives a raw console socket via
drive-verbs.py's Session/login instead), and the login-banner transcript
blocks (`login: admin` / `Pick a new password:` / ... -- the same
Session/login call handles both the first-claim and returning-login
shapes already, so the banner text is illustrative, not a block this
parser can turn into a command/expected-output pair).

  ```text  fences  -- console transcripts. Each "# <cmd>" line starts a
                      new entry; the following non-"# " lines (up to the
                      next "# " line or the fence end) are its expected
                      output.
  ```sh    fences  -- shell lines. A line naming "example.dgd" is the
                      tutorial's dgd-restart step (parsed for its
                      snapshot-file arguments); a line immediately
                      followed by a "# " comment line is a
                      command/expected-output pair (curl probes); any
                      other line is a plain setup command (mkdir, cp).
  ```c     fences  -- LPC source. The nearest preceding non-blank line
                      (the anchor sentence) is parsed for a backtick path
                      ending in ".c" (the target file). With no such path
                      the fence is illustrative quoting of already-shipped
                      code (e.g. first-http-endpoint.md's emit()/
                      doneRequest() excerpt) -- reported as a named SKIP,
                      not applied. With a path, the anchor sentence must
                      affirmatively match one of four phrasings the two
                      docs actually use, checked in this order:
                        "replace the whole `NAME()` function"  -> replace
                                                                    the
                                                                    whole
                                                                    function
                        "above `NAME`"                          -> insert
                                                                    before
                                                                    NAME's
                                                                    decl
                        "after the `NAME` branch"               -> insert
                                                                    after
                                                                    the
                                                                    if
                                                                    (method
                                                                    ==
                                                                    "NAME")
                                                                    block
                        `path.c`, <description>:                -> write
                                                                    the
                                                                    whole
                                                                    file
                                                                    (the
                                                                    initd.c
                                                                    /
                                                                    kv_daemon.c
                                                                    blocks
                                                                    --
                                                                    the
                                                                    anchor
                                                                    line
                                                                    opens
                                                                    with
                                                                    the
                                                                    backtick
                                                                    path
                                                                    itself)
                      A pathed c-fence matching none of the four is a
                      named FAIL quoting the anchor sentence -- never a
                      guessed write_file, which would silently corrupt
                      the target on a benign rewording.

Normalization: DGD console history slots ("$N = ...") are run-varying by
design (the tutorials say so explicitly -- any extra console command
shifts every later slot, and a fresh telnet session after a reboot starts
counting at $0 again). The comparator strips the leading "$<digits> ="
and compares only the value to its right; everything else is compared
verbatim. Literal teaching ports (8023, 8080) embedded in a parsed shell
command or its expected output are rewritten to the boot's SMOKE_* ports
before use. Any other mismatch -- there being no other run-varying
expectation class in these two documents -- is a hard FAIL naming the
block, not a silent skip.

What is loud, precisely: an unrecognized fence language is a FAIL naming
the file, line, and language; a recognized-but-unassertable block (the
illustrative c-fence case above) is a named SKIP with its reason; a
comparator mismatch is a FAIL naming the block. A parse yielding zero
command/expected-output blocks for a document is itself a FAIL (the
empty-parse guard) -- belt-and-braces alongside the fence-language
whitelist, which is what actually catches a single fence kind silently
losing all its blocks while another kind in the same document keeps the
total nonzero.
"""

import importlib.util
import os
import re
import shlex
import socket
import subprocess
import sys
import time

REPO_ROOT = os.environ.get("REPO_ROOT") or os.getcwd()
DGD_BIN = os.environ["DGD_BIN"]
CONFIG = os.environ.get("TUTORIAL_CONFIG", "state/tutorial-smoke.dgd")
HOST = "127.0.0.1"
TELNET_PORT = int(os.environ.get("SMOKE_TELNET_PORT", "48023"))
BINARY_PORT = int(os.environ.get("SMOKE_BINARY_PORT", "48080"))
BOOT_LOG = os.path.join(REPO_ROOT, "state", "tutorial-smoke-boot.log")

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "drive_verbs", os.path.join(SCRIPT_DIR, "drive-verbs.py"))
drive_verbs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(drive_verbs)

DOCS = [
    ("first-hour", "docs/first-hour.md", False),
    ("first-application", "docs/first-application.md", True),
    ("first-http-endpoint", "docs/first-http-endpoint.md", False),
]

# The complete set of fence languages the three tutorials use, derived by
# inspecting every ``` marker in all three files (`grep -n '^```' docs/
# first-hour.md docs/first-application.md docs/first-http-endpoint.md`):
# text (console transcripts), sh (shell/curl/boot lines), c (LPC source).
# A fence in any document tagged with anything else is unrecognized and
# a hard FAIL -- see classify_fence_kind below.
ALLOWED_KINDS = {"text", "sh", "c"}

FENCE_RE = re.compile(r"^```(\w+)\s*$")


class TutorialParseError(Exception):
    """A static, pre-boot parse failure: an unrecognized fence language,
    or a pathed c-fence whose anchor sentence matches none of the
    recognized phrasings. Always fatal -- never a guess."""


# ---- parsing ---------------------------------------------------------

def parse_fences(path):
    """Return an ordered list of {kind, context, content, line} for every
    fence in the file, where context is the nearest preceding non-blank
    line and line is the fence marker's 1-based line number."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    items = []
    i = 0
    n = len(lines)
    prev_nonblank = ""
    while i < n:
        line = lines[i]
        m = FENCE_RE.match(line)
        if m:
            kind = m.group(1)
            fence_line = i + 1
            j = i + 1
            content_lines = []
            while j < n and lines[j].strip() != "```":
                content_lines.append(lines[j])
                j += 1
            items.append({"kind": kind, "context": prev_nonblank,
                           "content": "\n".join(content_lines),
                           "line": fence_line})
            i = j + 1
            prev_nonblank = ""
            continue
        if line.strip():
            prev_nonblank = line.strip()
        i += 1
    return items


def rewrite_ports(text):
    text = text.replace(":8080", f":{BINARY_PORT}")
    text = text.replace(":8023", f":{TELNET_PORT}")
    return text


def classify_console(content):
    """```text fence -> list of {'op': 'console', 'cmd', 'expected': [lines]}."""
    actions = []
    cur_cmd = None
    cur_out = []

    def flush():
        if cur_cmd is not None:
            actions.append({"op": "console", "cmd": cur_cmd,
                             "expected": list(cur_out)})

    for line in content.split("\n"):
        if line.startswith("# "):
            flush()
            cur_cmd = line[2:]
            cur_out = []
        elif line == "#":
            flush()
            cur_cmd = ""
            cur_out = []
        else:
            cur_out.append(line)
    flush()
    return actions


INLINE_COMMENT_RE = re.compile(r"\s+#.*$")


def classify_shell(content):
    """```sh fence -> list of {'op': 'shell'|'restart'|'http_assert'|
    'skip', ...}. A trailing "    # ..." inline comment (first-hour.md's
    boot and telnet lines carry one, e.g. "... example.dgd    # or
    state/local.dgd, ...") is stripped before classification; it never
    appears on the curl/expected-output pairs below, which put their
    "# <expected>" comment on its own following line instead, so this
    strip cannot collide with that pairing."""
    lines = [l for l in content.split("\n")]
    actions = []
    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        stripped = INLINE_COMMENT_RE.sub("", line.strip()).strip()
        if not stripped:
            i += 1
            continue
        if stripped.startswith("telnet ") or stripped.startswith("nc "):
            actions.append({"op": "skip", "reason":
                             "interactive telnet/nc connect line; the "
                             "harness drives a raw console socket "
                             "(drive-verbs.py Session/login) instead"})
            i += 1
            continue
        if "example.dgd" in stripped:
            rest = stripped.split("example.dgd", 1)[1].strip()
            if not rest:
                actions.append({"op": "skip", "reason":
                                 "initial cold boot invocation; the "
                                 "harness performs this boot itself, "
                                 "once, before any tutorial's actions "
                                 "run"})
                i += 1
                continue
            snapshot_args = rest.split()
            actions.append({"op": "restart", "snapshot_args": snapshot_args})
            i += 1
            continue
        if i + 1 < n and lines[i + 1].startswith("#"):
            cmd = rewrite_ports(stripped)
            expected = rewrite_ports(lines[i + 1].lstrip("#").strip())
            actions.append({"op": "http_assert", "cmd": cmd,
                             "expected": expected})
            i += 2
            continue
        actions.append({"op": "shell", "cmd": stripped})
        i += 1
    return actions


PATH_RE = re.compile(r"`([\w./-]+\.c)`")
REPLACE_FN_RE = re.compile(r"replace the whole `(\w+)\(\)` function")
INSERT_BEFORE_RE = re.compile(r"above `(\w+)`")
INSERT_AFTER_BRANCH_RE = re.compile(r"after the `(\w+)` branch")
# The full-file-write anchors (initd.c, kv_daemon.c) all open the line
# with the backtick path itself, then a comma-led description ending in
# a colon -- e.g. "`src/usr/KV/initd.c`, the domain's bootstrap:". This
# is checked last and only fires on an affirmative structural match; it
# is not a fallback for "none of the above".
WRITE_FILE_RE = re.compile(r"^`[\w./-]+\.c`\s*,.*:\s*$")


def classify_code(relpath, context, content):
    """```c fence -> one code-mutation action, keyed off the anchor
    sentence (the preceding non-blank line).

    Returns None when the fence is illustrative quoting of already-
    shipped code with no `<path>.c` target named in its anchor sentence
    (e.g. first-http-endpoint.md's `emit()` excerpt, already present via
    the examples/http-app copy) -- the caller reports this as a SKIP.

    Raises TutorialParseError when the fence names a `<path>.c` target
    but its anchor sentence matches none of the recognized phrasings:
    guessing write_file here would silently overwrite (corrupt) the
    target file on a benign rewording instead of failing loudly."""
    paths = PATH_RE.findall(context)
    if not paths:
        return None
    path = paths[-1]

    m = REPLACE_FN_RE.search(context)
    if m:
        return {"op": "replace_function", "path": path, "name": m.group(1),
                "content": content}
    m = INSERT_BEFORE_RE.search(context)
    if m:
        return {"op": "insert_before_decl", "path": path, "name": m.group(1),
                "content": content}
    m = INSERT_AFTER_BRANCH_RE.search(context)
    if m:
        return {"op": "insert_after_branch", "path": path,
                "name": m.group(1), "content": content}
    if WRITE_FILE_RE.search(context):
        return {"op": "write_file", "path": path, "content": content}
    raise TutorialParseError(
        f"{relpath}: c-fence targets {path!r} but its anchor sentence "
        f"matches none of the recognized phrasings (replace-whole-"
        f"function / above-NAME / after-NAME-branch / "
        f"`path.c`,-description:): {context!r}")


def build_actions(name, relpath):
    full_path = os.path.join(REPO_ROOT, relpath)
    items = parse_fences(full_path)
    actions = []
    for item in items:
        if item["kind"] not in ALLOWED_KINDS:
            raise TutorialParseError(
                f"{relpath}:{item['line']}: unrecognized fence language "
                f"'{item['kind']}' -- this parser only recognizes "
                f"{sorted(ALLOWED_KINDS)} in these three tutorials "
                f"(ALLOWED_KINDS in tutorial-replay.py)")
        if item["kind"] == "text":
            if item["content"].strip().startswith("login:"):
                print(f"SKIP [{name}] interactive login-banner transcript "
                      f"at line {item['line']} (the harness's own "
                      f"drive-verbs.py Session/login already handles both "
                      f"the first-claim and returning-login shapes; the "
                      f"banner text is illustrative, not replayed "
                      f"verbatim)")
                continue
            console_actions = classify_console(item["content"])
            if not console_actions:
                print(f"SKIP [{name}] illustrative text fence at line "
                      f"{item['line']} with no `# <cmd>` line to drive")
                continue
            actions.extend(console_actions)
        elif item["kind"] == "sh":
            for act in classify_shell(item["content"]):
                if act["op"] == "skip":
                    print(f"SKIP [{name}] {act['reason']}")
                else:
                    actions.append(act)
        elif item["kind"] == "c":
            code_action = classify_code(relpath, item["context"],
                                         item["content"])
            if code_action is None:
                print(f"SKIP [{name}] illustrative c-fence, no `<path>.c` "
                      f"target in anchor: {item['context']!r}")
            else:
                actions.append(code_action)
    return actions


# ---- code-mutation execution ------------------------------------------

def brace_span(content, open_idx):
    depth = 0
    i = open_idx
    while i < len(content):
        if content[i] == "{":
            depth += 1
        elif content[i] == "}":
            depth -= 1
            if depth == 0:
                return open_idx, i
        i += 1
    raise ValueError("unbalanced braces from index %d" % open_idx)


def execute_code_action(action):
    full_path = os.path.join(REPO_ROOT, action["path"])
    if action["op"] == "write_file":
        os.makedirs(os.path.dirname(full_path), exist_ok=True)
        with open(full_path, "w", encoding="utf-8") as f:
            f.write(action["content"].rstrip("\n") + "\n")
        return

    with open(full_path, encoding="utf-8") as f:
        cur = f.read()

    if action["op"] == "replace_function":
        name = action["name"]
        m = re.search(re.escape(name) + r"\s*\([^)]*\)", cur)
        if not m:
            raise ValueError(f"function {name} not found in {full_path}")
        line_start = cur.rfind("\n", 0, m.start()) + 1
        brace_idx = cur.index("{", m.end())
        _, close_idx = brace_span(cur, brace_idx)
        new_cur = (cur[:line_start] + action["content"].rstrip("\n") + "\n"
                   + cur[close_idx + 1:])
    elif action["op"] == "insert_before_decl":
        name = action["name"]
        m = re.search(r"\n(\S.*\b" + re.escape(name) + r"\s*\()", cur)
        if not m:
            raise ValueError(f"declaration for {name} not found in {full_path}")
        insert_at = m.start() + 1
        new_cur = (cur[:insert_at] + action["content"].rstrip("\n") + "\n\n"
                   + cur[insert_at:])
    elif action["op"] == "insert_after_branch":
        name = action["name"]
        m = re.search(r'if\s*\(\s*method\s*==\s*"' + re.escape(name)
                       + r'"\s*\)\s*\{', cur)
        if not m:
            raise ValueError(f"branch for method {name} not found in {full_path}")
        open_idx = cur.index("{", m.start())
        _, close_idx = brace_span(cur, open_idx)
        new_cur = (cur[:close_idx + 1] + "\n" + action["content"].rstrip("\n")
                   + cur[close_idx + 1:])
    else:
        raise ValueError(f"unknown code op: {action['op']}")

    with open(full_path, "w", encoding="utf-8") as f:
        f.write(new_cur)


# ---- output comparison -------------------------------------------------

SLOT_RE = re.compile(r"^\$(\d+)\s*=\s*(.*)$", re.DOTALL)

# The second run-varying expectation class, first-hour.md only: a clone
# object's numeric index ("</usr/Pet/obj/pet#212>") is platform-global
# and run-dependent, exactly as the doc itself says ("Your clone number
# will differ from `#212`. Clone indices are platform-global."). Neither
# of the other two tutorials embeds a clone index in expected output
# (verified by inspection: `#\d+` appears nowhere in their fenced
# blocks), so normalizing it here cannot mask a real mismatch there.
CLONE_INDEX_RE = re.compile(r"#\d+")


def outputs_match(expected_lines, actual_text):
    # Telnet lines are CRLF; a multi-line expected block (e.g. the
    # `observers` verb's tree output) is written in the doc with plain
    # LF, so normalize line endings before any other comparison.
    expected = "\n".join(expected_lines).replace("\r\n", "\n").strip("\r\n")
    actual = actual_text.replace("\r\n", "\n").strip("\r\n")
    if expected == "":
        return actual.strip() == ""
    expected = CLONE_INDEX_RE.sub("#N", expected)
    actual = CLONE_INDEX_RE.sub("#N", actual)
    m_exp = SLOT_RE.match(expected)
    m_act = SLOT_RE.match(actual)
    if m_exp and m_act:
        return m_exp.group(2).strip() == m_act.group(2).strip()
    return expected.strip() == actual.strip()


# ---- dgd process + console session lifecycle ---------------------------

class Runner:
    def __init__(self):
        self.proc = None
        self.session = None

    def start_dgd(self, snapshot_args):
        os.makedirs(os.path.dirname(BOOT_LOG), exist_ok=True)
        with open(BOOT_LOG, "ab") as logf:
            logf.write(f"\n== boot: {DGD_BIN} {CONFIG} "
                        f"{' '.join(snapshot_args)} ==\n".encode())
        logf = open(BOOT_LOG, "ab")
        cmd = [DGD_BIN, CONFIG] + snapshot_args
        self.proc = subprocess.Popen(cmd, cwd=REPO_ROOT, stdout=logf,
                                      stderr=subprocess.STDOUT)
        self._wait_port()

    def _wait_port(self, cap=30):
        for _ in range(cap):
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"dgd exited during boot (rc={self.proc.returncode}); "
                    f"see {BOOT_LOG}")
            try:
                s = socket.create_connection((HOST, TELNET_PORT), 1)
                s.close()
                return
            except OSError:
                time.sleep(1)
        raise RuntimeError(
            f"telnet port {TELNET_PORT} did not come up within {cap}s; "
            f"see {BOOT_LOG}")

    def ensure_session(self):
        if self.session is None:
            self.session = drive_verbs.Session(HOST, TELNET_PORT, None)
            drive_verbs.login(self.session, "admin", "drive-verbs")
        return self.session

    def do_reboot(self):
        sess = self.ensure_session()
        sess.send_line("reboot")
        # The process self-exits; drain whatever the closing connection
        # sends (nothing is asserted here -- the doc shows no output for
        # this command) and let the socket close on its own.
        sess.read_until([r"\bNEVERMATCH\b"], timeout=5)
        sess.close()
        self.session = None
        if self.proc is not None:
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                raise RuntimeError(
                    "dgd did not self-exit after 'reboot' within 15s")
        self.proc = None

    def do_restart(self, snapshot_args):
        self.start_dgd(snapshot_args)

    def teardown(self):
        if self.session is not None:
            try:
                self.session.close()
            except Exception:
                pass
            self.session = None
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
            self.proc = None


def drive_console(runner, cmd):
    sess = runner.ensure_session()
    sess.send_line(cmd)
    response = sess.read_until([drive_verbs.PROMPT])
    body = re.sub(r"\n?[>#] $", "", response).strip("\r\n")
    return body


def run_http_assert(cmd):
    try:
        proc = subprocess.run(shlex.split(cmd), cwd=REPO_ROOT,
                               capture_output=True, text=True, timeout=15)
    except subprocess.TimeoutExpired:
        return "<timeout>"
    return proc.stdout.strip("\r\n")


def run_shell(cmd):
    proc = subprocess.run(shlex.split(cmd), cwd=REPO_ROOT,
                           capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"setup command failed: {cmd!r} (rc={proc.returncode})\n"
            f"stdout: {proc.stdout}\nstderr: {proc.stderr}")


# ---- main ---------------------------------------------------------------

def main():
    all_actions = {}
    for name, relpath, _fresh_boot in DOCS:
        try:
            actions = build_actions(name, relpath)
        except TutorialParseError as e:
            print(f"tutorial-replay.py: {e}", file=sys.stderr)
            print("TUTORIAL-SMOKE FAIL (parse error)", file=sys.stderr)
            sys.exit(2)
        all_actions[name] = actions
        assertable = sum(1 for a in actions
                          if a["op"] in ("console", "http_assert"))
        if assertable == 0:
            print(f"tutorial-replay.py: empty parse for {name} ({relpath}) "
                  f"-- 0 command/expected-output blocks found; a fence "
                  f"marker or anchor sentence this parser keys on may have "
                  f"changed", file=sys.stderr)
            print("TUTORIAL-SMOKE FAIL (empty-parse guard)", file=sys.stderr)
            sys.exit(2)

    runner = Runner()
    failures = []
    counts = {name: {"console": 0, "http": 0} for name, _, _ in DOCS}
    try:
        runner.start_dgd([])
        for name, _relpath, fresh_boot in DOCS:
            if fresh_boot:
                # first-application.md's own transcript starts back at
                # $0 (a fresh connection), and its audience line says
                # "boot the platform ... exactly as in first-hour.md
                # sections 1 and 2" -- a clean environment, not a
                # continuation of first-hour's Pet objects and console
                # history. Tear down and cold-boot again (no snapshot
                # arg); admin.pwd and access.data are file-backed and
                # survive this exactly as the tutorials themselves say.
                runner.teardown()
                runner.start_dgd([])
            for action in all_actions[name]:
                op = action["op"]
                if op in ("write_file", "replace_function",
                          "insert_before_decl", "insert_after_branch"):
                    execute_code_action(action)
                elif op == "shell":
                    run_shell(action["cmd"])
                elif op == "restart":
                    runner.do_restart(action["snapshot_args"])
                elif op == "http_assert":
                    counts[name]["http"] += 1
                    actual = run_http_assert(action["cmd"])
                    tag = f"[{name}] {action['cmd']}"
                    if outputs_match([action["expected"]], actual):
                        print(f"PASS {tag}")
                    else:
                        print(f"FAIL {tag}")
                        print(f"    expected: {action['expected']!r}")
                        print(f"    actual:   {actual!r}")
                        failures.append(tag)
                elif op == "console":
                    counts[name]["console"] += 1
                    tag = f"[{name}] {action['cmd']}"
                    if action["cmd"] == "reboot":
                        runner.do_reboot()
                        print(f"PASS {tag} (process self-exit)")
                        continue
                    actual = drive_console(runner, action["cmd"])
                    if outputs_match(action["expected"], actual):
                        print(f"PASS {tag}")
                    else:
                        print(f"FAIL {tag}")
                        print(f"    expected: {action['expected']!r}")
                        print(f"    actual:   {actual!r}")
                        failures.append(tag)
                else:
                    raise ValueError(f"unknown action op: {op}")
    finally:
        runner.teardown()

    summary = ", ".join(
        f"{name}: {counts[name]['console']} console + "
        f"{counts[name]['http']} http" for name, _, _ in DOCS)
    if failures:
        print(f"TUTORIAL-SMOKE FAIL (first: {failures[0]})", file=sys.stderr)
        return 1
    print(f"TUTORIAL-SMOKE PASS ({summary})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
