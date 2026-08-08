#!/usr/bin/env python3
"""Assert that documented sentinel counts match scripts/run-example.sh.

A doc that captures an example's run transcript hard-codes numbers the
profile table in scripts/run-example.sh owns: the " OK" sentinel count,
the count the harness says it expected, and -- where the transcript
elides its middle -- how many sentinel lines the elision stands for.
Nothing tied those to the profile table, so a profile change left the
prose stale silently. docs/getting-started.md's merry-app count sat at
28 through the profile's move to 30 for exactly that reason.

This is the static half of the doc-drift guard. The replay half
(scripts/tutorial-smoke.sh) boots DGD and replays the three tutorials'
command/expected-output pairs; it does not read getting-started.md, and
it cannot run without a DGD binary. This check needs neither, so it
stays a cheap pre-boot assertion that a contributor can run alone.

Every transcript names its own example, so no doc-to-example mapping is
hard-coded here: the deploy line inside the fence supplies it. A fence
carrying a sentinel-count line whose example cannot be identified is a
hard failure, never a guess -- the same stance scripts/tutorial-replay.py
takes on an unrecognized fence.

Usage:
  doc-sentinel-check.py            # exit 1 if any documented count drifted
"""
import re
import sys
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_EXAMPLE = os.path.join(REPO, "scripts", "run-example.sh")

# Markdown scanned for captured transcripts: the doc set plus the root
# README. Adding a doc needs no edit here -- it is covered the moment it
# carries a sentinel-count line.
SEARCH_DIRS = [os.path.join(REPO, "docs")]
SEARCH_FILES = [os.path.join(REPO, "README.md")]

# scripts/run-example.sh example_profile(): `example-name) echo "Domain
# boots mode count" ;;`. The count is the fourth field and is the value
# the harness compares its live sentinel tally against.
PROFILE_RE = re.compile(r'^\s*([a-z0-9-]+)\)\s*echo\s*"(\S+)\s+(\d+)\s+(\S+)\s+(\d+)"')

# The harness line every captured transcript reproduces verbatim, from
# run-example.sh: `== N " OK" sentinels (expected M) ==`.
COUNT_RE = re.compile(r'^==\s*(\d+)\s+" OK" sentinels \(expected (\d+)\)\s*==')

# The elision a transcript uses in place of its middle sentinels.
ELISION_RE = re.compile(r'^\[\.\.\.\s*(\d+)\s+more sentinels\s*\.\.\.\]')

# The transcript's own statement of which example it ran.
DEPLOY_RE = re.compile(r'^==\s*deploy\s+([a-z0-9-]+)\s+as\b')

# A literally-shown sentinel line, matched the way run-example.sh tallies
# them: a line containing " OK".
SENTINEL_TOKEN = " OK"

FENCE_RE = re.compile(r'^```')


def load_profiles():
    """Return {example: expected_ok_count} from run-example.sh."""
    profiles = {}
    with open(RUN_EXAMPLE, encoding="utf-8") as fh:
        for line in fh:
            m = PROFILE_RE.match(line)
            if m:
                profiles[m.group(1)] = int(m.group(5))
    if not profiles:
        sys.exit(
            "doc-sentinel-check: parsed zero profiles from scripts/"
            "run-example.sh -- example_profile() changed shape and this "
            "check would silently pass. Fix PROFILE_RE."
        )
    return profiles


def markdown_files():
    paths = [p for p in SEARCH_FILES if os.path.isfile(p)]
    for d in SEARCH_DIRS:
        for root, _dirs, names in os.walk(d):
            for n in sorted(names):
                if n.endswith(".md"):
                    paths.append(os.path.join(root, n))
    return sorted(paths)


def fenced_blocks(path):
    """Yield (start_line, [lines]) for each fenced block in the file."""
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    inside = False
    start = 0
    buf = []
    for i, line in enumerate(lines, 1):
        if FENCE_RE.match(line):
            if inside:
                yield start, buf
                buf = []
            inside = not inside
            start = i + 1
            continue
        if inside:
            buf.append(line)


