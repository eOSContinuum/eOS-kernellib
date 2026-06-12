# Runtime Platform Roadmap

eOS-kernellib's roadmap is a set of committed platform surfaces organized into dependency-ordered waves, each item gated by an activation trigger. The integrated programming model the platform set out to ship — structured-object persistence, typed addressable properties, data-shaped inheritance, a sandboxed scripting sublanguage, and a property-change dispatcher binding them together — ships today and is documented in `docs/runtime-primitives.md` and `docs/dispatcher.md`. This document is the forward statement: which surfaces the platform commits to next, in what dependency order, and what observable condition activates each one.

**Audience**: developers and architects gauging the platform's direction before adopting it; application authors deciding whether to build a pattern at the application tier now or wait for a committed platform surface; contributors choosing what to work on next.

## How to read this roadmap

- **Waves are dependency-ordered, not dated.** A wave number says what must exist first, not when the work happens. Wave 2 items build on Wave 1 substrates; Wave 3 items are independent expansions.
- **Every item carries an activation trigger** — the observable condition that starts the work. Wave 1 items harden surfaces that already ship, and the friction motivating them has already been observed; their triggers are met. Wave 2 and Wave 3 items wait on their named triggers.
- **Trigger-gating is the boundary discipline.** The kernel layer takes on a surface when a consumer demands it, not speculatively. A surface with no trigger met stays out of the platform, and the corresponding pattern lives at the application tier until then.

## What ships today

`docs/runtime-primitives.md` is the authoritative per-primitive status statement. The shipped inventory the roadmap builds on:

- The **property system, UrHierarchy ancestry, Merry sandbox, and property-change dispatcher** — the integrated reactive programming model (`docs/dispatcher.md`, `docs/merry-applications.md`).
- **Structured-object persistence** via Vault, Schema, and the XML transport (`docs/vault-applications.md`, `docs/schema.md`, `docs/xml.md`), layered over the host runtime's orthogonal persistence (`docs/persistence.md`).
- The **library upgrade cascade**: the object manager's inheritance graph plus the upgrade daemon's recompile-with-dependents flow and `call_touch` clone patching, driven by the operator `upgrade` verb (`docs/code-lifecycle.md`).
- The **logical-name registry** (the Index daemon): a colon-delimited name tree with reverse lookup, consumed by Schema and Vault.
- The **operator console** with history, navigation, code lifecycle, access management, and resource queries (`docs/admin-console.md`).
- **Transport surfaces**: the consuming HTTP/1 mount convention, plus TLS server/client variants, WebSocket framing, and datagram support compiled at boot (see Transport posture below for the activation commitments).
- **Compiled-but-unconsumed trees kept warm**: the continuation classes and the LPC self-compiler. Both compile at boot; their consumption is roadmap work (Wave 3) or application-tier demonstration.

## Wave 1 — hardening the shipped surface

Each Wave 1 item formalizes or completes something already in the platform. The motivating friction is already observed; no item waits on a trigger.

- **Observer-storage formalization.** The dispatcher's observer registration ships with per-timing validation and capability gating; what is missing is the read side and the contract: a public observer-query LFUN, an observer-enumeration operator verb, finer-grained unregistration, cross-property observer encoding, multi-inheritance disambiguation, and a documented observer lifecycle contract.
- **Logging facility.** The platform's `sysLog` / `info` / `debugLog` calls are no-op stubs today. This item ships a real diagnostic facility, including error reporting that survives the atomic barrier — diagnostics from a rolled-back execution must still reach the operator. Distinct from the Wave 2 change log: this is the developer/operator diagnostic surface, not an application-level event replay API.
- **Type-coercion helpers.** Marshal helpers for mixed LPC values, enabling the ascii-property accessors that the Schema callbacks already reference but the property layer does not yet implement — so bare property-bearing objects can marshal simple values without importing the full Schema subsystem. Also the substrate for the Wave 2 generalized serializer.
- **Capability library.** A kernel-tier library providing gated approved-set mutators and entry-point checks, replacing the heterogeneous per-subsystem gating that ships today. Six surfaces migrate to it: dispatcher registrar approval, script-space registration, the persistence helper's dump-and-exit entry point, the HTTP acceptor's binding check, the console access lists, and property-layer gating of direct writes to `merry:on:*` observer properties.
- **Clone addressing for operators.** Console verbs accept clone references by logical name rather than raw `path#index` form, composing the console with the logical-name registry. Addressing-by-name only; clone *enumeration* is the Wave 3 liveness registry's concern.

## Wave 2 — platform services on Wave 1 substrates

- **Change log.** An append-only record of property changes: per-batch mutation tuples on the dispatcher, a replay API (query changes since a batch or timestamp), per-property retention configuration, and an operator verb. The shipped batch-identity and batch-status surface is the precursor; the change log proper builds on the Wave 1 observer formalization. *Trigger: the first dispatcher consumer that needs replay or audit.*
- **Generalized value serializer.** Round-trip serialization for any LPC value — recursive structures, light-weight objects exporting their state, persistent objects by reference — without requiring a schema. The schema-driven XML export/import that Vault uses today covers the schematized case; this generalizes it, building on the Wave 1 type-coercion substrate. *Trigger: the first cross-system transfer or non-schema marshal need.*
- **Port-label registry.** A label layer ("http", "admin") over the numeric port-to-manager registration that ships, so modules associate with a port by label rather than by number. *Trigger: the first new transport consumer or multi-port application; labels precede any new transport activation.*
- **Thread-local storage API with originator identity.** A kernel-API wrapper over the host's per-execution-context storage (used internally today but not exposed), and its first consumer: originator identity persisted across deferred calls, so error reports from a `call_out` chain reach the principal that started the work. The mechanism and the use case ship together. *Trigger: the first per-principal state need that crosses a deferred-call boundary.*
- **Compile-or-locate utility.** A small library for the idempotent bootstrap pattern: return the existing master if present, compile otherwise, with automatic initialization. *Trigger: the pattern recurring across enough daemons that the inline form is measurable boilerplate.*
- **Inherited property defaults.** A plain property read with no local value walks the UrHierarchy ancestry for an inherited default, completing the platform's data-shaped inheritance story. The ancestry-walk mechanism already ships for script and observer resolution; this extends it to ordinary reads. *Trigger: the first consumer needing inherited property defaults — the prototype-style base-object shape.*
- **Transport activation** for HTTPS and server-sent events — see Transport posture below.

