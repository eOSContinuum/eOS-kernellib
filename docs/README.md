# eOS-kernellib documentation

This directory contains the reference documentation for eOS-kernellib: the kernel layer for orthogonally-persistent servers built on the [DGD] driver. The root `README.md` introduces the project and what the runtime platform provides. The docs here cover the platform's model, operator surface, and application-authoring patterns in depth.

**Audience**: a reader entering the documentation directly to set up the platform, evaluate whether it fits, understand its architecture, write an application on top, operate a running deployment, or contribute to the kernel layer itself.

## Documentation map

Grouped by audience and goal. Each doc opens with its own `Audience:` callout naming who it is for.

### Setup

- [evaluating.md](evaluating.md): the one-page fit brief, before investing in setup: fit and anti-fit checklist, what is proven today, the measured envelope and ceilings, and the adoption risks priced (language, tooling, exit cost, the driver dependency, the security envelope). Every claim links its owning depth doc.
- [getting-started.md](getting-started.md): first-time install of DGD and eOS-kernellib. Run the bundled example configuration. Start here if the platform is not yet booted on your machine.
- [first-hour.md](first-hour.md): hands-on tutorial from a fresh boot to the persistence loop: create a domain, compile and clone live objects, watch an observer fire, and verify everything survives a process restart. The natural next step after `getting-started.md`.
- [first-application.md](first-application.md), the second tutorial: author your own application (a key-value service) end to end: an initd and a daemon you write, verbs you drive, an atomic rollback, a hot-fix without a restart, and its store surviving a restart. Follows `first-hour.md`, before the reference patterns in `application-authoring.md`.
- [first-http-endpoint.md](first-http-endpoint.md), the third tutorial: put an HTTP face on the second tutorial's service -- a transport domain routing to your daemon, driven with `curl`, a route added live, and the persistence proof observed over the wire. Ends where [http-applications.md](http-applications.md) (the reference) and [composite-applications.md](composite-applications.md) (the assembly) begin.
- [first-composition.md](first-composition.md), the fourth tutorial: grow the third tutorial's service into a composition -- a clonable entity kind with registered names, a secondary index maintained in the same atomic write, an audit observer whose appends roll back with an aborted write, and a capability-gated admin route provisioned with operator-minted identity and session. Ends having built every mechanism [composite-applications.md](composite-applications.md) composes at scale.
- [coming-from-contemporary-infrastructure.md](coming-from-contemporary-infrastructure.md): the translation bridge for developers arriving from an application language plus managed services: what replaces the database, the queue, the deploy pipeline, and the identity provider, plus the habits to unlearn.
- [building.md](building.md): DGD build details and platform-specific notes. Read this if `getting-started.md` does not fit your platform or you need to customize the build.

### Platform model

- [architecture.md](architecture.md): capability tiers, daemons, boot sequence, auto-inheritance, System global-access, host-driver extensions. The structural reference for the platform.
- [runtime-primitives.md](runtime-primitives.md): the eight runtime primitives (atomicity, capability separation, persistent state, hot reload, sandboxed code load, asynchronous events, multi-agent coherence, state introspection), each with foundation, demonstration status, supporting extensions, and open work.
- [execution-model.md](execution-model.md), the task model: what starts a task, run-to-completion serialization, the head-of-line latency price and its `call_out` chunking mitigation, and where the atomic envelope sits.
- [capability.md](capability.md): the capability library (`capabilityd` store + inheritable check face) behind the capability-separation primitive: the mechanism, the six gating surfaces, what "capability" means here (tier-mediation, not strict object-capability) and does not, the lifecycle, the identity-principal grant path, and the limitations.
- [identity.md](identity.md): the shared identity substrate -- one record per human or agent, passkey (WebAuthn) credentials with TOFU registration, atomic rotation and the recovery order, sessions, the three-layer authorization split, and the boundary that keeps application-user identity separate from operator authentication.
- [runtime-platform-roadmap.md](runtime-platform-roadmap.md): the committed forward surfaces in dependency-ordered waves, each with an activation trigger, the transport posture, and the application-tier boundary.
- [persistence.md](persistence.md): orthogonal persistence as architectural property, the statedump cycle, hot boot, per-variable persistence semantics, and persistence boundaries.
- [code-lifecycle.md](code-lifecycle.md): compile, clone, destruct, hot reload, `call_touch`, and the object-manager event surface.
- [changing-a-running-system.md](changing-a-running-system.md), the consolidated change story: the ladder from one-object hot fix through library cascade, live state migration, reactive data change, sandboxed behavior, and host-binary swap, with the atomicity safety net under all of it.
- [dispatcher.md](dispatcher.md), the property-change dispatcher: registration and registrar approval, pre/main/post timings, batching, cascade bounds and cycle detection, persistence across snapshot restore, and the verification table.
- [observers.md](observers.md), the observer lifecycle contract: storage encoding, registration and removal surfaces, the query views, eviction, and end-of-life.

