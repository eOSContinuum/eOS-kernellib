---
name: Bug report
about: A reproducible defect in eOS-kernellib platform behavior
title: ''
labels: bug
assignees: ''
---

## Summary

What is broken, in one sentence?

## Which primitive or surface

Which platform primitive or surface does the bug affect? Multi-select OK.

- [ ] Atomicity (rollback bypass, partial-completion leak)
- [ ] Capability separation (tier bypass, unauthorized call)
- [ ] Persistent state (lost-on-restart, statedump corruption)
- [ ] Hot reload (recompile failure, dispatch failure)
- [ ] Sandboxed code load (capability bypass at load time)
- [ ] Asynchronous events (event misdelivery, lost event, race)
- [ ] Multi-agent coherence (inconsistent view across callers)
- [ ] State introspection (unauthorized exposure, missing data)
- [ ] Daemon surface (initd / userd / errord / objectd / accessd / resource_daemon)
- [ ] admin_console verb behavior
- [ ] HTTP/1 application surface
- [ ] Build / examples / docs

## Environment

- **eOS-kernellib commit**: `<git rev-parse HEAD>`
- **DGD version**: `<dgd --version output, or build-time banner>`
- **OS and architecture**: `<uname -a>`
- **Boot mode**: cold boot / statedump-restore / hot boot
- **Extensions loaded**: `<list from .dgd `modules =` if any>`

## Steps to reproduce

1.
2.
3.

## Observed behavior

What happened.

## Expected behavior

What you expected to happen, and why (a doc claim, a primitive guarantee, the LPC reference behavior).

## Evidence

Logs, HTTP transcripts, admin-console session output, LPC traces, anything reproducible. Paste inline for short evidence; attach as a file for anything beyond ~50 lines.

```text
<evidence here>
```

## Notes

Anything else relevant -- adjacent work in flight, related issues, partial mitigation you have applied locally.
