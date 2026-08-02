#!/usr/bin/env python3
"""Generate docs/function-index.md: an alphabetical callable-name index.

Each platform-authored signature home has its own heading or table
format, so extraction is table-driven: one rule per home names the file,
the kind label, and how a callable name is found in that home. The index
maps name -> kind -> owning doc + anchor, generated so it cannot drift
from the per-home blocks it summarizes.

Usage:
  gen-function-index.py            # write docs/function-index.md
  gen-function-index.py --check    # exit 1 if the committed file is stale
"""
import re
import sys
import os

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(REPO, "docs")
INDEX = os.path.join(DOCS, "function-index.md")


def slug(heading):
    """GitHub-style anchor slug for a heading's visible text. GitHub's
    algorithm (its GFM TableOfContents filter): lowercase, drop every
    character that is not a word character (letters, digits, underscore),
    a hyphen, or a space, then render each remaining space as a hyphen.
    It does NOT collapse consecutive hyphens -- so a removed `/` between
    two signatures leaves the two surrounding spaces as a double hyphen,
    which this must reproduce exactly for the link to resolve."""
    s = heading.replace("`", "")
    s = s.lower()
    s = re.sub(r"[^a-z0-9 _-]", "", s)
    s = s.replace(" ", "-")
    return s


def read(name):
    with open(os.path.join(DOCS, name), encoding="utf-8") as f:
        return f.read().splitlines()


def strip_markdown(text):
    """Reduce inline markdown to prose: links keep their text, backtick
    code spans and bold/italic markers are dropped."""
    text = re.sub(r"\[([^\]]+)\]\([^)]*\)", r"\1", text)
    text = text.replace("`", "")
    text = re.sub(r"\*\*([^*]+)\*\*", r"\1", text)
    text = re.sub(r"\*([^*]+)\*", r"\1", text)
    return text


def first_sentence(text, limit=90):
    """The first sentence of collapsed prose, truncated on a word
    boundary at ~limit chars with a trailing ellipsis."""
    text = re.sub(r"\s+", " ", text).strip()
    if not text:
        return ""
    m = re.search(r"[.!?](\s|$)", text)
    sentence = text[:m.end()].strip() if m else text
    if len(sentence) <= limit:
        return sentence
    truncated = sentence[:limit]
    cut = truncated.rfind(" ")
    if cut > 0:
        truncated = truncated[:cut]
    return truncated.rstrip(".,;:") + "..."


def harvest_synopsis(lines, heading_idx):
    """The first sentence of prose following the heading at heading_idx,
    or "" if the next content is not plain prose. A leading fenced code
    block (kernel-reference.md's signature blocks) is skipped past before
    looking for prose; a table, list, heading, or further fence yields an
    empty cell rather than a garbled fragment."""
    i = heading_idx + 1
    n = len(lines)

    def skip_blank(i):
        while i < n and lines[i].strip() == "":
            i += 1
        return i

    i = skip_blank(i)
    if i < n and lines[i].lstrip().startswith("```"):
        i += 1
        while i < n and not lines[i].lstrip().startswith("```"):
            i += 1
        i += 1
        i = skip_blank(i)
    if i >= n:
        return ""
    stripped = lines[i].lstrip()
    if (stripped.startswith(("|", "#", "-", "*", ">"))
            or re.match(r"^\d+\.", stripped)):
        return ""
    chunk = []
    while i < n and lines[i].strip() != "":
        ln = lines[i]
        if ln.lstrip().startswith(("|", "```", "#")):
            break
        chunk.append(ln)
        i += 1
    text = strip_markdown(" ".join(chunk))
    if not text or "|" in text:
        return ""
    return first_sentence(text)