## Wave 3 — trigger-gated expansions

Independent items, each waiting on its own demand signal.

- **Liveness registry.** One subsystem unifying per-master clone enumeration, per-owner object traversal, and clone-count tracking — three interfaces over one registry. Today, clone patching during upgrades sweeps the object table; the registry replaces the sweep. *Trigger: the first audit, GC, or patching need beyond the upgrade daemon's per-upgrade enumeration.*
- **Typed configuration store with change events.** Runtime-configurable values (booleans, integers, strings, object references) with subscription on change. Configuration is boot-static today. *Trigger: the first subsystem that must be reconfigurable at runtime.*
- **Module-load events.** Per-module load tracking with load events for staged bootstrap. The boot sequence is a hand-ordered domain list today, and that suffices. *Trigger: staged loading of third-party domains.*
- **Init sequencer.** Staged daemon initialization via deferred-call chaining when bootstrap ordering outgrows the hand-ordered list. *Trigger: pairs with module-load events.*
- **Two-level mapping.** A mapping-of-mappings collection that bypasses the host runtime's per-mapping size limit, with an iterator. *Trigger: the first persistent collection approaching the host ceiling.*
- **Relationship managers.** Bidirectional relationship state (many-to-many, many-to-one) with atomic update semantics for graph-shaped data. *Trigger: the first graph-shaped platform-tier data need; application patterns suffice until then.*
- **Stub auto-creation.** Instantiating an abstract library directly auto-creates and compiles the tiny concrete stub, avoiding hand-written boilerplate classes. *Trigger: stub-class boilerplate measurably accumulating across applications.*
- **LPC self-compiler consumption.** The self-compiler tree ships and compiles at boot; consuming it — AST-level safety transforms enabling bounded loading of plain LPC beyond the Merry sandbox — is the roadmap item. *Trigger: a consumer demanding bounded plain-LPC loading.*
- **Dispatcher performance and diagnostics.** Descendant-chain cache invalidation and dispatch-trace verbosity tiers. *Triggers: observed dispatch cost on deep hierarchies; diagnostic demand beyond the shipped trace flag.*

## Transport posture

The transport mechanisms are largely paid for — the TLS server and client variants, the WebSocket framing, and the buffered-connection layer all compile at boot — so the posture below is a commitment decision per transport, not a construction plan.

- **HTTPS: committed, activation trigger-gated.** Native TLS termination is a platform transport. *Trigger: the first network-crossing consumer.* Until activation, the documented deployment doctrine is reverse-proxy TLS termination in front of the platform's HTTP/1 port; the certificate-management story is decided at activation time.
- **HTTP streaming / server-sent events: committed, activation trigger-gated.** Composes with the shipped HTTP/1 surface. *Trigger: the first live-update consumer — a browser-facing demonstration or an integration bridge that pushes state changes outward.*
- **WebSocket: deferred, with a trigger.** The shipped framing stays compiled. *Trigger: a bidirectional consumer that server-sent events plus POST cannot serve.* No removal; no activation work until the trigger fires.

## The application-tier boundary

Some patterns stay above the kernel layer permanently, and the roadmap commits to keeping them out:

- **Intent-carrying domain events.** The platform's change surface is property diffs (and, at Wave 2, the change log's entity-attribute-value facts). An application that wants named domain events builds them as a thin observer layer translating property changes into events; the platform surface stays uniform.
- **Verb dispatchers and scene fan-out.** Application-level action dispatch (multi-object act/react patterns) layers on the property-change dispatcher; the kernel layer does not commit to a verb namespace.
- **Auto-tracked reactivity.** Observer registration is explicit by design: the scripting surface is too unrestricted for static dependency inference, and long-running runtime contexts pay too high a debugging cost for auto-tracking's ergonomics. An auto-tracking layer is an application-tier library if a specific application class needs it.

The tier boundary is packaging convention, not enforcement (`docs/runtime-primitives.md` Appendix): a pattern proven at the application tier can be promoted into the platform when its trigger condition is met and the pattern has demonstrated its shape.

## Where to next

- **`docs/runtime-primitives.md`** — the authoritative shipped-status statement per primitive: foundation, demonstration, extensions, open work.
- **`docs/dispatcher.md`** — the shipped property-change dispatcher: registration, timings, batching, cycle detection, persistence.
- **`docs/architecture.md`** — capability tiers, daemons, boot sequence, the structural model under all of the above.
- **`docs/operations.md`** — the deployment surface, including the reverse-proxy doctrine the transport posture references.
- **`CONTRIBUTING.md`** — conventions for contributing roadmap work.
