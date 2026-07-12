# Contributing to eOS-kernellib

Thanks for your interest in eOS-kernellib. This document covers the conventions visible across recent history and the practical steps for getting work merged.

## Before you start

- Read [README.md](README.md) for the project's framing and what the runtime platform provides.
- Read [docs/architecture.md](docs/architecture.md) for the architectural model: capability tiers, daemons, boot sequence, auto-inheritance, host-driver extensions.
- Skim [docs/source-map.md](docs/source-map.md) to find your way around the source tree: what lives in each directory, and which document owns each subsystem.
- Read [docs/where-code-belongs.md](docs/where-code-belongs.md) for placement doctrine: which tier and which shape (library, daemon, cloneable, utility) new code takes, and the authority choke-point behind those choices.
- Read [docs/runtime-primitives.md](docs/runtime-primitives.md) for the eight runtime primitives, each with foundation, demonstration status, supporting extensions, and open work.
- For changes that touch the DGD driver itself, contributions belong upstream at [`dworkin/dgd`](https://github.com/dworkin/dgd). eOS-kernellib builds on DGD without modifying its source.
- For changes that touch the upstream kernellib lineage, the relationships are: [`dworkin/cloud-server`](https://github.com/dworkin/cloud-server) is the immediate upstream; [`ChatTheatre/kernellib`](https://github.com/ChatTheatre/kernellib) is the public-domain ancestor; both remain valid contribution targets for changes appropriate to their scope.

## Project status

eOS-kernellib is the contemporary repackaging of the orthogonally-persistent kernellib lineage around a documented kernel-layer surface for builders. The doc set is content-and-form complete as of [README's Tested-against line](README.md#quickstart); LPC and platform behavior is stable enough for application authoring to begin on top.

Substantial changes are welcome but the bar for changing capability-tier discipline, kernel-auto inheritance, daemon contracts, or the eight runtime primitives is high; the bar for adding library code under `src/lib/`, documenting an empirical observation, or extending the `examples/http-app/` reference is low.

## How to propose a change

For non-trivial work:

1. **Open an issue first** describing the change, the use case it addresses, and how you would test it. Issues are cheap; PRs that arrive without a prior issue tend to need more rework.
2. **For architectural questions** -- capability-tier boundaries, daemon-surface changes, primitive-set additions, extension-loading mechanics -- read the relevant doc in [`docs/`](docs/) before proposing alternatives. The doc set names what each layer is responsible for.
3. **For new platform commitments** -- "the platform should always do X" -- the proposal needs to demonstrate that the commitment is a runtime guarantee (something the platform owns), not an application-layer pattern. The eight primitives are the canonical examples of platform-owned guarantees.

For trivial work (typos, small documentation fixes, broken cross-references): open a PR directly.

## Commit conventions

Commits in this repository are atomic, signed, and content-attributed to humans only.

- **Atomic**: one logical change per commit. Renames precede content edits; automated changes are separate from manual content.
- **Signed**: `git commit -S -s` (GPG-signed and DCO-style sign-off). Configured via `~/.gitconfig` once; subsequent commits inherit.
- **Message format**: imperative-mood title under 50 characters; body wraps at 72 columns; ASCII only (no em-dashes, curly quotes, or ellipsis characters).
- **No AI attribution**: commit history names the human author. AI assistance is a tool, not a co-author.
- **Body content**: lead with outcomes (what is different now), then supporting detail. For multi-file commits, the body is required and enumerates what changed per file or area.

Example:

```text
Add resource_daemon per-owner quota check

Per-owner resource quotas now check against the active principal's
quota table at allocation time, rejecting allocations that would
exceed the owner's limit before the allocation lands in the runtime.
The check fires inside the atomic operation, so a rejected allocation
rolls back wholly.

- src/usr/System/sys/resource_daemon.c: quota check added before
  the allocation handoff.
- src/lib/api/resource.c: API path exercises the new check.
- docs/operations.md: Resource limits section updated to name the
  per-owner check.
```

## Documentation shape

Documentation in this repository follows a functional, present-tense voice. Files describe what the platform does now. History and provenance belong in commit messages, not in document content. The [README's Heritage section](README.md#heritage) is the one place where lineage attribution lives; per-file history is git's job.

Two documentation surfaces:

- **Reference docs** under [`docs/`](docs/) -- the platform model, application-authoring patterns, operations surface, glossary, and citation index. Each doc opens with an `Audience` callout naming who it is for and ends with a `Where to next` section (the folder index `docs/README.md` carries reading paths instead).
- **Procedural docs** outside `docs/` -- README, CONTRIBUTING, SECURITY, this file. Follow the standard OSS-repo convention.

New docs added to `docs/` use lowercase-hyphenated filenames matching the existing pattern. The `docs/README.md` folder index is updated to include any new doc in the appropriate audience group.

## Testing

Platform behavior is exercised through the bundled examples and the regression harness under `scripts/`: `run-example.sh <example>` boots an example profile and checks its sentinel assertions (including snapshot restore where the profile exercises it), `drive-verbs-smoke.sh` drives the admin-console verbsets under `scripts/verbsets/`, and `base-boot-guard.sh` guards the bare boot. `scripts/README.md`'s Full regression sweep section enumerates every command in this harness, in order, with the pass signal for each; that sweep is the pre-PR bar. Changes to capability tiers, daemons, or the primitive surfaces should include:

- Reproducible boot evidence (cold boot, statedump restore, hot boot as applicable to the change).
- An empirical Observation in the relevant doc when the change demonstrates a previously-unverified primitive behavior.
- LPC-level evidence (admin-console transcript or HTTP probe transcript) where the change affects an externally-visible surface.

An empirical Observation is a dated statement, in the doc it supports, naming the command that ran and the output marker it produced -- a sentinel line, a response body, a status field -- specific enough that a later reader can rerun the command and check for the same marker. `docs/runtime-primitives.md`'s Demonstration entries set the citation shape: each names the exercising example or script and the sentinel or transcript line that constitutes the evidence, though they read as continuous platform-model prose rather than individually dated entries -- a new Observation adds the date.

A behavior change updates the document that owns it: `docs/source-map.md`'s "Finding a subsystem" table names, for each subsystem, the entry-point code and the doc that explains it, and that doc is the one a behavior change must update. Adding or renaming a doc under `docs/` also updates `docs/README.md`'s Documentation map, the index a new doc does not otherwise appear in.

## Anatomy of a mergeable change

Two merged units show the full shape the sections above ask for; read their diffs as templates.

- **A fix** (PR #52): a focused two-file type correction in the collection stack, shipped in the same PR as the regression verbset that drives exactly the fixed path (plus a companion console-baseline verbset), verified against a live boot and the default sweep. Internals-only, so no doc touch -- the test is the evidence.
- **A feature** (PR #31): a diagnostics-routing change across two domains, shipped with the verbset assertion that proves the new visibility end to end and the five documentation updates that keep the owning docs true. Code, proof, and prose land as one reviewed unit.

The common skeleton: one logical change, the regression that demonstrates it (sentinel driver phase, verbset entry, or example assertion), and whatever documentation the change makes stale -- in the same PR.

## Code style

LPC code follows the conventions visible across `src/`:

- Filenames are lowercase with hyphens or underscores following the surrounding directory's convention.
- The `/lib`, `/obj`, `/sys`, `/data` subdirectory convention applies inside each domain under `src/usr/`.
- Capability-tier discipline: code at tier N does not bypass tier N-1's contract. When in doubt, [`docs/architecture.md`](docs/architecture.md) names what each tier may call.
- Inherit-chain conventions: kernel-auto for `src/kernel/`, system-auto for `src/usr/System/`, application-auto chain for other tier-E domains.
- ASCII directory trees in docs use indent-based shape; Unicode box-drawing is not used.

Shell scripts under `examples/` follow POSIX conventions and target a portable invocation environment.

## Pull request flow

1. Fork the repository to your account.
2. Create a branch named for the change shape: `feature/<short-name>`, `fix/<short-name>`, `docs/<short-name>`, or `refactor/<short-name>`.
3. Commit per the conventions above.
4. Open the PR against `main`. Use the [PR template](.github/PULL_REQUEST_TEMPLATE.md) shape; fill in the linked issue, the change summary, and the verification steps.
5. Maintainer review focuses on: does the change match an existing primitive or doc commitment; does it preserve capability-tier discipline; is the test evidence reproducible.

## Code of Conduct

This project follows the [Code of Conduct](CODE_OF_CONDUCT.md). Be excellent to each other; treat contributors as capable collaborators.

## Questions

Open an issue with the `question` label, or start a discussion in the [eOSContinuum](https://github.com/eOSContinuum) organization's discussion area.
