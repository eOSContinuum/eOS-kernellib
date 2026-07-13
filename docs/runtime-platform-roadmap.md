# Runtime platform roadmap

eOS-kernellib's roadmap is a set of committed platform surfaces organized into dependency-ordered waves, each item gated by an activation trigger. The integrated programming model the platform set out to ship (structured-object persistence, typed addressable properties, data-shaped inheritance, a sandboxed scripting sublanguage, and a property-change dispatcher binding them together) ships today and is documented in `docs/runtime-primitives.md` and `docs/dispatcher.md`. This document is the forward statement: which surfaces the platform commits to next, in what dependency order, and what observable condition activates each one.

**Audience**: developers and architects gauging the platform's direction before adopting it; application authors deciding whether to build a pattern at the application tier now or wait for a committed platform surface; contributors choosing what to work on next.

## How to read this roadmap

- **Waves are dependency-ordered, not dated.** A wave number says what must exist first, not when the work happens. Wave 2 items build on Wave 1 substrates. Wave 3 items are independent expansions.
- **Every item carries an activation trigger**: the observable condition that starts the work. Wave 1 is complete: its five items hardened surfaces the platform already shipped, and each now appears in the shipped inventory below. Two Wave 2 items have since fired and shipped the same way -- the port-label registry and HTTPS transport activation -- and the remaining Wave 2 and Wave 3 items wait on their named triggers.
- **Trigger-gating is the boundary discipline.** The kernel layer takes on a surface when a consumer demands it, not speculatively. A surface with no trigger met stays out of the platform, and the corresponding pattern lives at the application tier until then.

## What ships today

`docs/runtime-primitives.md` is the authoritative per-primitive status statement. The shipped inventory the roadmap builds on:

- The **property system, UrHierarchy ancestry, Merry sandbox, and property-change dispatcher**: the integrated reactive programming model (`docs/dispatcher.md`, `docs/merry-applications.md`).
- **Observer-storage formalization** (Wave 1). The observer read side and lifecycle contract: three public read-only query LFUNs on the Merry daemon (`query_observers`, `query_effective_observers`, `query_observed_paths`) exposing the local slot, the ancestry-walk view, and observed-path enumeration, all with descriptive returns; `remove_observer` by-index removal beside the coarse clear; `register_observer` path-array cross-property sugar; the `observers` verb's enumeration and `-effective` walk views. Multi-inheritance is an explicit single-parent contract, to revisit only if the hierarchy grows multiple parents. See `docs/observers.md` for the lifecycle contract.
- **Structured-object persistence** via Vault, Schema, and the XML transport (`docs/vault-applications.md`, `docs/schema.md`, `docs/xml.md`), layered over the host runtime's orthogonal persistence (`docs/persistence.md`).
- **Type-coercion helpers** (Wave 1). The `/lib/util/coercion` codec round-trips simple LPC values over the literal grammar `dumpValue` prints (full-precision floats, object references by logical name, nested arrays and mappings), refusing aliased/cyclic structures and light-weight objects loudly. The property layer's `query_ascii_property` / `set_ascii_property` accessors marshal bare property-bearing objects through the built-in `Core:Entries` schema with no per-app schema: the default state root points there, imports write through the dispatch path, and reserved `merry:*` runtime wiring stays out of the marshal by default. The substrate the Wave 2 generalized serializer builds on. See `docs/schema.md` Property-table marshaling.
- The **library upgrade cascade**: the object manager's inheritance graph plus the upgrade daemon's recompile-with-dependents flow and `call_touch` clone patching, driven by the operator `upgrade` verb (`docs/code-lifecycle.md`).
- The **logical-name registry** (the Index daemon): a colon-delimited name tree with reverse lookup, consumed by Schema and Vault.
- The **capability library** (Wave 1). The kernel layer's consolidated authority mechanism: a store daemon (`/kernel/sys/capabilityd`) holding namespaced approved-sets behind one store-backed `is_allowed` / `require_member` membership check, with an inheritable check face (`/kernel/lib/capability`) for the System-tier surfaces that can inherit it. Six gating surfaces route through the library: dispatcher registrar approval, script-space registration, the persistence helper's dump-and-exit entry point, the HTTP acceptor's binding check, console verb-elevation callers, and property-layer gating of direct `merry:on:*` observer-property writes. Five of the six check against the store, while the dump-and-exit gate uses the check face's fixed-principal `require()` and consults no stored set. See `docs/capability.md` for the mechanism, the tier-split access path, and the limitations.
- The **operator console** with history, navigation, code lifecycle, access management, and resource queries (`docs/admin-console.md`).
- **Clone addressing for operators** (Wave 1). Console verbs that take object references accept Index logical names beside LPC paths: the kernel verbs (`clone`, `destruct`, `new`, `status`) via masks in the console clonable, the observer verbs (`observers`, `register-observer`, `unregister-observer`) via the extension's shared resolver: path first, Index fallback, the same order the coercion codec uses. Clones gain a boot-stable operator-facing address (their `path#index` form is not), so the full observer registration cycle runs against clone hosts from the console. Unresolved names report a name-aware diagnostic. See `docs/admin-console.md` target resolution. Addressing-by-name only. Clone *enumeration* is the Wave 3 liveness registry's concern.
- The **logging facility** (Wave 1). The platform's `sysLog` / `info` / `debugLog` calls forward to a System-tier diagnostic daemon (`logd`) that maps each to a fixed severity over a runtime-settable threshold and appends to one persistent sink (`/usr/System/log/system.log`), with `log` / `log-level` operator verbs. Error reporting survives the atomic barrier: `errord` drains its post-rollback reports into the same sink, so diagnostics from a rolled-back execution still reach the operator durably. Distinct from the Wave 2 change log: this is the developer/operator diagnostic surface, not an application-level event replay API. See `docs/operations.md` Logging and diagnostics.
- **Port-label registry** (Wave 2). A label layer over the kernel's numeric port-to-manager registration: System-tier boot code declares a label (`admin`, `http`, `https`) for a configured port slot and connection managers register by role rather than by position in the `.dgd` port list, with loud refusals where the kernel surface is silent and an open query surface reporting what actually landed. Its "labels precede any new transport activation" ordering was honored by its first consumer, the HTTPS activation. See `docs/common-tasks.md` Bind an additional port.
- **Transport surfaces**: the consuming HTTP/1 mount convention, plus native HTTPS termination on the labeled `https` port (Wave 2 transport activation: the HTTPS bootstrap, the `examples/https-app` reference subclass of the shipped TLS server, the `tls-cert` certificate surface with host-ACME acquisition and reload-without-restart, and the tested guarantee that the private key never persists into a statedump -- `docs/operations.md` Network boundary and transport security). TLS client variants, WebSocket framing, and datagram support stay compiled at boot (see Transport posture below for the remaining activation commitments).
- **Compiled-but-unconsumed trees kept warm**: the continuation classes and the LPC self-compiler. Both compile at boot. Their consumption is roadmap work (Wave 3) or application-tier demonstration.

