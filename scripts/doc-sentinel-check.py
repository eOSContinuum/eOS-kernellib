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

# The other numbered list scripts/README.md owns, whose size two
# files state in prose: this README and CONTRIBUTING.md. It grows
# whenever registering an example gains a step, which is exactly
# what makes both statements go stale at once and silently.
CHECKLIST_HEADING = "## Adding a new example"

# ---------------------------------------------------------------------
# Roster claims.
#
# A ROSTER is a set some file registers and prose restates the size of:
# the regression sweep's steps, the replayed tutorials, the bundled
# examples, the runtime primitives. One file owns the membership; any
# number of published sentences state how many members there are; and
# nothing derives the second from the first, so every addition to a
# roster makes each of those sentences wrong at once and silently.
#
# The sweep-step half of this check proved the shape and then proved its
# own narrowness: it recognises a number as a claim only where a leading
# word makes it one AND the word `sweep` is nearby, so it saw exactly one
# roster. Three others were restated across the doc set the whole time --
# `README.md` said thirteen runnable applications against fifteen
# directories, `examples/README.md` said the harness covered eleven of
# fourteen against twelve of fifteen, and `application-authoring.md` said
# seven of nine bundled examples ship a test driver against eleven of
# fifteen. None was reachable by a sweep-anchored pattern, so widening
# the pattern per-defect would have meant widening it once per roster
# forever. The roster is the unit instead.
#
# WHAT A CLAIM LOOKS LIKE. Three phrasings are recognised, each requiring
# the number to sit next to the roster's noun (one intervening adjective
# is allowed, so "the nine bundled examples" is a claim and "the HTTP/1
# worked example" is not):
#
#   lead-in     "the 34 steps", "each of the eight primitives"
#   sentence    "Four docs carry an executable transcript"
#   subset      "eleven of the fourteen examples" -- the TOTAL is
#               asserted and the subset is not, because only the total
#               is a claim about the roster
#
# Cardinal words count as numbers because published prose spells small
# ones out. ORDINALS DELIBERATELY DO NOT, and that exclusion is what
# lets the tutorial roster be checked at all: `docs/README.md` calls
# `first-composition.md` "the fourth tutorial" by CHAIN POSITION while
# `tutorial-smoke.sh` names a "four-tutorial getting-started path" that
# is a DIFFERENT four -- it excludes `first-composition.md`, which opens
# by loading the crypto module and cannot be replayed on a module-less
# bar, and includes `first-vault-entity.md`. Both sentences are true. A
# checker that read every "four" near the word tutorial as one roster
# would report that agreement as drift, so "fourth" is not a number here
# and the anchor separates the replay roster from the reading chain.
#
# WHERE A COUNT IS A CURATED JUDGEMENT, PROSE STATES NO COUNT. Not every
# sentence about a roster should be guarded into agreement with it: the
# root README's examples bullet used to say "thirteen runnable
# applications", a figure reachable from the fifteen directories only by
# deciding that `kv-tutorial` is a tutorial and `hot-reload-master` is a
# companion. No mechanical roster reproduces that, so any number there
# drifts by construction. Those sentences are restated by ROLE instead,
# the way `scripts/README.md`'s script roster was, and this check has
# nothing to assert about them -- which is the intended outcome, not a
# gap.

CARDINALS = {
    "one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6,
    "seven": 7, "eight": 8, "nine": 9, "ten": 10, "eleven": 11,
    "twelve": 12, "thirteen": 13, "fourteen": 14, "fifteen": 15,
    "sixteen": 16, "seventeen": 17, "eighteen": 18, "nineteen": 19,
    "twenty": 20,
}
NUM = r"(?:\d+|%s)" % "|".join(sorted(CARDINALS, key=len, reverse=True))

# The leading words that turn a number into an assertion about the whole
# roster. "While 33 steps were registered" recounts history and will
# never go stale; "an ordinary `3 steps` in a tutorial" is an
# illustration. Both appear in scripts/README.md's own description of
# this check, and a bare `(\d+) steps` pattern flagged both.
LEAD = (r"(?:the|all|its|only|each of the|runs|registers|enumerates|"
        r"lists|has|covers|ships|carries|is|are)"
        # A verb may take a preposition before its number: "registers at
        # six points", "runs to 34 steps". Without this the claim reads
        # as prose about the roster rather than a count of it.
        r"(?:\s+(?:at|to|in|of))?")

