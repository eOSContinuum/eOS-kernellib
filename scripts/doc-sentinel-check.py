#!/usr/bin/env python3
"""Assert that counts stated in prose match the files that own them.

Two rosters, one failure mode. A doc that captures an example's run
transcript hard-codes numbers scripts/run-example.sh owns; a doc that
describes the regression sweep hard-codes the number of steps
scripts/full-sweep.sh owns. Neither number is derived at read time, so
both drift silently and in bulk when the owning file changes.

A doc that captures an example's run transcript hard-codes numbers the
profile table in scripts/run-example.sh owns: the " OK" sentinel count,
the count the harness says it expected, and -- where the transcript
elides its middle -- how many sentinel lines the elision stands for.
Nothing tied those to the profile table, so a profile change left the
prose stale silently. docs/getting-started.md's merry-app count sat at
28 through the profile's move to 30 for exactly that reason.

The sweep-step half has its own history: the script header said 32 and
docs/first-contribution.md said 30 while 33 steps were registered. The
script and scripts/README.md are maintained together by a documented
rule, and that rule reaches neither the counts stated in prose nor its
own two files' agreement -- so each added step made two published
statements more wrong at once, and nothing said so.

This is the static half of the doc-drift guard. The replay half
(scripts/tutorial-smoke.sh) boots DGD and replays the guarded tutorials'
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
FULL_SWEEP = os.path.join(REPO, "scripts", "full-sweep.sh")
SCRIPTS_README = os.path.join(REPO, "scripts", "README.md")

# Markdown scanned for captured transcripts and for stated counts: the
# doc set plus every markdown file at the repository root. Adding a doc
# needs no edit here -- it is covered the moment it carries a
# sentinel-count line or states a roster's size.
#
# The root is taken wholesale rather than as a list because a list is
# the failure this check exists to prevent, one level up. CONTRIBUTING.md
# carried two stale claims about the sweep roster while sitting outside
# an earlier README-only scan, so the file that motivated the step-count
# assertion was the one file it could not see.
SEARCH_DIRS = [os.path.join(REPO, "docs")]
SEARCH_FILES = [os.path.join(REPO, n) for n in sorted(os.listdir(REPO))
                if n.endswith(".md")]

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

# scripts/full-sweep.sh registers each step of the regression sweep as
# `run_step <num> <needs-crypto> <desc> <command...>`; the registration is
# the roster every statement of the sweep's size is measured against.
RUN_STEP_RE = re.compile(r"^run_step\s+(\d+)\s")

# scripts/README.md enumerates the same roster, annotated, as a numbered
# list under this heading.
SWEEP_HEADING = "## Full regression sweep"
ENUM_RE = re.compile(r"^(\d+)\.\s")

# A prose statement of the roster's SIZE: "the 34 steps", "each of the 34
# steps", "runs 34 steps". Any file may carry one, which is the point --
# the two-file maintenance rule reaches the script and the README and
# stops there.
#
# The leading word is required, and it is what separates a claim about
# this roster from a number that merely sits near the word steps. "While
# 33 steps were registered" recounts history and will never go stale;
# "an ordinary `3 steps` in a tutorial" is an illustration. Both appear
# in scripts/README.md's own description of this check, and a bare
# `(\d+) steps` pattern flagged both. The cost of the narrower form is a
# claim phrased outside it going unchecked, so the recognised phrasings
# are documented beside the step in scripts/README.md rather than left
# for an author to guess at.
PROSE_COUNT_RE = re.compile(
    r"\b(?:the|all|its|only|each of the|runs|registers|enumerates|lists|has)"
    r"\s+(\d+)[ -]steps?\b")

# ...but only where the word `sweep` is within a line of it. "3 steps" is
# ordinary English in a tutorial, and an unanchored count would assert
# this roster's size over sentences that have nothing to do with it.
PROSE_ANCHOR_RE = re.compile(r"sweep", re.IGNORECASE)

# Non-markdown files that state the count. The markdown side needs no
# list: markdown_files() already walks the doc set.
PROSE_EXTRA = ["scripts/README.md", "scripts/full-sweep.sh"]


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


def sweep_steps():
    """Return the step numbers scripts/full-sweep.sh registers, sorted."""
    nums = []
    with open(FULL_SWEEP, encoding="utf-8") as fh:
        for line in fh:
            m = RUN_STEP_RE.match(line)
            if m:
                nums.append(int(m.group(1)))
    if not nums:
        sys.exit(
            "doc-sentinel-check: parsed zero run_step lines from scripts/"
            "full-sweep.sh -- the roster changed shape, and every step-count "
            "assertion below would compare against nothing. Fix RUN_STEP_RE.")
    return sorted(nums)


def readme_enumeration():
    """Return the step numbers scripts/README.md's sweep section lists."""
    nums = []
    inside = False
    with open(SCRIPTS_README, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("## "):
                inside = line.strip() == SWEEP_HEADING
                continue
            if inside:
                m = ENUM_RE.match(line)
                if m:
                    nums.append(int(m.group(1)))
    return sorted(nums)


def prose_counts():
    """Return [(relpath, line_no, stated)] for every prose statement of the
    sweep's size, across the doc set and the scripts that describe it.

    The anchor is a window rather than the line: a sentence wraps, and the
    count and the word `sweep` routinely land on different lines of the
    same paragraph.
    """
    paths = [os.path.join(REPO, rel) for rel in PROSE_EXTRA]
    paths += markdown_files()
    out = []
    for path in sorted(set(paths)):
        if not os.path.isfile(path):
            continue
        relpath = os.path.relpath(path, REPO)
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
        for i, line in enumerate(lines):
            window = "\n".join(lines[max(0, i - 1):i + 2])
            if not PROSE_ANCHOR_RE.search(window):
                continue
            for m in PROSE_COUNT_RE.finditer(line):
                out.append((relpath, i + 1, int(m.group(1))))
    return out


def check_sweep_roster(failures):
    """Assert every statement of the regression sweep's size against the
    one file that registers it. Returns the number of assertions made."""
    registered = sweep_steps()
    count = len(registered)
    claims = 1

    if registered != list(range(1, count + 1)):
        failures.append(
            "scripts/full-sweep.sh: run_step numbers %s are not a contiguous "
            "1..%d -- a gap or a duplicate means a step cannot be selected by "
            "number, or two steps answer to one" % (registered, count))

    claims += 1
    enumerated = readme_enumeration()
    if enumerated != registered:
        failures.append(
            "scripts/README.md: the '%s' section enumerates %s, but "
            "scripts/full-sweep.sh registers %s -- the enumeration and the "
            "script are maintained together, same number"
            % (SWEEP_HEADING.lstrip('# '), enumerated, registered))

    stated = prose_counts()
    if not stated:
        failures.append(
            "no prose anywhere states the sweep's step count. Either every "
            "such sentence was removed, or PROSE_COUNT_RE / PROSE_ANCHOR_RE "
            "stopped matching the way they are written -- both mean this "
            "half of the check is asserting nothing")
    for relpath, line_no, value in stated:
        claims += 1
        if value != count:
            failures.append(
                "%s:%d: prose states %d sweep steps; scripts/full-sweep.sh "
                "registers %d" % (relpath, line_no, value, count))

    return claims


def main():
    profiles = load_profiles()
    failures = []
    claims = 0
    files_with_claims = []

    step_claims = check_sweep_roster(failures)

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
          "counts in %s match run-example.sh; %d assertion(s) over the "
          "regression sweep's step count match full-sweep.sh"
          % (claims, ", ".join(files_with_claims), step_claims))


if __name__ == "__main__":
    main()