### Writing applications

- [lpc-essentials.md](lpc-essentials.md): LPC language orientation, bridging to the formal language reference at [dworkin/lpc-doc][lpc-doc]. Read this first if LPC is unfamiliar.
- [kernel-reference.md](kernel-reference.md): the function-level kernel reference (efun overrides, per-object lfuns and hooks including `patch()`, and the signature routers).
- [kernel-heritage-model.md](kernel-heritage-model.md): the kernel library's conceptual overview inherited from the upstream `dworkin/cloud-server` documentation -- motivation, directory structure, resource control, file security, and user management, for the contributor mapping the kernel tier to its origins.
- [function-index.md](function-index.md): a generated alphabetical index of every platform-authored callable -- name, kind, and the document that carries its signature.
- [kernel-libraries.md](kernel-libraries.md): inheritable libraries under `src/lib/` (strings, persistent collections, large arrays, iteration, asynchronous control, time, utilities).
- [where-code-belongs.md](where-code-belongs.md), placement doctrine: plain LPC at a capability tier versus a Merry script on a property, and for plain LPC which shape (library, daemon, cloneable, utility), with the authority choke-point and composition-seam disciplines behind the choices.
- [common-tasks.md](common-tasks.md): task-shaped recipes for the recurring author jobs -- test driver, recurring work, live-state migration, access grants, operator verbs, extra ports -- each linking the doc that owns its mechanism.
- [application-authoring.md](application-authoring.md): general tier-E application patterns, owner/access conventions, `call_touch` upgrade model, and non-HTTP transports.
- [application-repository.md](application-repository.md): the repo-and-tree posture for an application team -- what your repository owns, composing onto a platform checkout for dev, CI, and deploy, System-tier overlay files, and platform pin updates.
- [debugging-applications.md](debugging-applications.md): reading error traces, where diagnostics land, the observer-didn't-fire checklist, and the day-to-day working environment.
- [http-applications.md](http-applications.md), HTTP/1-specific patterns: mount-point convention, application layout, the server object's role, body-bearing methods, the five platform contracts.
- [schema.md](schema.md), the Schema subsystem: namespace-indexed typed-element registry, type-system dispatcher, the schema-for-schemas bootstrap, namespace vocabulary, property-table marshaling (Core:Entries + the coercion codec).
- [xml.md](xml.md), the XML transport subsystem: parser, generator, XMD internal form, type registration with the Schema dispatcher, naming conventions.
- [vault-applications.md](vault-applications.md), Vault-specific patterns: participating-domain contract, property-bearing clonables, per-application schema registration, on-disk XML shape, round-trip cycle, cross-domain access requirements.
- [signal-applications.md](signal-applications.md), the smallest signal-on-property demonstration: one observer, one write, the reaction done when the write returns. Why reacting to state change is a runtime primitive here rather than assembled queue/poller/worker glue.
- [merry-applications.md](merry-applications.md), Merry-specific patterns: script-bearing object contract, the `merry:<mode>:<signal>` storage convention, the ancestry walk via `find_merry`, the static invocation surface, what the sandbox forbids.
- [chat-applications.md](chat-applications.md), multi-user chat patterns: room and user clonables, capability-token LWO, capability-gated admin verbs, and the shipped demonstrations spanning capability separation, persistence, sandboxed reactions, async events, and multi-agent coherence.
- [composite-applications.md](composite-applications.md), the composite application walkthrough: the multi-domain route registry, the connection-object-to-daemon seam, binding WebAuthn/session authentication to the wire through the authd facade, and the outbound HTTP client's working shape.
- [merry-language.md](merry-language.md), Merry-the-language reference: dialect restrictions over LPC, the four extensions (`$arg`, `${obj}`, `$delay()`, `space::method()`), the compile pipeline, AST node types, the 51-entry sandbox surface, the fifteen merryfuns with full signatures. Read this when writing Merry source, not just binding it.

