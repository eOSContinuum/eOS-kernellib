<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# eOS-kernellib

**Tested against**: DGD 1.7.9 (March 2026) on macOS 26.4 (arm64), validated through the BD-1 through HW-3 work captured in `doc/getting-started.md` and verified as of 2026-05-15.

eOS-kernellib is the kernel layer for orthogonally-persistent servers built on the [DGD] driver. It is the runtime platform above the driver and below an application — providing capability tiers, daemons, and runtime primitives the application uses to express its own logic.

An orthogonally-persistent server treats in-memory state as the primary state of the system. Objects survive restart without explicit serialization; transactions roll back partial effects on failure; loaded code joins the running runtime under capability bounds. eOS-kernellib makes these properties available to the application above it.

DGD has carried these properties since 2000; Christopher Allen's [contemporary MUD-Dev description][allen-dgd-2000] names them concisely — "DGD maintains persistence as a characteristic of its runtime environment," "atomic function calls allow full system-state rollback in the event of a run-time error," "full system state dump files implement persistence across reboots." eOS-kernellib is a contemporary repackaging of the same platform properties around a documented kernel-layer surface for builders who are not specifically writing for the platform's original use case.

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html

## What it provides

- **Atomicity** — Operations commit wholly or roll back wholly. Partial effects do not escape on failure.
- **Capability separation** — Code runs under a capability tier that bounds what it can call.
- **Persistent state** — The in-memory object graph survives restart without explicit serialization.
- **Hot reload** — Code recompiles into the live runtime; existing objects update in place.
- **Sandboxed code load** — New code compiles into the runtime under capability bounds set at load time.
- **Asynchronous events** — Event delivery is atomic with the state change that produced it.
- **Multi-agent coherence** — Multiple callers see a consistent view of state without user-land coordination.
- **State introspection** — The state graph is queryable directly through runtime calls.

`doc/architecture.md` covers the architecture: capability tiers, daemons, boot sequence, auto-inheritance, System global-access, and host-driver extensions. `doc/runtime-primitives.md` covers each primitive's foundation, demonstration status, supporting extensions, and open work.

Treating these eight as runtime primitives is the architectural commitment of eOS-kernellib: each one is a runtime guarantee the application inherits rather than a pattern the application reimplements. An orthogonally-persistent server cannot fake them at the application layer — atomicity requires runtime cooperation with the transaction manager; persistence requires runtime cooperation with the storage manager; capability separation requires runtime cooperation with the access checks; hot reload requires runtime cooperation with the dispatcher. Asking the application to provide them is asking it to reproduce the runtime in user space.

## Quickstart

New to eOS-kernellib? Read `doc/getting-started.md` for first-time install of DGD plus this repository, then run the bundled example configuration. After that, `doc/architecture.md` orients you to the platform model and `doc/application-authoring.md` covers writing your own application on top.

## Documentation

- **Setup** — `doc/getting-started.md` (first-time setup, install DGD, run the example configuration), `doc/building.md` (DGD build details, platform-specific notes)
- **Platform model** — `doc/architecture.md` (capability tiers, daemons, boot sequence, auto-inheritance, host-driver extensions), `doc/runtime-primitives.md` (the eight runtime primitives with per-primitive foundation and status), `doc/persistence.md` (orthogonal persistence, statedump cycle, hot boot), `doc/code-lifecycle.md` (compile / clone / destruct / call_touch / object-manager events)
- **Writing applications** — `doc/lpc-essentials.md` (LPC language orientation, bridges to the formal spec), `doc/kernel-libraries.md` (the inheritable libraries under `src/lib/`), `doc/application-authoring.md` (general tier-E application patterns, non-HTTP transports), `doc/http-applications.md` (HTTP/1-specific patterns)
- **Operations** — `doc/operations.md` (`.dgd` configuration, boot modes, state persistence, logging, resource limits, extension loading), `doc/admin-console.md` (operator console: connecting, security posture, per-task operational reference, verb appendix)
- **Working example** — `examples/http-app/` (minimal HTTP/1 application with GET /health, POST /echo, 404 fallback)

## How it composes

eOS-kernellib sits between the [DGD] driver and the application built on top:

- **Below**: DGD provides the LPC runtime, the storage manager, and the atomic-commit boundary.
- **Above**: An application uses the kernel layer's capability tiers, daemons, and primitives to express its own logic.

The kernel layer is application-neutral. Long-running stateful workflows, customer-authored automation, durable memory for long-lived processes, agent-runtime harnesses, and other orthogonally-persistent-server use cases all build on the same runtime primitives.

## Heritage

eOS-kernellib descends from a multi-decade lineage of orthogonal-persistence runtime work:

- **[DGD]** (Felix Croes, 1993-present) — the LPC runtime this kernel layer runs on. Its original use case was the LPMud lineage of multi-user text environments; the atomicity, persistence, and hot-reload primitives the platform surfaces are the same ones that supported those environments at production scale.
- **Kernellib** (Felix Croes; declared public domain in 2016. The [ChatTheatre/kernellib] fork is released under CC0 1.0 with further public-domain declarations from Skotos Tech, Dyvers Hands, Christopher Allen, and Noah Gibbs) — the architectural ancestor of this repository. Established the tier discipline (kernel / system / user), the auto-inheritance pattern, and the per-owner resource model.
- **[SkotOS]** (Skotos Tech, 1999-2018; now owned in full by Christopher Allen) — a long-running production deployment of the kernellib pattern. Demonstrates the platform at multi-decade scale and contributes structural patterns (object Vault, signal-based events, four-phase event dispatch, the wiztool operator surface) that inform platform-layer extensions under consideration here.

Earlier work in the systems literature grounds the architectural property the platform provides. Atkinson and Morrison's *Orthogonally Persistent Object Systems* (VLDB Journal 4, 1995) names the property explicitly; the KeyKOS and EROS capability-systems literature names the runtime-enforced-capability model.

The platform's text-MUD heritage shows in some implementation details — the connection-handling vocabulary, the `people` admin_console verb, the wiztool patterns inherited from SkotOS. The runtime primitives themselves are application-neutral. Online text environments are one historical use case; the platform suits any application class that values orthogonal persistence and runtime-enforced capabilities.

[ChatTheatre/kernellib]: https://github.com/ChatTheatre/kernellib
[SkotOS]: https://github.com/ChatTheatre/SkotOS

## Contributing

Contributions are welcome. A formal `CONTRIBUTING.md` is in progress; until it lands, the practical guidance is: open an issue describing the change before submitting a pull request, keep commits atomic and signed (`git commit -S -s`), and follow the conventions visible in recent history (declarative-topic doc openers, lowercase doc filenames, ASCII commit messages). The doc set's authoring conventions are exercised across every doc in `doc/`; new content should match the established shape.

## License

`LICENSE.md` carries the full text. Files in this repository are released under either the Unlicense or BSD-2-Clause-Patent; `LICENSE.md` identifies which applies where.

[DGD]: https://github.com/dworkin/dgd
