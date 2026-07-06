<!-- Thanks for contributing to eOS-kernellib. Please fill out the sections below. -->

## Summary

What this PR changes, in one or two sentences.

## Motivation

Why this change is worth making. Link the issue that introduced the work, if any.

Closes #

## Changes

Per file or area, what changed:

- `<path>`:
- `<path>`:

## Verification

How the change was verified. Reproducible steps that a reviewer can run locally:

```sh
# Steps to verify
```

Expected output or behavior:

```text
# What should appear
```

## Platform touch points

If this PR touches load-bearing platform commitments, name them:

- [ ] Touches one of the eight runtime primitives (which: ___, see [docs/runtime-primitives.md](../docs/runtime-primitives.md))
- [ ] Changes capability-tier discipline (see [docs/architecture.md](../docs/architecture.md))
- [ ] Changes a daemon contract (which daemon: ___)
- [ ] Changes the boot sequence (cold / restore / hot)
- [ ] Adds or modifies a library under `src/lib/`
- [ ] Touches the example HTTP/1 application or its docs
- [ ] Documentation-only change
- [ ] Build / scripts / .github

## Checklist

- [ ] Commits are atomic and signed (`git commit -S -s`).
- [ ] Commit messages are ASCII only; no AI co-author attribution.
- [ ] [CONTRIBUTING.md](../CONTRIBUTING.md) conventions followed.
- [ ] Empirical evidence included for behavior changes (an empirical Observation in the relevant doc, a transcript, a statedump round-trip).
- [ ] Documentation updated where reader expectations change.
- [ ] If a primitive's behavior changed, [docs/runtime-primitives.md](../docs/runtime-primitives.md) updated.
