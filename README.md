[![License: BSD-2-Clause-Patent](https://img.shields.io/badge/License-BSD--2--Clause--Patent-blue.svg)](LICENSE.md)
[![Built on: DGD](https://img.shields.io/badge/Built_on-DGD-green.svg)](https://github.com/dworkin/dgd)
[![Tested against: DGD 1.7.9](https://img.shields.io/badge/Tested_against-DGD_1.7.9-brightgreen.svg)](#quickstart)

# eOS-kernellib

Build applications where:

- **Objects survive restart** without serialization code. The in-memory object graph IS the storage layer.
- **Operations commit atomically or roll back wholly.** Partial effects do not escape on failure; the runtime undoes everything in the failing call.
- **Code recompiles into the live runtime.** Hot-reload a function and existing objects pick up the new behavior without losing state.
- **Multiple actors see coherent state** without distributed-lock coordination.

eOS-kernellib is the kernel layer that exposes these as runtime primitives — capability tiers, daemons, and the eight runtime guarantees every application inherits rather than reimplements. It sits above the [DGD] LPC driver (Felix Croes, 1993-present, which has carried orthogonal persistence at production scale for three decades) and below your application.

## What it provides

- **Atomicity** — Operations commit wholly or roll back wholly. Partial effects do not escape on failure.
- **Capability separation** — Code runs under a capability tier that bounds what it can call.
- **Persistent state** — The in-memory object graph survives restart without explicit serialization.
- **Hot reload** — Code recompiles into the live runtime; existing objects update in place.
- **Sandboxed code load** — New code compiles into the runtime under capability bounds set at load time.
- **Asynchronous events** — Event delivery is atomic with the state change that produced it.
- **Multi-agent coherence** — Multiple callers see a consistent view of state without user-land coordination.
- **State introspection** — The state graph is queryable directly through runtime calls.

`docs/architecture.md` covers the architecture: capability tiers, daemons, boot sequence, auto-inheritance, System global-access, and host-driver extensions. `docs/runtime-primitives.md` covers each primitive's foundation, demonstration status, supporting extensions, and open work.

Treating these eight as runtime primitives is the architectural commitment of eOS-kernellib: each one is a runtime guarantee the application inherits rather than a pattern the application reimplements. An orthogonally-persistent server cannot fake them at the application layer — atomicity requires runtime cooperation with the transaction manager; persistence requires runtime cooperation with the storage manager; capability separation requires runtime cooperation with the access checks; hot reload requires runtime cooperation with the dispatcher. Asking the application to provide them is asking it to reproduce the runtime in user space.

## Two primitives, in code

The counter master from `examples/atomic-demo/`, with only its header comment trimmed:

```c
inherit "/usr/System/lib/auto";

private int counter;

static void create()
{
    ::create();
    counter = 0;
}

int query()
{
    return counter;
}

atomic void increment_with_failure()
{
    counter += 1;
    error("deliberate failure for atomic-rollback demonstration");
}
```

`private int counter` is persistent state: no serialize or restore method backs it, and the value survives a process restart because the runtime's statedump captures the whole object graph, this field included. `atomic void increment_with_failure()` demonstrates the atomicity primitive: the increment and the `error()` share one envelope, so the runtime restores `counter` to its pre-call value when the error fires. `examples/atomic-demo/smoke.sh` runs this exact rollback as a three-step HTTP probe and asserts the counter is unchanged; `docs/first-application.md` builds the same two primitives into a larger service and adds the restart-survival proof this excerpt only implies.

## Quickstart

New to eOS-kernellib? Read `docs/getting-started.md` for first-time install of DGD plus this repository, then run the bundled example configuration. Then take the hands-on hour: `docs/first-hour.md` walks from a fresh boot to watching your own objects, state, and reactions survive a process restart. After that, `docs/architecture.md` orients you to the platform model and `docs/application-authoring.md` covers writing your own application on top. Arriving from a cloud-services stack? `docs/coming-from-contemporary-infrastructure.md` maps the familiar components onto the runtime. Evaluating whether the platform fits before building on it? `docs/README.md`'s "Evaluate whether the platform fits" reading path collects the proofs, the ships-today-versus-next boundary, the capacity envelope, and the security posture in one path.

**See it proven in one command.** With DGD built, the regression harness deploys an example, boots the platform, exercises it — including a full snapshot-and-restart persistence cycle — and counts the assertion sentinels:

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh merry-app
```

A passing run ends with the expected `OK` sentinel count. `scripts/README.md` documents the harness; each example under `examples/` names its own profile.

**Tested against**: DGD 1.7.9 (March 2026) on macOS 26.4 (arm64), validated as of 2026-05-15. Other POSIX-compatible systems should work; `docs/building.md` covers platform-specific build notes.

## Documentation

- **Setup** — `docs/getting-started.md` (first-time setup, install DGD, run the example configuration), `docs/first-hour.md` (hands-on tutorial: from a fresh boot to the persistence loop), `docs/building.md` (DGD build details, platform-specific notes)
- **Orientation** — `docs/coming-from-contemporary-infrastructure.md` (the translation bridge from database/queue/deploy-pipeline/IAM stacks to the runtime's mechanisms)
- **Platform model** — `docs/architecture.md` (capability tiers, daemons, boot sequence, auto-inheritance, host-driver extensions), `docs/runtime-primitives.md` (the eight runtime primitives with per-primitive foundation and status), `docs/persistence.md` (orthogonal persistence, statedump cycle, hot boot), `docs/code-lifecycle.md` (compile / clone / destruct / call_touch / object-manager events)
- **Writing applications** — `docs/lpc-essentials.md` (LPC language orientation, bridges to the formal spec), `docs/kernel-libraries.md` (the inheritable libraries under `src/lib/`), `docs/application-authoring.md` (general tier-E application patterns, non-HTTP transports), `docs/http-applications.md` (HTTP/1-specific patterns)
- **Operations** — `docs/operations.md` (`.dgd` configuration, boot modes, state persistence, logging, resource limits, extension loading), `docs/admin-console.md` (operator console: connecting, security posture, per-task operational reference, verb appendix)
- **Working examples** — nine runnable applications under `examples/`, from the minimal HTTP/1 service (`examples/http-app/`) through atomic rollback, hot reload, Vault persistence, signals, Merry scripting, and multi-user chat; the `docs/README.md` Working-examples section maps each to its companion doc

## How it composes

eOS-kernellib sits between the [DGD] driver and the application built on top:

- **Below**: DGD provides the LPC runtime, the storage manager, and the atomic-commit boundary.
- **Above**: An application uses the kernel layer's capability tiers, daemons, and primitives to express its own logic.

The kernel layer is application-neutral. Long-running stateful workflows, customer-authored automation, durable memory for long-lived processes, agent-runtime harnesses, and other orthogonally-persistent-server use cases all build on the same runtime primitives.

## Heritage

eOS-kernellib descends from a multi-decade lineage of orthogonal-persistence runtime work. The credits below are the project's canonical attribution; see `LICENSE.md` for the legal license posture.

**Built on**: [DGD] (Felix Croes, 1993-present), the LPC runtime this kernel layer runs on. AGPL-3.0 as a runtime dependency; eOS-kernellib builds *on* DGD without modifying its source. DGD has carried orthogonal-persistence, atomic rollback, and statedump-based snapshot since 2000; Christopher Allen's [contemporary MUD-Dev description][allen-dgd-2000] names the properties concisely — "DGD maintains persistence as a characteristic of its runtime environment," "atomic function calls allow full system-state rollback in the event of a run-time error," "full system state dump files implement persistence across reboots." eOS-kernellib is a contemporary repackaging of the same properties around a documented kernel-layer surface for builders who are not specifically writing for the platform's original use case.

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html

**Forked from**: [ChatTheatre/kernellib] (CC0 public domain, with further public-domain declarations from Skotos Tech, Dyvers Hands, Christopher Allen, and Noah Gibbs), which descends from Felix Croes' kernellib (declared public domain in 2016). The kernellib lineage established the tier discipline (kernel / system / user), the auto-inheritance pattern, and the per-owner resource model the platform's capability machinery rests on.

**Inspired by**: [SkotOS] (Skotos Tech, 1999-2018; now owned in full by Christopher Allen), the largest application built on this lineage and a long-running production deployment of the kernellib pattern. Pattern-level reference for application authoring on top of a kernellib-derived kernel; not a foundation. Structural patterns (object Vault, signal-based events, four-phase event dispatch, the wiztool operator surface) inform platform-layer extensions under consideration here.

**Grounded in**: the orthogonal-persistence research canon. Atkinson and Morrison's *Orthogonally Persistent Object Systems* (VLDB Journal 4, 1995) names the architectural property explicitly; the KeyKOS and EROS capability-systems literature names the runtime-enforced-capability model. Christopher Allen's 2000 MUD-Dev description of DGD names the same properties in operational vocabulary. See [docs/references.md](docs/references.md) for citation detail.

The platform's text-MUD heritage shows in some implementation details — the connection-handling vocabulary, the `people` admin_console verb, the wiztool patterns inherited from SkotOS. The runtime primitives themselves are application-neutral. Online text environments are one historical use case; the platform suits any application class that values orthogonal persistence and runtime-enforced capabilities.

[ChatTheatre/kernellib]: https://github.com/ChatTheatre/kernellib
[SkotOS]: https://github.com/ChatTheatre/SkotOS

## Contributing

Contributions are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md) for conventions: branch naming, atomic-commit discipline, signed commits, documentation shape, code style, and the pull-request flow. The [PR template](.github/PULL_REQUEST_TEMPLATE.md) and [issue templates](.github/ISSUE_TEMPLATE/) shape the surfaces for bug reports, feature requests, and questions.

## Community

eOS-kernellib participates in the [eOSContinuum](https://github.com/eOSContinuum) organization on GitHub alongside [`dgd`](https://github.com/eOSContinuum/dgd) (the DGD fork that tracks `dworkin/dgd` upstream) and the [eOS-DeepContext](https://github.com/eOSContinuum/eOS-DeepContext) graph (the cross-repository architectural commitments that span the organization's projects).

- **Bug reports, feature requests, and questions**: [Issues on this repository](https://github.com/eOSContinuum/eOS-kernellib/issues). Use the template that matches your shape.
- **Security reports**: see [SECURITY.md](SECURITY.md) for private vulnerability reporting.
- **Code of Conduct**: see [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). The conventions are minimal and trust-based; we treat contributors as capable collaborators.
- **Cross-repository discussion**: the [eOS-DeepContext](https://github.com/eOSContinuum/eOS-DeepContext) graph carries the cross-repository architectural commitments and decisions; the [`dworkin/dgd` discussion](https://github.com/dworkin/dgd/discussions) and the [DGD mailing list](https://mail.dworkin.nl/mailman/listinfo/dgd) cover driver-layer topics.

## License

eOS-kernellib is released under the BSD 2-Clause Plus Patent License. See [LICENSE.md](LICENSE.md) for the full text. Code inherited from public-domain ancestors is incorporated under those original dedications and acknowledged in the Heritage section above; the repository as a whole is governed by the BSD 2-Clause Plus Patent License.

[DGD]: https://github.com/dworkin/dgd