### Operations

- [operations.md](operations.md): the `.dgd` configuration, boot modes, state persistence, backup and restore, the availability and data-loss model, logging and diagnostics, resource limits and capacity, host-driver extension loading. The deployment surface.
- [admin-console.md](admin-console.md), the operator's console (verb-based REPL on `telnet_port`): connecting, console security posture, per-task operational reference, verb appendix.
- [security-posture.md](security-posture.md), the consolidated security overview: trust boundaries, what the platform enforces, the operator's deployment responsibilities, and the known limits. Routes to the authority model (`capability.md`), the deployment perimeter (`operations.md`), and the reporting policy (`../SECURITY.md`).

### Working examples

`../examples/README.md` is the in-directory index. `../scripts/README.md` documents the one-command harness that drives the sentinel-bearing examples.

- `../examples/http-app/`: minimal HTTP/1 application with `GET /health`, `POST /echo`, and a 404 fallback. Read alongside [http-applications.md](http-applications.md).
- `../examples/https-app/`, the TLS mount point: an `Http1TlsServer` subclass cloned per connection on the labeled `https` port. Read alongside [operations.md](operations.md) Network boundary and transport security.
- `../examples/composite-app/`, the composite transport-connected application: a WWW routing domain plus an Inventory application domain, WebAuthn/session authentication bound to the wire, a persistent daemon with a synchronous audit observer, and the capability-gated admin route, all driven over real TCP. Read alongside [composite-applications.md](composite-applications.md).
- `../examples/vault-app/`, minimal Vault application: a property-bearing clonable persisted via on-disk XML with a boot-time round-trip test. Read alongside [vault-applications.md](vault-applications.md).
- `../examples/signal-app/`, minimal signal application: a one-inherit property host, one Merry observer, a synchronous-fire assertion. Read alongside [signal-applications.md](signal-applications.md).
- `../examples/merry-app/`, minimal Merry application: a property + ur-bearing clonable, an ancestry-walk assertion through `run_merry`, and a sandbox-firing assertion. Read alongside [merry-applications.md](merry-applications.md).
- `../examples/chat-app/`, multi-user chat application: Room and User clonables, an admin-token LWO, capability-gated admin verbs, a three-boot test driver whose assertions span the runtime-primitive demonstrations (capability gates, persistence, sandboxed reactions, atomic events, multi-agent coherence). Read alongside [chat-applications.md](chat-applications.md).
- `../examples/atomic-demo/`, the atomicity demonstration: a counter mutates inside an `atomic` function that errors, and the runtime rolls the mutation back. The empirical evidence behind [runtime-primitives.md](runtime-primitives.md) §1.
- `../examples/hot-reload-demo/`, the hot-reload demonstration: new LPC source recompiled into the live runtime via `compile_object`, the next dispatch picking up the new program. The evidence behind [runtime-primitives.md](runtime-primitives.md) §4.
- `../examples/hot-reload-master/`, clone-upgrade demonstration: recompiling a clonable master propagates the new program to existing clones while each keeps its state. Read alongside [code-lifecycle.md](code-lifecycle.md).
- `../examples/upgrade-cascade/`, library-upgrade demonstration: upgrading a parent library through the upgrade daemon recompiles its inheritors and `call_touch`-patches their existing clones, each keeping its state. Read alongside [code-lifecycle.md](code-lifecycle.md) Library upgrade.
- `../examples/webauthn-app/`, WebAuthn substrate demonstration: the base64url, CBOR, and COSE_Key decoding a relying party performs on credential payloads (RFC 4648 and RFC 8949 vectors with strict-reject batteries), plus -- with the crypto module -- registration and assertion ceremony verification against foreign-generated vectors including the negative batteries. Read alongside [kernel-libraries.md](kernel-libraries.md) Utilities.
- `../examples/agent-app/`, the agent identity worked example: controller registration through the `authd` facade, self-service agent minting, the foreign-signed key ceremony with its refusal battery, delegation through the operator continuation, suspension and resume. Read alongside [identity.md](identity.md) Agent identities.