def check_block(relpath, start, block, profiles, failures):
    """Assert one fenced block's sentinel arithmetic.

    Returns the number of ASSERTIONS made, not the number of lines
    carrying them: a count line states two numbers (the tally and what
    the harness expected) and both are checked independently.
    """
    counts = [(i, m) for i, line in enumerate(block)
              for m in [COUNT_RE.match(line)] if m]
    if not counts:
        return 0

    example = None
    for line in block:
        m = DEPLOY_RE.match(line)
        if m:
            example = m.group(1)
            break
    if example is None:
        failures.append(
            "%s:%d: transcript states a sentinel count but no `== deploy "
            "<example> as ... ==` line identifies which example ran; this "
            "check will not guess" % (relpath, start))
        return 0
    if example not in profiles:
        failures.append(
            "%s:%d: transcript deploys '%s', which has no profile in "
            "scripts/run-example.sh example_profile()"
            % (relpath, start, example))
        return 0

    expected = profiles[example]
    claims = 0

    for offset, m in counts:
        line_no = start + offset
        reported, harness_expected = int(m.group(1)), int(m.group(2))
        claims += 2
        if reported != expected:
            failures.append(
                "%s:%d: documented tally %d != run-example.sh profile for "
                "%s (%d)" % (relpath, line_no, reported, example, expected))
        if harness_expected != expected:
            failures.append(
                "%s:%d: documented '(expected %d)' != run-example.sh profile "
                "for %s (%d)"
                % (relpath, line_no, harness_expected, example, expected))

    # The elision stands for the sentinels the transcript does not show,
    # so shown + elided must equal the profile count. This is what makes a
    # partial transcript checkable at all.
    shown = sum(1 for line in block
                if SENTINEL_TOKEN in line and not COUNT_RE.match(line))
    for offset, line in enumerate(block):
        m = ELISION_RE.match(line)
        if not m:
            continue
        claims += 1
        elided = int(m.group(1))
        if shown + elided != expected:
            failures.append(
                "%s:%d: elision says %d more sentinels and %d are shown, "
                "totalling %d != run-example.sh profile for %s (%d)"
                % (relpath, start + offset, elided, shown, shown + elided,
                   example, expected))

    return claims


def main():
    profiles = load_profiles()
    failures = []
    claims = 0
    files_with_claims = []

    for path in markdown_files():
        relpath = os.path.relpath(path, REPO)
        before = claims
        for start, block in fenced_blocks(path):
            claims += check_block(relpath, start, block, profiles, failures)
        if claims > before:
            files_with_claims.append(relpath)

    # Report specific failures BEFORE the empty-parse guard. A block that
    # fails to identify its example, or names an example with no profile,
    # records a failure and contributes zero assertions -- so a guard that
    # ran first would answer "every transcript was removed", which is both
    # wrong and the opposite of actionable. Diagnosing the actual break
    # beats reporting the symptom it produces downstream.
    if failures:
        for f in failures:
            print("doc-sentinel-check: " + f, file=sys.stderr)
        print("doc-sentinel-check: %d drifted or unresolvable claim(s)"
              % len(failures), file=sys.stderr)
        sys.exit(1)

    # Empty-parse guard, the same stance tutorial-smoke.sh takes: a doc
    # restructure that renames the harness line this parser keys on must
    # not silently vacuum the check into a pass over nothing. Reached only
    # when nothing failed, so "asserting nothing" is the accurate reading.
    if claims == 0:
        sys.exit(
            "doc-sentinel-check: found zero documented sentinel counts "
            "across the doc set. Either every captured transcript was "
            "removed, or COUNT_RE no longer matches run-example.sh's "
            "output line. Both mean this check is asserting nothing.")

    print("doc-sentinel-check: %d assertion(s) over documented sentinel "
          "counts in %s match run-example.sh"
          % (claims, ", ".join(files_with_claims)))


if __name__ == "__main__":
    main()
