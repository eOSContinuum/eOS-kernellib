---
name: Feature request
about: Propose a new capability, change in scope, or addition to the platform surface
title: ''
labels: enhancement
assignees: ''
---

## Summary

What capability or change, in one sentence?

## Use case

The concrete workload or workflow this enables. Avoid abstract framings; describe a specific thing a builder is trying to do and what is currently blocking them at the platform layer.

## Layer this belongs to

- [ ] Kernel layer (`src/kernel/`)
- [ ] System layer (`src/usr/System/`)
- [ ] Inheritable library (`src/lib/`)
- [ ] Application-authoring pattern (documented, not platform code)
- [ ] Build / examples / docs
- [ ] Upstream DGD driver (route to [`dworkin/dgd`](https://github.com/dworkin/dgd))

## Relationship to existing commitments

Does this proposal compose with, extend, or contradict an existing platform commitment?

- [ ] Composes with the eight runtime primitives without adding a ninth
- [ ] Adds or extends a runtime primitive (which: ___)
- [ ] Changes capability-tier discipline (specify how: ___)
- [ ] Changes a daemon contract (which daemon: ___)
- [ ] Documentation-only change

## Proposed shape

Sketch the proposal:

- Where it lives (kernel, system, library, application pattern).
- What runtime primitives it composes.
- What the surface looks like for an application author (LPC API, daemon call, configuration knob).
- What evidence would demonstrate it works (empirical observation, test script, statedump-restore round-trip).

## Alternatives considered

What other approaches did you consider, and why this one?

## Notes

Anything else relevant.