### Reference

- [source-map.md](source-map.md): a map of the source tree and a fast index from each subsystem to the code that implements it and the doc that explains it. The navigation companion to `architecture.md`.
- [glossary.md](glossary.md): definitions for terms used inline across the doc set (atomic, auto-inheritance, dataspace, statedump, hotboot, capability tier, mount point, principal, master, clone, LWO, and similar).
- [kernel-reference.md](kernel-reference.md) carries the "Where signatures live" router: which doc holds the signature for each kind of callable (efun override, library class, property surface, daemon LFUN, merryfun, console verb).
- [system-daemons.md](system-daemons.md): the System-daemon application surface -- per-function signatures, gating, and semantics for objectd, upgraded, errord, logd, capabilityd, identityd, webauthnd, sessiond, authd, and the Index daemon.
- [references.md](references.md): citations for the orthogonal-persistence literature (Atkinson and Morrison 1995; KeyKOS / EROS), DGD mailing-list discussions (Allen 2000, Croes 2003, Croes 2010), and upstream documentation (DGD itself, lpc-doc, kernellib lineage).

## Reading paths

Common goals and the docs that serve them.

- **Run the platform and see it work**: [getting-started.md](getting-started.md), then `DGD_BIN=... ../scripts/run-example.sh merry-app` to watch the assertion sentinels pass, then [first-hour.md](first-hour.md) and [first-application.md](first-application.md), then `../examples/http-app/README.md`.
- **Evaluate whether the platform fits**: start at [evaluating.md](evaluating.md), the one-page brief that assembles the verdict (its closing section orders the depth reading by remaining budget). The depth links: [runtime-primitives.md](runtime-primitives.md) for what is proven today (`../scripts/run-example.sh` runs the proofs), [runtime-platform-roadmap.md](runtime-platform-roadmap.md) for the ships-today-versus-next boundary, [operations.md](operations.md) Limits and capacity for the envelope, [security-posture.md](security-posture.md) for the trust boundaries and operator responsibilities, [architecture.md](architecture.md) The driver dependency for the AGPL runtime's license boundary and continuity posture, [coming-from-contemporary-infrastructure.md](coming-from-contemporary-infrastructure.md) for what the platform replaces, and [debugging-applications.md](debugging-applications.md) The working environment, plainly for a team's day-to-day (editor reality, source-of-truth discipline, CI).
- **Arriving from a cloud-services stack**: [coming-from-contemporary-infrastructure.md](coming-from-contemporary-infrastructure.md), then [persistence.md](persistence.md) Why orthogonal persistence.
- **Do a recurring author task** (add a test driver, schedule recurring work, migrate live state, grant cross-domain access, add an operator verb, bind another port): [common-tasks.md](common-tasks.md), each recipe linking its owning mechanism doc.
- **Understand the platform's architectural commitments**: [architecture.md](architecture.md), then [runtime-primitives.md](runtime-primitives.md), then [execution-model.md](execution-model.md) for the concurrency and latency model.
- **Audit the platform's authority model**: [architecture.md](architecture.md) Capability tiers, [runtime-primitives.md](runtime-primitives.md) §2, then [capability.md](capability.md).
- **Understand the platform's security posture**: [security-posture.md](security-posture.md), then [capability.md](capability.md) for the authority mechanism and [operations.md](operations.md) for the deployment perimeter.
- **Authenticate human or agent users**: [identity.md](identity.md) for the substrate, passkey ceremonies, recovery, and the authorization split, then [system-daemons.md](system-daemons.md) for the `identityd` / `webauthnd` / `sessiond` surfaces and [application-authoring.md](application-authoring.md) Identity and request authentication for consuming an authenticated identity; then the task recipes in [common-tasks.md](common-tasks.md) -- register-and-gate a route, mint-and-delegate an agent -- and [composite-applications.md](composite-applications.md) Authenticating a wire request for the worked wire flow (its Reading the example in stages marks the identity-free skeleton to start from).
- **Write an HTTP application**: [lpc-essentials.md](lpc-essentials.md), then the hands-on ramp [first-http-endpoint.md](first-http-endpoint.md) (assumes [first-application.md](first-application.md)), then [http-applications.md](http-applications.md) at reference depth beside `../examples/http-app/`.
- **Write a non-HTTP application**: [lpc-essentials.md](lpc-essentials.md), [first-application.md](first-application.md) for a worked build from an empty domain, then [application-authoring.md](application-authoring.md) for the patterns at reference depth.
- **Model your data**: [where-code-belongs.md](where-code-belongs.md) Where state belongs for the representation fork (property table versus plain member), then [application-authoring.md](application-authoring.md) Modeling domain data for entity shapes, lookup, and secondary indexes, [kernel-libraries.md](kernel-libraries.md) Choosing a collection with [operations.md](operations.md) Sizing a workload for the ceilings, and [persistence.md](persistence.md) Getting data out for what each representation can export.
- **Decide where a new piece of behavior lives**: [where-code-belongs.md](where-code-belongs.md), then [application-authoring.md](application-authoring.md) for the mechanics of the chosen shape.
- **React to property changes**: [lpc-essentials.md](lpc-essentials.md), [signal-applications.md](signal-applications.md), `../examples/signal-app/`, then [dispatcher.md](dispatcher.md) for dispatch semantics and [observers.md](observers.md) for the observer lifecycle contract.
- **Write a multi-user chat application**: [lpc-essentials.md](lpc-essentials.md), [chat-applications.md](chat-applications.md), `../examples/chat-app/`.
- **Compose several primitives behind one transport**: [http-applications.md](http-applications.md), [composite-applications.md](composite-applications.md), `../examples/composite-app/`.
- **Write a Vault-persisted application**: [lpc-essentials.md](lpc-essentials.md), [persistence.md](persistence.md), [vault-applications.md](vault-applications.md), `../examples/vault-app/`.
- **Add scripted, sandboxed behavior to an object**: [lpc-essentials.md](lpc-essentials.md), [runtime-primitives.md](runtime-primitives.md), [merry-applications.md](merry-applications.md), `../examples/merry-app/`.
- **Write Merry source**: [lpc-essentials.md](lpc-essentials.md), [merry-language.md](merry-language.md), then [merry-applications.md](merry-applications.md) for the binding surface.
- **Operate a running deployment**: [operations.md](operations.md), [admin-console.md](admin-console.md), [persistence.md](persistence.md).
- **Reason about hot reload and code evolution**: [code-lifecycle.md](code-lifecycle.md), [changing-a-running-system.md](changing-a-running-system.md), then the hot-reload sections of [runtime-primitives.md](runtime-primitives.md) and the `../examples/hot-reload-demo/`, `../examples/hot-reload-master/`, and `../examples/upgrade-cascade/` demonstrations.
- **Understand what survives a restart**: [persistence.md](persistence.md), then the persistence sections of [operations.md](operations.md).
- **Debug a misbehaving application**: [debugging-applications.md](debugging-applications.md), then [operations.md](operations.md) Logging and diagnostics.
- **Cross-reference an unfamiliar term mid-document**: [glossary.md](glossary.md).
- **Follow a citation back to its source**: [references.md](references.md).
- **Contribute to the kernel layer**: `../CONTRIBUTING.md`, then [lpc-essentials.md](lpc-essentials.md) if LPC is new to you (the kernel source assumes that literacy), [architecture.md](architecture.md) for the model, [source-map.md](source-map.md) to find your way around the tree, [where-code-belongs.md](where-code-belongs.md), [capability.md](capability.md), and [kernel-reference.md](kernel-reference.md) for the modified API surface. `../scripts/README.md` documents the regression harness a change must keep green. [architecture.md](architecture.md) The boot, in source order is the guided first read of the kernel source; ../CONTRIBUTING.md Anatomy of a mergeable change shows two merged units as templates.

[DGD]: https://github.com/dworkin/dgd
[lpc-doc]: https://github.com/dworkin/lpc-doc
