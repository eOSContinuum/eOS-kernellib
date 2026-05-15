<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# eOS-kernellib documentation

This directory contains the reference documentation for eOS-kernellib — the kernel layer for orthogonally-persistent servers built on the [DGD] driver. The root `README.md` introduces the project and what the runtime platform provides; the docs here cover the platform's model, operator surface, and application-authoring patterns in depth.

**Audience**: a reader entering the documentation directly — either to set up the platform, understand its architecture, write an application on top, or operate a running deployment.

## Documentation map

Grouped by audience and goal. Each doc opens with its own `Audience:` callout naming who it is for.

### Setup

- [getting-started.md](getting-started.md) — first-time install of DGD and eOS-kernellib; run the bundled example configuration. Start here if the platform is not yet booted on your machine.
- [building.md](building.md) — DGD build details and platform-specific notes. Read this if `getting-started.md` does not fit your platform or you need to customize the build.

### Platform model

- [architecture.md](architecture.md) — capability tiers, daemons, boot sequence, auto-inheritance, System global-access, host-driver extensions. The structural reference for the platform.
- [runtime-primitives.md](runtime-primitives.md) — the eight runtime primitives (atomicity, capability separation, persistent state, hot reload, sandboxed code load, asynchronous events, multi-agent coherence, state introspection), each with foundation, demonstration status, supporting extensions, and open work.
- [persistence.md](persistence.md) — orthogonal persistence as architectural property; the statedump cycle; hot boot; per-variable persistence semantics; persistence boundaries.
- [code-lifecycle.md](code-lifecycle.md) — compile, clone, destruct, hot reload, `call_touch`, and the object-manager event surface.

### Writing applications

- [lpc-essentials.md](lpc-essentials.md) — LPC language orientation, bridging to the formal language reference at [dworkin/lpc-doc][lpc-doc]. Read this first if LPC is unfamiliar.
- [kernel-libraries.md](kernel-libraries.md) — inheritable libraries under `src/lib/` (strings, persistent collections, iteration, asynchronous control, time, utilities).
- [application-authoring.md](application-authoring.md) — general tier-E application patterns; owner/access conventions; `call_touch` upgrade model; non-HTTP transports.
- [http-applications.md](http-applications.md) — HTTP/1-specific patterns: mount-point convention, application layout, the server object's role, body-bearing methods, the four platform contracts.

### Operations

- [operations.md](operations.md) — the `.dgd` configuration, boot modes, state persistence, logging and diagnostics, resource limits, host-driver extension loading. The deployment surface.
- [admin-console.md](admin-console.md) — the operator's console (verb-based REPL on `telnet_port`): connecting, security posture, per-task operational reference, verb appendix.

### Working example

- `../examples/http-app/` — minimal HTTP/1 application with `GET /health`, `POST /echo`, and a 404 fallback. Read alongside [http-applications.md](http-applications.md).

## Reading paths

Common goals and the docs that serve them.

- **Run the platform and see it work** — [getting-started.md](getting-started.md), then `../examples/http-app/README.md`.
- **Understand the platform's architectural commitments** — [architecture.md](architecture.md), then [runtime-primitives.md](runtime-primitives.md).
- **Write an HTTP application** — [lpc-essentials.md](lpc-essentials.md), [http-applications.md](http-applications.md), `../examples/http-app/`.
- **Write a non-HTTP application** — [lpc-essentials.md](lpc-essentials.md), [application-authoring.md](application-authoring.md).
- **Operate a running deployment** — [operations.md](operations.md), [admin-console.md](admin-console.md), [persistence.md](persistence.md).
- **Reason about hot reload and code evolution** — [code-lifecycle.md](code-lifecycle.md), then the hot-reload sections of [runtime-primitives.md](runtime-primitives.md).
- **Understand what survives a restart** — [persistence.md](persistence.md), then the persistence sections of [operations.md](operations.md).

[DGD]: https://github.com/dworkin/dgd
[lpc-doc]: https://github.com/dworkin/lpc-doc