## Wave 1: hardening the shipped surface (complete)

Wave 1 formalized or completed five surfaces the platform already shipped: observer-storage formalization, the type-coercion helpers, the capability library, clone addressing for operators, and the logging facility. All five have landed and moved to the shipped inventory above, each with its documentation pointer. The wave heading remains as the dependency anchor the Wave 2 items build on.

## Wave 2: platform services on Wave 1 substrates

Two Wave 2 items have shipped and moved to the inventory above: the port-label registry, and transport activation for HTTPS (the server-sent-events half of that item remains below).

- **Change log.** An append-only record of property changes: per-batch mutation tuples on the dispatcher, a replay API (query changes since a batch or timestamp), per-property retention configuration, and an operator verb. The shipped batch-identity and batch-status surface is the precursor. The change log proper builds on the Wave 1 observer formalization. *Trigger: the first dispatcher consumer that needs replay or audit.*
- **Generalized value serializer.** Round-trip serialization for any LPC value (recursive structures, light-weight objects exporting their state, persistent objects by reference) without requiring a schema. The schema-driven XML export/import that Vault uses today covers the schematized case. This generalizes it, building on the Wave 1 type-coercion substrate. *Trigger: the first cross-system transfer or non-schema marshal need.*
- **Thread-local storage API with originator identity.** A kernel-API wrapper over the host's per-execution-context storage (used internally today but not exposed), and its first consumer: originator identity persisted across deferred calls, so error reports from a `call_out` chain reach the principal that started the work. The mechanism and the use case ship together. *Trigger: the first per-principal state need that crosses a deferred-call boundary.*
- **Compile-or-locate utility.** A small library for the idempotent bootstrap pattern: return the existing master if present, compile otherwise, with automatic initialization. *Trigger: the pattern recurring across enough daemons that the inline form is measurable boilerplate.*
- **Inherited property defaults.** A plain property read with no local value walks the UrHierarchy ancestry for an inherited default, completing the platform's data-shaped inheritance story. The ancestry-walk mechanism already ships for script and observer resolution. This extends it to ordinary reads. *Trigger: the first consumer needing inherited property defaults (the prototype-style base-object shape).*
- **Transport activation** for server-sent events (the HTTPS half shipped; see Transport posture below).

## Wave 3: trigger-gated expansions

Independent items, each waiting on its own demand signal.