def names_from_signature(text):
    """Every callable name in a heading that may carry more than one
    signature, however they are joined -- system-daemons.md uses ' / ',
    dispatcher.md uses the word 'and'. A name is the identifier
    immediately before a '(', so every function signature in the heading
    is captured, in order, with no assumption about the separator."""
    seen = set()
    out = []
    for m in re.finditer(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", text):
        name = m.group(1)
        if name not in seen:
            seen.add(name)
            out.append(name)
    return out


def in_fence_tracker():
    """Return a function that, fed lines in order, reports whether the
    current line sits inside a ``` code fence."""
    state = {"open": False}

    def inside(line):
        if line.lstrip().startswith("```"):
            state["open"] = not state["open"]
            return True  # the fence line itself is not content
        return state["open"]

    return inside


def collect():
    entries = []  # (name, kind, docfile, anchor, synopsis)

    def add(name, kind, docfile, anchor, synopsis=""):
        entries.append((name, kind, docfile, anchor, synopsis))

    # 1. kernel-reference.md: bare-name ### headings under the three
    #    signature sections; the overview above them is skipped. Each
    #    heading is its own signature block, so its first prose sentence
    #    (past the fenced signature) is a usable per-name synopsis.
    lines = read("kernel-reference.md")
    inside = in_fence_tracker()
    kind_by_section = {
        "Efun overrides": "efun override",
        "Per-object functions (lfuns)": "lfun",
        "Driver and user-daemon hooks": "driver/userd hook",
    }
    current = None
    for idx, ln in enumerate(lines):
        fence = inside(ln)
        m2 = re.match(r"^## (.+)$", ln)
        if m2 and not fence:
            title = m2.group(1).strip()
            current = kind_by_section.get(title)
            continue
        if current and not fence:
            m3 = re.match(r"^### `?([A-Za-z_][A-Za-z0-9_]*)`?\s*$", ln)
            if m3:
                name = m3.group(1)
                add(name, current, "kernel-reference.md", "#" + slug(name),
                    harvest_synopsis(lines, idx))

    # 2. system-daemons.md: ### `<signature>` headings (may hold two).
    #    Own heading per entry (or per pair), so harvest applies.
    lines = read("system-daemons.md")
    inside = in_fence_tracker()
    for idx, ln in enumerate(lines):
        fence = inside(ln)
        if fence:
            continue
        m = re.match(r"^### (`.+`)\s*$", ln)
        if m:
            heading = m.group(1)
            anchor = "#" + slug(ln[4:].strip())
            synopsis = harvest_synopsis(lines, idx)
            for name in names_from_signature(heading):
                add(name, "daemon API", "system-daemons.md", anchor,
                    synopsis)

    # 3. dispatcher.md: ### `<signature>` headings (skip prose ###).
    lines = read("dispatcher.md")
    inside = in_fence_tracker()
    for idx, ln in enumerate(lines):
        fence = inside(ln)
        if fence:
            continue
        m = re.match(r"^### (`.+`)\s*$", ln)
        if m and "(" in m.group(1):
            anchor = "#" + slug(ln[4:].strip())
            synopsis = harvest_synopsis(lines, idx)
            for name in names_from_signature(m.group(1)):
                add(name, "dispatcher LFUN", "dispatcher.md", anchor,
                    synopsis)

    # 4. kernel-libraries.md: ### `Class` / `/lib/...` module headings,
    #    plus the property-surface bullets under /lib/util/properties.c.
    #    The bullets share the module's own heading/anchor rather than
    #    having one of their own, so they get no per-name synopsis.
    lines = read("kernel-libraries.md")
    inside = in_fence_tracker()
    in_props = False
    for idx, ln in enumerate(lines):
        fence = inside(ln)
        if fence:
            continue
        m = re.match(r"^### `([^`]+)`\s*$", ln)
        if m:
            label = m.group(1)
            anchor = "#" + slug(ln[4:].strip())
            in_props = label.endswith("properties.c")
            synopsis = harvest_synopsis(lines, idx)
            if "/" in label or label.endswith(".c"):
                add(label, "utility module", "kernel-libraries.md", anchor,
                    synopsis)
            else:
                add(label, "library class", "kernel-libraries.md", anchor,
                    synopsis)
            continue
        if in_props:
            b = re.match(r"^- `[^`]*?([A-Za-z_][A-Za-z0-9_]*)\s*\(", ln)
            if b:
                add(b.group(1), "property surface", "kernel-libraries.md",
                    "#" + slug("/lib/util/properties.c"))

    # 5. merry-language.md: merryfun table rows `| `Name` | `sig` | ... |`.
    #    Section anchor only (no per-row anchor); every merryfun would
    #    otherwise inherit the same section-level sentence, so no synopsis.
    lines = read("merry-language.md")
    inside = in_fence_tracker()
    merry_anchor = None
    for ln in lines:
        fence = inside(ln)
        if fence:
            continue
        h = re.match(r"^#{1,3} (.*[Mm]erryfun.*)$", ln)
        if h:
            merry_anchor = "#" + slug(h.group(1))
        m = re.match(r"^\| `([A-Za-z_][A-Za-z0-9_]*)` \| `[^`]+\([^|]*` \|", ln)
        if m and merry_anchor:
            add(m.group(1), "merryfun", "merry-language.md", merry_anchor)

    # 6. http-applications.md: ### `Class` headings under ## API signatures.
    lines = read("http-applications.md")
    inside = in_fence_tracker()
    in_api = False
    for idx, ln in enumerate(lines):
        fence = inside(ln)
        if fence:
            continue
        m2 = re.match(r"^## (.+)$", ln)
        if m2:
            in_api = "API signatures" in m2.group(1)
            continue
        if in_api and ln.startswith("### "):
            anchor = "#" + slug(ln[4:].strip())
            synopsis = harvest_synopsis(lines, idx)
            # A heading may title several classes ("`HttpRequest` (...) and
            # `HttpResponse` (...)"). Take every backtick token that is a
            # bare class name, not a file path.
            for tok in re.findall(r"`([^`]+)`", ln):
                if re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", tok):
                    add(tok, "HTTP class", "http-applications.md", anchor,
                        synopsis)

    # 7. admin-console.md: verb-appendix table rows. Section anchor only;
    #    same section-sentence-for-every-verb reasoning as merryfuns, so
    #    no synopsis.
    lines = read("admin-console.md")
    inside = in_fence_tracker()
    verb_anchor = None
    for ln in lines:
        fence = inside(ln)
        if fence:
            continue
        h = re.match(r"^## (Appendix.*verb reference.*)$", ln)
        if h:
            verb_anchor = "#" + slug(h.group(1))
        m = re.match(r"^\| `([A-Za-z_][A-Za-z0-9_-]*)", ln)
        if m and verb_anchor:
            add(m.group(1), "console verb", "admin-console.md", verb_anchor)

    return entries


def parse_kind_router():
    """Extract the "Where signatures live" markdown table from
    kernel-reference.md verbatim, to mirror in the generated preamble.
    Fails loudly if the section or its table is missing, so a renamed or
    restructured router does not silently drop out of the mirror."""
    lines = read("kernel-reference.md")
    start = None
    for i, ln in enumerate(lines):
        if re.match(r"^## Where signatures live\s*$", ln):
            start = i
            break
    if start is None:
        sys.exit(
            "gen-function-index.py: kernel-reference.md is missing the "
            "'Where signatures live' section; cannot mirror it into "
            "function-index.md")
    i = start + 1
    while i < len(lines) and not lines[i].lstrip().startswith("|"):
        if re.match(r"^#{1,6} ", lines[i]):
            break
        i += 1
    table = []
    while i < len(lines) and lines[i].lstrip().startswith("|"):
        table.append(lines[i])
        i += 1
    if not table:
        sys.exit(
            "gen-function-index.py: kernel-reference.md's 'Where "
            "signatures live' section has no table to mirror")
    return table


def render(entries, kind_router_table):
    seen = set()
    rows = []
    for name, kind, docfile, anchor, synopsis in entries:
        key = (name, kind, docfile)
        if key in seen:
            continue
        seen.add(key)
        rows.append((name, kind, docfile, anchor, synopsis))
    rows.sort(key=lambda r: (r[0].lower(), r[1]))

    out = []
    out.append("# Function index")
    out.append("")
    out.append(
        "An alphabetical index of the platform's callable surfaces: each "
        "name, its kind, and the document that carries its signature. This "
        "page is generated by `scripts/gen-function-index.py` from the "
        "signature homes themselves and regenerated in the pre-PR sweep, so "
        "it cannot drift from them -- edit the owning doc, not this file.")
    out.append("")
    out.append(
        "The link resolves to the exact signature where the owning home "
        "titles each callable (efun overrides, daemon APIs, dispatcher "
        "LFUNs, library classes, HTTP classes); for the table-based homes "
        "(merryfuns, console verbs) it resolves to the table's section.")
    out.append("")
    out.append(
        "One boundary is deliberate rather than an omission: the Vault "
        "layout's five subsystems (Vault / Marshal / Schema / XML / Index) "
        "have no rows here. An application consumes them through the "
        "participating-domain contract "
        "([vault-applications.md](vault-applications.md) The "
        "participating-domain contract), not through a flat callable "
        "surface, and their file-by-file references live on their own "
        "pages ([vault-applications.md](vault-applications.md), "
        "[schema.md](schema.md), [xml.md](xml.md)).")
    out.append("")
    out.append(
        "The table below is mirrored by the generator from "
        "`kernel-reference.md`'s Where signatures live router, so a query "
        "shaped as \"what kind of callable is X\" can start here too, "
        "before dropping to the flat name index -- kernel-reference.md "
        "stays the owner; edit the router there, not the table below.")
    out.append("")
    out.extend(kind_router_table)
    out.append("")
    out.append("| Name | Kind | Signature home | Synopsis |")
    out.append("|---|---|---|---|")
    for name, kind, docfile, anchor, synopsis in rows:
        out.append("| `{}` | {} | [{}]({}{}) | {} |".format(
            name, kind, docfile, docfile, anchor, synopsis))
    out.append("")
    out.append("## By kind")
    out.append("")
    out.append(
        "The same names grouped by kind, for a \"what does the platform "
        "give me for X kind\" browse; names only, no synopses -- see the "
        "table above for those.")
    out.append("")
    kinds = sorted(set(r[1] for r in rows))
    for kind in kinds:
        names_in_kind = sorted({r[0] for r in rows if r[1] == kind},
                                key=str.lower)
        links = []
        for name in names_in_kind:
            docfile, anchor = next(
                (r[2], r[3]) for r in rows if r[0] == name and r[1] == kind)
            links.append("[`{}`]({}{})".format(name, docfile, anchor))
        out.append("**{}:** {}".format(kind, ", ".join(links)))
        out.append("")
    return "\n".join(out)


def main():
    check = "--check" in sys.argv[1:]
    content = render(collect(), parse_kind_router())
    if check:
        try:
            with open(INDEX, encoding="utf-8") as f:
                current = f.read()
        except FileNotFoundError:
            current = None
        if current != content:
            sys.stderr.write(
                "function-index.md is stale; run scripts/gen-function-index.py\n")
            sys.exit(1)
        print("function-index.md up to date")
        return
    with open(INDEX, "w", encoding="utf-8") as f:
        f.write(content)
    print("wrote " + INDEX)


if __name__ == "__main__":
    main()