# How a number attaches to its noun. Two forms, and keeping them
# separate is load-bearing:
#
#   "the nine bundled examples"   space, with at most one adjective
#   "a four-tutorial path"        hyphen-joined, with none
#
# Allowing an adjective in the hyphen form collapses the two and reads
# "the two in-tutorial reboot cycles" in scripts/tutorial-smoke.sh as a
# claim that two tutorials are replayed. That sentence is true and about
# something else entirely, and flagging it would be this check failing on
# correct prose -- the one outcome that makes a guard worse than nothing.
SEP = r"(?:-|\s+(?:[a-z]+\s+)?)"

# A claim that OPENS a clause needs no leading word: "Four docs carry an
# executable transcript" asserts exactly what "the four docs" would.
# Clauses are split on sentence punctuation and on both dash conventions
# this repository uses (README.md em-dashes, docs/*.md double hyphens),
# because the root README states its example roster after a dash and an
# anchor keyed to line start would never see it.
CLAUSE_SPLIT_RE = re.compile(r"--|[.;:—–]")

# Leading list markers and emphasis, stripped so a clause-initial claim
# inside a bullet is still clause-initial.
LIST_MARKER_RE = re.compile(r"^\s*(?:[-*+]\s+)?(?:\*\*|__|`)*\s*")

# A count inside a code span or quotation marks is being MENTIONED, not
# asserted, and the difference is not academic here: the section of
# scripts/README.md that documents this check quotes the very phrasings
# it recognises -- "the nine bundled examples", "eleven of the fourteen
# examples" -- and without this the check fails on its own documentation
# while the tree is correct. Prose that discusses a roster's wording has
# to be able to show the wording.
QUOTED_RE = re.compile(r"`[^`]*`|\"[^\"]*\"|\u201c[^\u201d]*\u201d")


def claim_res(noun):
    """Return the three claim regexes for a roster whose members are `noun`.

    Each yields the number asserting the roster's SIZE -- for the subset
    form that is the second number, never the first, because only the
    total is a claim about the roster.
    """
    return [
        re.compile(r"^(" + NUM + r")" + SEP + noun + r"\b", re.IGNORECASE),
        re.compile(r"\b" + LEAD + r"\s+(" + NUM + r")" + SEP + noun + r"\b",
                   re.IGNORECASE),
        re.compile(r"\b" + NUM + r"\s+of\s+the\s+(" + NUM + r")" + SEP + noun
                   + r"\b", re.IGNORECASE),
    ]


def to_number(token):
    return int(token) if token.isdigit() else CARDINALS[token.lower()]


# scripts/README.md enumerates the sweep roster, annotated, as a numbered
# list under this heading.
ENUM_RE = re.compile(r"^(\d+)\.\s")

# examples/README.md enumerates the example roster as one backticked
# directory bullet per example.
EXAMPLE_BULLET_RE = re.compile(r"^-\s+`([a-z0-9-]+)/`")

# docs/runtime-primitives.md registers the primitives as numbered H2s.
PRIMITIVE_HEADING_RE = re.compile(r"^##\s+(\d+)\.\s+\S")

# scripts/tutorial-replay.py registers the replayed tutorials as the DOCS
# list; each entry names its doc. This is the roster tutorial-smoke.sh
# and scripts/README.md restate the size of.
DOCS_ENTRY_RE = re.compile(r'^\s*\("([a-z0-9-]+)",\s*"docs/')

# Non-markdown files that state a roster's size. The markdown side needs
# no list: markdown_files() already walks the doc set and the root.
PROSE_EXTRA = ["scripts/README.md", "scripts/full-sweep.sh",
               "scripts/tutorial-smoke.sh"]


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


