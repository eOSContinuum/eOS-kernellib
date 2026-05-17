# Security Policy

## Supported versions

eOS-kernellib is under active development. The [`main`](https://github.com/eOSContinuum/eOS-kernellib/tree/main) branch is the supported reporting target. The [README's Tested-against line](README.md#status) names the runtime configuration the doc set and the example application are validated against; reports targeting that configuration will be triaged first.

Tagged releases will land as the platform stabilizes; this section will name the supported version range at that point.

## Reporting a vulnerability

Please report security vulnerabilities privately, not through public issues.

**Preferred**: use GitHub's [private vulnerability reporting](https://github.com/eOSContinuum/eOS-kernellib/security/advisories/new) feature. This creates a private advisory visible only to the maintainers and the reporter.

**Fallback**: if GitHub access is unavailable or the advisory feature is not configured, email <ChristopherA@LifeWithAlacrity.com> with the subject line `eOS-kernellib security`.

In either channel, please include:

- A description of the issue and what the impact is.
- Steps to reproduce, including the runtime configuration (DGD version, eOS-kernellib commit, `.dgd` settings, and any LPC code that triggers the issue).
- Any proof-of-concept material -- LPC code, HTTP request shapes, admin-console transcripts.
- The version or commit SHA you are reporting against.

## What to expect

- **Acknowledgement**: within 5 business days of receipt.
- **Initial assessment**: within 10 business days, including whether the issue is in scope (see below) and a rough triage of severity.
- **Resolution timeline**: depends on severity and complexity. High-severity issues (capability bypass, atomic-rollback bypass, sandboxed-code-load escape, persistence corruption, statedump tampering vector) target a 30-day window from confirmation to a fix landing.
- **Coordinated disclosure**: once a fix is available, we will coordinate a disclosure date with you. We follow a default 90-day disclosure window from initial report unless extended by mutual agreement.

## Scope

In scope for this repository:

- Kernel-layer code under `src/kernel/`.
- System-layer code under `src/usr/System/`.
- The eight runtime primitives' behavior as documented in [`docs/runtime-primitives.md`](docs/runtime-primitives.md). A vulnerability is anything that breaks one of these guarantees: atomicity bypass, capability bypass, persistence corruption, hot-reload escape, code-load sandbox escape, event misdelivery, multi-agent coherence violation, or state-introspection unauthorized exposure.
- Daemon contracts (initd, userd, errord, objectd, accessd, resource_daemon, and related kernel/system daemons).
- The HTTP/1 application surface bound by `mva.dgd`-style configurations of the example application.
- The admin-console exposure shaped by the kernel layer's `admin_console.c`.
- Documentation that misleads readers into insecure configurations.

Out of scope for this repository (route to the upstream project):

- **DGD driver issues** -- report to [`dworkin/dgd`](https://github.com/dworkin/dgd). eOS-kernellib depends on DGD but does not modify it.
- **Upstream kernellib lineage issues** affecting code we inherited unchanged -- report appropriately to [`dworkin/cloud-server`](https://github.com/dworkin/cloud-server) or [`ChatTheatre/kernellib`](https://github.com/ChatTheatre/kernellib).

If you are unsure whether an issue belongs to eOS-kernellib or to one of the upstream layers, report it here and we will route it.

## Out-of-scope reports

We will close the following as out of scope:

- Issues affecting only abandoned forks or third-party derivatives.
- Theoretical issues without a demonstrated path to impact.
- Vulnerability reports against an unsupported runtime configuration (e.g., DGD versions older than the [README's Tested-against baseline](README.md#status)) where the fix would be a configuration change in the reporter's deployment.

## Acknowledgement

We acknowledge confirmed valid reports in the relevant fix commit's body and, with the reporter's consent, in the project's release notes. We don't currently offer monetary bounties.