- **Liveness registry.** One subsystem unifying per-master clone enumeration, per-owner object traversal, and clone-count tracking: three interfaces over one registry. Today, clone patching during upgrades sweeps the object table. The registry replaces the sweep. *Trigger: the first audit, GC, or patching need beyond the upgrade daemon's per-upgrade enumeration.*
- **Typed configuration store with change events.** Runtime-configurable values (booleans, integers, strings, object references) with subscription on change. Configuration is boot-static today. *Trigger: the first subsystem that must be reconfigurable at runtime.*
- **Module-load events.** Per-module load tracking with load events for staged bootstrap. The boot sequence is a hand-ordered domain list today, and that suffices. *Trigger: staged loading of third-party domains.*
- **Init sequencer.** Staged daemon initialization via deferred-call chaining when bootstrap ordering outgrows the hand-ordered list. *Trigger: pairs with module-load events.*
- **Two-level mapping.** A mapping-of-mappings collection that bypasses the host runtime's per-mapping size limit, with an iterator. *Trigger: the first persistent collection approaching the host ceiling.*
- **Relationship managers.** Bidirectional relationship state (many-to-many, many-to-one) with atomic update semantics for graph-shaped data. *Trigger: the first graph-shaped platform-tier data need. Application patterns suffice until then.*
- **Stub auto-creation.** Instantiating an abstract library directly auto-creates and compiles the tiny concrete stub, avoiding hand-written boilerplate classes. *Trigger: stub-class boilerplate measurably accumulating across applications.*
- **LPC self-compiler consumption.** The self-compiler tree ships and compiles at boot. Consuming it (AST-level safety transforms enabling bounded loading of plain LPC beyond the Merry sandbox) is the roadmap item. *Trigger: a consumer demanding bounded plain-LPC loading.*
- **Dispatcher performance and diagnostics.** Descendant-chain cache invalidation and dispatch-trace verbosity tiers. *Triggers: observed dispatch cost on deep hierarchies; diagnostic demand beyond the shipped trace flag.*

## Transport posture

The transport mechanisms are largely paid for (the TLS server and client variants, the WebSocket framing, and the buffered-connection layer all compile at boot), so the posture below is a commitment decision per transport, not a construction plan.

- **HTTPS: activated.** Native TLS 1.3 termination shipped as a platform transport: the HTTPS bootstrap on the labeled `https` port, the `examples/https-app` reference subclass (the shipped TLS server's first binding), and the `tls-cert` certificate surface -- host-ACME acquisition at configured paths, reload without restart, and the tested guarantee that the private key never persists into a statedump. The reverse-proxy doctrine is retired; proxy termination remains a valid alternative where one host fronts several services (`docs/operations.md` Network boundary and transport security).
- **HTTP streaming / server-sent events: committed, activation trigger-gated.** Composes with the shipped HTTP/1 surface. *Trigger: the first live-update consumer, a browser-facing demonstration or an integration bridge that pushes state changes outward.*
- **WebSocket: deferred, with a trigger.** The shipped framing stays compiled. *Trigger: a bidirectional consumer that server-sent events plus POST cannot serve.* No removal. No activation work until the trigger fires.
- **Outbound HTTP/HTTPS client: shipped, unproven, activation trigger-gated.** `Http1Client` and `Http1TlsClient` compile at boot; no application subclass, example, or test exercises either, and the atomic-envelope interaction is untested (`docs/application-authoring.md` Outbound connections). *Trigger: the first external-service consumer.* Activation work is a worked example plus the untested-interaction proofs, not construction.

## The application-tier boundary

Some patterns stay above the kernel layer permanently, and the roadmap commits to keeping them out:

- **Intent-carrying domain events.** The platform's change surface is property diffs (and, at Wave 2, the change log's entity-attribute-value facts). An application that wants named domain events builds them as a thin observer layer translating property changes into events. The platform surface stays uniform.
- **Verb dispatchers and scene fan-out.** Application-level action dispatch (multi-object act/react patterns) layers on the property-change dispatcher. The kernel layer does not commit to a verb namespace.
- **Auto-tracked reactivity.** Observer registration is explicit by design: the scripting surface is too unrestricted for static dependency inference, and long-running runtime contexts pay too high a debugging cost for auto-tracking's ergonomics. An auto-tracking layer is an application-tier library if a specific application class needs it.

The tier boundary is packaging convention, not enforcement (`docs/runtime-primitives.md` Appendix): a pattern proven at the application tier can be promoted into the platform when its trigger condition is met and the pattern has demonstrated its shape.

## Where to next

- **[`docs/runtime-primitives.md`](runtime-primitives.md)**: the authoritative shipped-status statement per primitive: foundation, demonstration, extensions, open work.
- **[`docs/dispatcher.md`](dispatcher.md)**: the shipped property-change dispatcher: registration, timings, batching, cycle detection, persistence.
- **[`docs/architecture.md`](architecture.md)**: capability tiers, daemons, boot sequence, the structural model under all of the above.
- **[`docs/operations.md`](operations.md)**: the deployment surface: `.dgd` configuration, boot modes, the operator console, extensions.
- **[`CONTRIBUTING.md`](../CONTRIBUTING.md)**: conventions for contributing roadmap work.