def readme_enumeration(heading=None):
    """Return the numbers scripts/README.md enumerates under one heading."""
    heading = heading or SWEEP_HEADING
    nums = []
    inside = False
    with open(SCRIPTS_README, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("## "):
                inside = line.strip() == heading
                continue
            if inside:
                m = ENUM_RE.match(line)
                if m:
                    nums.append(int(m.group(1)))
    return sorted(nums)


def example_dirs():
    """Return the bundled examples: every directory under examples/.

    The filesystem is the registrar here, not a list in a file. An
    example is added by creating a directory, so nothing can be added
    without this count moving.
    """
    root = os.path.join(REPO, "examples")
    names = sorted(n for n in os.listdir(root)
                   if os.path.isdir(os.path.join(root, n)))
    if not names:
        sys.exit(
            "doc-sentinel-check: found zero directories under examples/ -- "
            "the example roster cannot be empty, so this check would be "
            "asserting every stated count against nothing.")
    return names


def example_enumeration():
    """Return the examples examples/README.md lists, one bullet each."""
    nums = []
    path = os.path.join(REPO, "examples", "README.md")
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = EXAMPLE_BULLET_RE.match(line)
            if m:
                nums.append(m.group(1))
    return sorted(nums)


def replayed_tutorials():
    """Return the tutorials scripts/tutorial-replay.py registers in DOCS.

    This is the roster tutorial-smoke.sh and scripts/README.md restate
    the size of. It is NOT the tutorial reading chain -- see the ordinal
    exclusion above.
    """
    path = os.path.join(REPO, "scripts", "tutorial-replay.py")
    names = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            m = DOCS_ENTRY_RE.match(line)
            if m:
                names.append(m.group(1))
    if not names:
        sys.exit(
            "doc-sentinel-check: parsed zero entries from scripts/"
            "tutorial-replay.py DOCS -- the roster changed shape and every "
            "tutorial-count assertion would compare against nothing. Fix "
            "DOCS_ENTRY_RE.")
    return names


# The rosters whose size published prose restates.
#
# NOT HERE, and the omission is the finding rather than an oversight: the
# EIGHT RUNTIME PRIMITIVES are restated across nine files and look like
# the best candidate of all, but that noun carries legitimate subset
# claims everywhere -- "the two runtime primitives with unverified
# extension behavior" (operations.md), "Two primitives, in code"
# (README.md), "the reference examples each demonstrate one primitive"
# (application-authoring.md). Every one is true, and a size-recogniser
# over `primitives` would report all of them as drift. The subset form
# below rescues "N of the M examples" because it says which total it is
# a subset OF; these say nothing of the kind, so there is no phrasing to
# key on. A roster is checkable here only when prose that means a subset
# says so; the primitives roster does not, and forcing it to would mean
# rewriting correct published sentences to suit a checker.
ROSTERS = [
    {
        "label": "the regression sweep's steps",
        "owner": "scripts/full-sweep.sh run_step registrations",
        "noun": r"steps?",
        # `steps` is ordinary English -- "the three steps of the
        # rollback" is not this roster. The anchor is what separates a
        # claim about the sweep from a number that merely sits near the
        # word steps.
        "anchor": r"sweep",
        "count": lambda: len(sweep_steps()),
    },
    {
        "label": "the replayed getting-started tutorials",
        "owner": "scripts/tutorial-replay.py DOCS",
        "noun": r"(?:tutorials?|docs?)",
        # Narrow on purpose: `docs` and `tutorials` are both ordinary
        # words in this repo, and the reading chain is a different
        # roster of a different size that must not be caught here.
        "anchor": (r"(?:executable transcript|getting-started path|"
                   r"tutorial-smoke|tutorial-replay)"),
        "count": lambda: len(replayed_tutorials()),
    },
    {
        "label": "the new-example checklist",
        "owner": "the numbered list under scripts/README.md "
                 "'Adding a new example'",
        "noun": r"(?:points?|steps?)",
        "anchor": r"(?:Adding a new example|example checklist|registers at)",
        "count": lambda: len(readme_enumeration(CHECKLIST_HEADING)),
    },
    {
        "label": "the bundled examples",
        "owner": "the directories under examples/",
        "noun": r"(?:examples?|runnable applications?)",
        "anchor": r"(?:bundled|examples/|run-example)",
        "count": lambda: len(example_dirs()),
    },
]


def prose_claims(roster):
    """Return [(relpath, line_no, stated)] for every prose statement of
    one roster's size, across the doc set and the scripts that describe
    it.

    The anchor is a window rather than the line: a sentence wraps, and
    the number and the anchoring word routinely land on different lines
    of the same paragraph. The claim match itself is per CLAUSE, so a
    number opening a clause is recognised without a leading word.
    """
    regexes = claim_res(roster["noun"])
    anchor = re.compile(roster["anchor"], re.IGNORECASE)
    paths = [os.path.join(REPO, rel) for rel in PROSE_EXTRA]
    paths += markdown_files()
    paths += [os.path.join(REPO, "examples", "README.md")]
    out = []
    for path in sorted(set(paths)):
        if not os.path.isfile(path):
            continue
        relpath = os.path.relpath(path, REPO)
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().splitlines()
        for i, line in enumerate(lines):
            window = "\n".join(lines[max(0, i - 1):i + 2])
            if not anchor.search(window):
                continue
            seen = set()
            spoken = QUOTED_RE.sub(" ", line)
            for clause_no, clause in enumerate(CLAUSE_SPLIT_RE.split(spoken)):
                clause = LIST_MARKER_RE.sub("", clause)
                for rx in regexes:
                    for m in rx.finditer(clause):
                        key = (clause_no, m.start(1))
                        if key in seen:
                            continue
                        seen.add(key)
                        out.append((relpath, i + 1, to_number(m.group(1))))
    return out


def check_rosters(failures):
    """Assert every prose statement of every roster's size against the
    file that registers that roster. Returns the assertions made."""
    claims = 0
    for roster in ROSTERS:
        count = roster["count"]()
        stated = prose_claims(roster)
        if not stated:
            failures.append(
                "no prose anywhere states the size of %s. Either every such "
                "sentence was removed, or this roster's claim patterns "
                "stopped matching the way they are written -- both mean this "
                "roster is asserting nothing" % roster["label"])
            continue
        for relpath, line_no, value in stated:
            claims += 1
            if value != count:
                failures.append(
                    "%s:%d: prose states %d for %s; %s registers %d"
                    % (relpath, line_no, value, roster["label"],
                       roster["owner"], count))
    return claims


def check_enumerations(failures):
    """Assert the two hand-maintained enumerations against their rosters.

    A count agreeing says the right NUMBER of members is claimed; these
    say the right MEMBERS are listed. scripts/README.md walks the sweep
    step by step and examples/README.md gives each example a row, and
    both drift by omission without moving any count.
    """
    claims = 2
    registered = sweep_steps()
    enumerated = readme_enumeration()
    if enumerated != registered:
        failures.append(
            "scripts/README.md: the '%s' section enumerates %s, but "
            "scripts/full-sweep.sh registers %s -- the enumeration and the "
            "script are maintained together, same number"
            % (SWEEP_HEADING.lstrip('# '), enumerated, registered))

    dirs = example_dirs()
    listed = example_enumeration()
    if listed != dirs:
        missing = [n for n in dirs if n not in listed]
        extra = [n for n in listed if n not in dirs]
        failures.append(
            "examples/README.md: the example rows list %s, but examples/ "
            "holds %s (missing: %s; listed but absent: %s) -- every example "
            "directory gets a row"
            % (listed, dirs, missing or "none", extra or "none"))
    return claims


# THE ONE COUPLING NO COUNT AGREEMENT REACHES. docs/source-map.md cites
# the sweep's steps BY NUMBER -- a "Regression surface" column of "steps
# 11, 13, 15 (console-ext)" rows, plus a sentence naming the three
# doc-hygiene steps. Renumbering a step leaves every one of those
# pointing at the wrong check while every COUNT in the tree still agrees,
# so a count-agreement check cannot see it by construction. Existence
# can: a step number nobody registers is unresolvable whatever the counts
# say, which is what a removed or renumbered-past-the-end step produces.
#
# WHAT THIS DOES NOT REACH, said plainly so it is not mistaken for
# complete cover: two steps swapping numbers leaves both citations
# resolvable and both wrong. The only mechanical defence against that is
# requiring every citation to name the step as well as number it, and the
# doc's own naming does not survive it -- source-map.md calls step 31
# "the documented-sentinel-count check" where full-sweep.sh registers it
# as "doc-sentinel-check", both good names for the same thing and not
# matchable without a normalisation loose enough to accept wrong pairs.
# The pairing note in scripts/README.md carries that half.
#
# Opt-in per file rather than repo-wide, which is the opposite of the
# stance SEARCH_FILES takes and deliberately so: "step 3" is ordinary
# English, and examples/atomic-demo/README.md's "the unchanged counter
# across step 1 and step 3" is about that demo's own steps. A checker
# that read those as sweep citations would fail on correct prose. The
# emptiness guard below is what keeps the list from going stale silently.
STEP_CITATION_FILES = ["docs/source-map.md"]
STEP_CITATION_RE = re.compile(
    r"\bsteps?\s+(\d+(?:\s*(?:\([^)]*\))?\s*,\s*\d+)*)")
# The same citation in its other published shape: a parenthesised list
# that names each step as well as numbering it -- "the three doc-hygiene
# steps (29, the function-index check; 30, ...)". The bare-list pattern
# above stops at the opening paren and would miss exactly the sentence
# this check was added for, so both shapes are matched and every integer
# inside the parenthesis is taken as a citation.
STEP_PAREN_RE = re.compile(r"\bsteps?\s+\(([^)]*)\)")
STEP_NUMBER_RE = re.compile(r"\d+")


def check_step_citations(failures):
    """Assert every by-number citation of a sweep step resolves to a step
    scripts/full-sweep.sh actually registers."""
    registered = set(sweep_steps())
    claims = 0
    for rel in STEP_CITATION_FILES:
        path = os.path.join(REPO, rel)
        if not os.path.isfile(path):
            failures.append(
                "%s: listed in STEP_CITATION_FILES but absent -- either the "
                "file moved and this check silently stopped covering its "
                "step citations, or the list is stale" % rel)
            continue
        found = 0
        with open(path, encoding="utf-8") as fh:
            for line_no, line in enumerate(fh, 1):
                cited = [n for m in STEP_CITATION_RE.finditer(line)
                         for n in STEP_NUMBER_RE.findall(m.group(1))]
                cited += [n for m in STEP_PAREN_RE.finditer(line)
                          for n in STEP_NUMBER_RE.findall(m.group(1))]
                for num in cited:
                    found += 1
                    claims += 1
                    if int(num) not in registered:
                        failures.append(
                            "%s:%d: cites sweep step %s, which "
                            "scripts/full-sweep.sh does not register "
                            "(registered: 1..%d)"
                            % (rel, line_no, num, max(registered)))
        if not found:
            failures.append(
                "%s: states no sweep step numbers. Either the citations "
                "were removed, or STEP_CITATION_RE stopped matching the way "
                "they are written -- both mean this file's by-number "
                "coupling to full-sweep.sh is asserting nothing" % rel)
    return claims


def check_sweep_roster(failures):
    """Assert the sweep roster's own structural invariant: the step
    numbers are a contiguous 1..N, so a step can be selected by number
    and no two steps answer to one."""
    registered = sweep_steps()
    count = len(registered)
    if registered != list(range(1, count + 1)):
        failures.append(
            "scripts/full-sweep.sh: run_step numbers %s are not a contiguous "
            "1..%d -- a gap or a duplicate means a step cannot be selected by "
            "number, or two steps answer to one" % (registered, count))
    return 1


def main():
    profiles = load_profiles()
    failures = []
    claims = 0
    files_with_claims = []

    roster_claims = check_sweep_roster(failures)
    roster_claims += check_enumerations(failures)
    roster_claims += check_rosters(failures)
    roster_claims += check_step_citations(failures)

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
          "rosters published prose restates the size of (%s)"
          % (claims, ", ".join(files_with_claims), roster_claims,
             ", ".join(r["label"] for r in ROSTERS)))


if __name__ == "__main__":
    main()
