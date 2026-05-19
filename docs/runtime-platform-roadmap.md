# Runtime Platform Roadmap

eOS-kernellib's near-term roadmap commits to one cohesive lift that integrates structured-object persistence, typed-property addressing, inheritance with override, and a sandboxed scripting sublanguage into a single use-case-agnostic developer-facing programming model. On top of that programming model, a generic property-change dispatcher ships as the platform's reactive-data surface. This document is the architecture statement: what the lift integrates and why, what the dispatcher looks like, which prior-art systems inform the design, and what readiness conditions Phase 3 implementation depends on.

**Audience**: developers and architects who want to understand the platform's near-term architectural direction before Phase 3 implementation begins; downstream application authors gauging which programming-model surfaces will be available; reviewers of the Phase 3 implementation work-in-progress comparing design intent against landed code; assumes `docs/architecture.md` for the structural model, `docs/runtime-primitives.md` for the eight runtime primitives, and `docs/lpc-essentials.md` for LPC vocabulary.

**What this doc is not**: an implementation guide. The lift's components and the dispatcher's API are described at the level of architectural commitments and design intent; concrete signatures, type definitions, and code citations belong in the per-component reference docs that ship alongside the implementation. Two architectural sub-decisions are explicitly deferred to implementation time and named in the open-decisions subsection of the dispatcher section.

---

## The cohesive lift

The lift integrates five components into the platform's kernel layer as one structurally inseparable unit:

1. **Vault** — structured-object persistence with on-disk source-of-truth, schematized via per-domain XML, surviving image-orthogonal statedump cycles.
2. **Property system** — typed named properties on objects, addressable by name pattern, the storage surface the rest of the lift uses.
3. **UrHierarchy** — typed inheritance with override semantics, ancestry walked by name to resolve property lookups and observer chains.
4. **Merry** — a safe sublanguage for scripts that run inside the platform's capability boundary, with no host-runtime side effects beyond the script's declared output.
5. **Merry-on-property invocation** — the binding convention that ties the four above together: scripts stored as named properties on objects, looked up by mode and signal, walked through UrHierarchy ancestry to find inherited handlers.

The components are not separately liftable. Lifting Vault without the property system leaves Vault with no schema to address; lifting the property system without UrHierarchy leaves property lookups flat (no inheritance, no override); lifting Merry without the invocation convention leaves the sublanguage without a way to be invoked in canonical usage; lifting the invocation convention without UrHierarchy ancestry breaks inheritance semantics. The cohesive-lift framing is therefore an architectural commitment to integrate the five components together or not at all.

What the lift delivers as a programming-model surface: sandboxed scripts in a safe sublanguage, stored as named properties on typed objects, invoked by name with timing modes, with transparent inheritance via the UrHierarchy. The surface is use-case-agnostic; no specific application domain is wired in.

### Vault: structured-object persistence

Vault stores objects as XML files in a per-domain directory tree, with namespaced property elements as the storage primitive. The relationship to the host runtime's image-orthogonal persistence is additive: the host's statedump/restore mechanism captures the entire in-memory image at the bytes level (no schema); Vault adds named persistence by object identity, schematized via the XML namespaces, surviving across statedump cycles via on-disk source-of-truth. An object reconstituted from Vault has an identity that survives image-level statedump cycles, image migrations, and full host restarts from cold disk — not just from a snapshot.

The relationship is layered: Vault depends on host-runtime persistence (the daemon's in-memory state is itself statedump-persistent), but adds named-by-identity addressability on top.

### Property system: schematized addressable state

The property system stores typed property values on objects, indexed by property name. Properties are first-class — addressable, observable, settable independently of method calls. The XML namespacing convention from Vault carries forward: properties are named with a namespace prefix (`<namespace>:<name>`), allowing different concerns to attach properties to the same object without name collision.

The property system is the storage surface the dispatcher reads and writes. Observer registration, observer-script storage, and the change-log surface all key off property names.

### UrHierarchy: typed inheritance with override

UrHierarchy provides typed inheritance: objects have an UrParent reference; property lookups walk the ancestry chain when no value is found locally; subclass property writes override the inherited value. The walk is performed by name (the property-name key) rather than by class (no method-resolution-order machinery); inheritance is data-shaped rather than code-shaped.

The dispatcher's observer-inheritance mechanism reuses this exact ancestry walk: when a property changes on an object, the dispatcher looks for observer scripts on the object first, then walks the UrHierarchy to find inherited observer chains. The same lookup function the property system uses for value resolution is reused for handler resolution.

### Merry: the safe sublanguage

Merry is a scripting sublanguage with a constrained surface: no arbitrary host-runtime side effects, no access to capabilities the script's caller did not explicitly hand it, no ability to subvert the platform's atomic envelope or capability boundary. Scripts are textual content stored as property values; the platform compiles and runs them with the constraints enforced by the language itself, not by application-level checks.

The constraints are what make Merry safe to invoke from positions where the platform cannot fully trust the caller: an observer script on a user-domain property is run by the dispatcher, but the dispatcher does not need to vet the script's contents for malicious behavior — the language guarantees the script cannot exceed the dispatcher's intended capability surface.

### Merry-on-property invocation: the binding pattern

The invocation pattern that ties the four above together: scripts stored as named properties on objects, looked up by mode and signal, walked through UrHierarchy ancestry. In canonical use, every Merry invocation follows this pattern; there is no Merry-as-pure-eval entry point. The convention is:

```text
property name:  merry:<mode>:<signal>
property value: Merry script text
invocation:     run_merry(object, signal, mode, args)
lookup:         object's properties → UrHierarchy ancestors' properties
```

Lifting Merry without the invocation convention would leave the sublanguage without a way to be used in canonical platform code; lifting the invocation convention without UrHierarchy ancestry would break inheritance of handler chains. The two halves are structurally coupled.

---

## The property-change dispatcher

On top of the cohesive lift, the kernel layer ships a generic dispatcher that fires observer scripts in response to property changes. The dispatcher is the platform's reactive-data surface: changes to typed properties trigger sandboxed scripts, with timing modes, cascade-depth bounds, batching brackets, and an append-only change log for replay and audit.

The dispatcher is **property-change-triggered, not action-verb-triggered**. The trigger axis is "this typed property on this object changed to this value"; the dispatch is independent of any application-level verb namespace. An application-level verb dispatcher (act/react/witness fan-out across multi-object scenes, for example) is a downstream construct an application can layer on top; the kernel-layer dispatcher does not commit to it.

### Observer registration (KVO-shape API)

Observers register against property-name patterns. The registration API takes an object reference and a property-name pattern; the dispatcher fires the observer when any property matching the pattern changes on that object or its UrHierarchy descendants.

Property-name patterns support exact names, namespace-prefix matches, and (deferred) glob shapes. The pattern surface follows the canonical KVO key-path semantics most developers already recognize: register an observer on `entity.address.city` and the dispatcher fires when `city`'s value changes, with the old and new values passed to the observer.

Registration is a runtime operation; observers can be added and removed dynamically. Observer registrations persist via Vault on the object they observe; observer chains restored across statedump cycles automatically.

### Pre/main/post timings (SQL-trigger shape)

Each observer registration specifies a timing: **pre** (fires before the property write commits; can refuse the write by erroring), **main** (fires at commit time; cannot refuse but sees the committed value), or **post** (fires after the write commits; the change is durable when the observer runs).

The three-timing model is the SQL-trigger pattern, validated by decades of operational lessons across major database engines. Observers with side effects beyond the trigger object register **post**; observers that need to validate the write register **pre**; observers that need transactional cohort with the write register **main**.

Pre-observers run inside the host runtime's atomic envelope for the write. If a pre-observer errors, the write is rolled back as part of the atomic context; the property never observes the new value. Main-observers run inside the same atomic envelope after the value lands. Post-observers run after the atomic envelope closes; their errors do not roll back the write.

### Append-only change log (Datomic shape)

Every property change is recorded as a fact in an append-only log: entity, attribute, value, time. The log is the dispatcher's source of truth for replay, audit, and time-travel queries; observers can subscribe to the log directly (reading the change stream) as an alternative to registering against property names. The log is a first-class queryable surface.

The two access patterns — registered observers via the KVO-shape API and log-stream subscribers via the Datomic-shape API — coexist. Registered observers are convenient for scoped reactivity; log subscribers are convenient for projection-shaped derived state, audit trails, and replay of observer chains during recovery.

Retention of the change log is a deferred design decision; the options range from full append-only history through windowed retention to garbage-collected purges based on observer-subscription state. The platform's commitment is that the surface exists and is queryable; the retention policy is settable per deployment.

### Sandboxed observer scripts (Merry)

Observer scripts are written in Merry and stored as property values on the observed object. The property naming follows the Merry-on-property invocation pattern with timing as an additional axis: `merry:on:<property-path>[:<timing>]` where the timing suffix is omitted for main and named explicitly for pre or post.

```text
merry:on:address.city           main observer on address.city
merry:on:address.city:pre       pre observer on address.city
merry:on:address.*:post         post observer on any address.* property
```

The script body is plain Merry; the dispatcher invokes it via the same `run_merry` entry point used elsewhere. The script receives the object, the changed property's old and new values, and the timing as arguments; its return value contributes to the dispatch (in pre-timing, an error refuses the write; in main and post, the return is recorded in the change log).

Storing observer scripts as property values means they persist through Vault, survive restarts, and are visible to introspection. An operator inspecting an object can see what reactive behavior is wired in by reading the object's properties.

### Inheritance through UrHierarchy

The dispatcher walks the UrHierarchy when looking up observer scripts. When a property on an object changes, the dispatcher checks the object's own properties first for matching observer scripts, then walks the UrParent chain looking for inherited observers. Override semantics match the property system's: an observer defined locally on an object replaces an inherited observer of the same property name and timing; an observer defined on an ancestor is inherited by descendants unless explicitly overridden.

This is the structurally novel element of the dispatcher design. No surveyed comparable system provides typed-property inheritance with override at the observer-registration level. The combination unlocks programming-model patterns where a base class declares default reactive behavior, subclasses override or extend it, and the dispatcher honors the inheritance chain without application-level lookup logic.

### Open design decisions deferred to Phase 3 implementation

Two architectural sub-decisions inside the dispatcher will be resolved at implementation time:

- **The dispatcher's exact API surface**: key-path syntax (dot-separated vs slash-separated vs glob); observer-property naming convention details (the timing suffix's exact spelling, optional vs required); cascade-depth defaults (where to set the cap before pathological feedback loops fire) and the operator's surface for raising or lowering it; batching API shape (transaction-boundary brackets vs explicit `willChange`/`didChange` calls vs both).
- **The change-log retention policy**: full append-only history retained indefinitely; windowed retention based on a time or size budget; garbage-collected based on observer-subscription state. The trade-offs are storage cost vs replay range vs audit completeness.

Both decisions affect the dispatcher's user-facing API; both have multiple reasonable choices. The Phase 3 implementation work surfaces the design space concretely and lands one choice with documented reasoning.

---

## Prior art and lessons

Eight comparable architectures inform the dispatcher's design. The dispatcher is genuinely novel only in one dimension — observer inheritance through typed-object UrHierarchy — but each of the other dimensions has at least one comparable that has already paid the operational price of getting the shape right. The design adopts the best-validated shape per dimension rather than inventing from first principles.

### Datomic

Typed entities with a schema declaring attribute types, cardinalities, and indexes. State is an append-only log of facts (entity-attribute-value-time tuples); the current value is the latest fact per (entity, attribute). Listeners subscribe via `tx-report-queue`, a push stream of transaction reports. Inline transaction functions (`tx-fns`) run during transaction commit and can refuse the transaction by erroring. Time-travel queries replay history at any past `t`.

**Lesson for the dispatcher**: the closest semantic match. The Datomic separation between change-event-log and reactive-subscribers maps directly to the dispatcher's append-only change log plus registered observers. The tx-fn model maps cleanly to "Merry script on property change with pre/main timing." Datomic does not have typed-property inheritance with override; the dispatcher's contribution is adding that dimension to a Datomic-shaped foundation.

### iOS Key-Value Observing

The canonical property-change observer pattern. Observers register with `observeValueForKeyPath:ofObject:change:context:`, scoped to a specific object and a specific dot-separated key path. The change notification includes the old and new values. Dot-separated key paths compose: registering on `person.address.city` auto-observes through `person.address` and fires when `city` changes.

**Lesson for the dispatcher**: the canonical API shape. Most developers already know KVO's mental model. The dispatcher adopts the key-path syntax and the old/new value diff convention. KVO's biggest operational lesson — **notification storms during bulk updates** — drives the dispatcher's explicit batching-brackets requirement: bulk operations need an explicit suspend/resume around the batch so observers see one notification per logical change, not one per micro-step.

### SQL triggers

The oldest property-change-on-persistent-store precedent. Triggers fire on row insert, update, or delete; column-specific firing via WHEN clauses; BEFORE / AFTER timing modes; ROW vs STATEMENT scope. Triggers persist in the database schema; firing is transactional.

**Lesson for the dispatcher**: the operational hazards. Every major database engine has a recursion-depth cap because **cascading triggers are nightmares** without one — observer A fires, modifies a property observed by B, which fires and modifies a property observed by A, infinite loop. Lock contention on heavily-triggered tables is the second well-validated hazard. Debugging is hard because triggers fire invisibly. The dispatcher takes these as design requirements: explicit cascade-depth bound with infinite-recursion detection mandatory, explicit batching brackets for bulk operations, and introspection surface (the change log) so observers' firing is visible rather than invisible.

### Spreadsheet cells

The architectural ancestor of all reactive-property systems (VisiCalc 1979 onward). Cells whose formulas reference a changed cell auto-recompute. The dependency graph is **inferred from formula references**, not declared; the formula language is constrained by design (no I/O, no side effects beyond setting own cell) so dependencies are statically inferable.

**Lesson for the dispatcher**: the trade-off between automatic dependency tracking and explicit registration. Auto-tracking is ergonomic but requires constraining the script language so dependencies can be inferred without running the script. Merry is too unrestricted for static dependency inference; the dispatcher therefore commits to **explicit observer registration** rather than auto-tracked reads. The reactive surface is more verbose but the inference cost is zero and the dependency graph is itself a first-class artifact (the registered observers).

### MobX / Vue 3 / Solid signals

Modern fine-grained reactive UI. Observers ("reactions", "computeds", "autoruns") that *read* a property during their last execution are auto-tracked via Proxy interception or accessor wrapping. No explicit subscription; the reactivity engine watches reads and auto-registers dependencies.

**Lesson for the dispatcher**: where auto-tracking pays off and where it doesn't. Auto-tracking is ergonomic for UI code that recomputes views from data, where the read pattern is stable and the recomputation is idempotent. It introduces subtle bugs (untracked reads, conditional dependencies, "why isn't this updating?" debugging) that compound in long-running runtime contexts. For an orthogonally-persistent runtime serving long-lived applications, explicit registration is the simpler architectural choice. Auto-tracking remains a candidate for a layered library on top of the dispatcher if a specific application class needs it.

### Erlang/OTP gen_event + gen_server

Process-isolated pub-sub. `gen_event:notify` is an explicit call that triggers handlers registered via `add_handler`. State changes within a gen_server do not auto-notify — the developer must call `notify` explicitly. Handlers live in their own processes; the actor model provides isolation.

**Lesson for the dispatcher**: the layer separation. Erlang's actor isolation is a real capability-separation primitive but does not compose naturally with "property changes fire scripts" — Erlang's model is message-passing, not reactive-data. The property-change discipline must live at the language / framework layer (the dispatcher), not at the actor layer (the host runtime's call dispatch). The dispatcher accepts that capability separation per script invocation comes from Merry's sandbox, not from process isolation.

### Event sourcing

Greg Young's and Martin Fowler's pattern. Append-only events as source of truth; projections subscribe and update derived state. Events carry **intent**: `OrderPlaced` rather than `order_status changed to "placed"`.

**Lesson for the dispatcher**: events with intent are more reusable than raw property diffs, but raw property diffs are what the dispatcher fires. The dispatcher's change-log shape is closer to Datomic (entity-attribute-value-time facts) than to event sourcing (named domain events). Applications that want intent-carrying events build them as a thin layer on top of the dispatcher: an event-emitting observer translates the property change into a domain event for downstream consumers. The dispatcher's surface stays uniform; the intent layer is application-side.

### CRDT-reactive

Yjs, Automerge, Replicache. Triggers fire on CRDT operation applied locally or remotely. Per-path observer callbacks on shared documents or specific paths. The CRDT op-log persists; the current value is a fold of operations.

**Lesson for the dispatcher**: CRDT mechanics solve distributed consensus, which the single-coherence-domain architecture (see `docs/architecture.md`) explicitly does not need. The dispatcher does not adopt CRDT machinery. The per-path observer API, however, is clean prior art for "observe `entity.property` and fire when it changes" — the dispatcher's KVO-shape API matches.

### Synthesis

The dispatcher adopts:

- **Datomic's separation of concerns**: append-only change log plus reactive subscribers as two coexisting access patterns over the same change stream. The log gives time-travel, replay, and audit; the subscribers give scoped reactivity.
- **SQL triggers' operational discipline**: pre/main/post timings, explicit cascade-depth bounds with infinite-recursion detection, explicit batching brackets for bulk operations, schema-storage of observer scripts.
- **KVO's API shape**: registered observers per property-name pattern with old/new values in the notification. The mental model most developers already know.
- **Spreadsheet cells' dependency-tracking lesson taken in the negative**: explicit observer registration rather than auto-tracked reads, because Merry's surface is too unrestricted for static dependency inference and long-running runtime contexts pay too high a debugging cost for the ergonomic gain.

The dispatcher does not adopt:

- CRDT consensus machinery — outside the architecture's distributed-coherence scope.
- Auto-tracking via Proxy interception — wrong fit for the long-running runtime context.
- Erlang's process-isolation-as-capability-separation — capability separation comes from Merry's sandbox in this architecture, not from actor processes.
- Event-sourcing's intent-carrying domain events as the primary surface — applications can build intent-carrying events on top of the property-change log.

The dispatcher's novel contribution is **observer inheritance through typed-object UrHierarchy with override semantics**. No comparable surveyed system provides observer inheritance at the typed-property level; the combination is what makes the cohesive lift's programming-model surface distinct from the sum of its prior-art components.

---

## Implementation readiness for Phase 3

Phase 3 implementation begins in a separate workstream after the current workstream closes. The implementation lifts the cohesive unit into eOS-kernellib's source tree and builds the dispatcher on top. Readiness conditions before Phase 3 begins:

- **The cohesive-lift integrity claim is documented and uncontested.** This document carries it; reviewer feedback on the Phase 2 deployment is the validation. If a reviewer surfaces evidence that one of the five components is structurally separable from the others, the lift plan re-modularizes before Phase 3 begins.
- **The five components' provenance is identified.** Vault, the property system, UrHierarchy, Merry, and the invocation convention exist as working code in adjacent codebases. Phase 3 lifts that working code into the kernel layer rather than implementing the components from scratch. The adjacent-codebase paths are known to the Phase 3 workstream's planning artifact.
- **The dispatcher's two open design decisions are flagged.** The exact API surface and the change-log retention policy are deferred to implementation time per the open-decisions subsection above. Phase 3 surfaces each choice with documented reasoning before landing it.
- **The eight runtime primitives' demonstration status is current.** `docs/runtime-primitives.md` reflects the current state of each primitive's foundation and demonstration. The lift's effect on each primitive's status is predictable from the lift's contents: atomicity, hot reload, and state introspection close independently of the lift; capability separation, persistent state, and sandboxed code load close as a direct consequence of the lift's components; asynchronous events and multi-agent coherence need additional Phase 3 demonstrations beyond the lift mechanics.
- **The Phase 1 demonstrations are in place.** `examples/atomic-demo/`, `examples/hot-reload-demo/`, and the corresponding `docs/runtime-primitives.md` cross-references show the closed-independently primitives at the pre-lift level. Phase 3 demonstrations build on the same example surface for the lift-closed primitives.
- **The repository's contribution conventions are stable.** Voice (functional, declarative-topic), license posture (BSD-2-Clause-Patent for newly authored content), commit-message conventions, and review process are documented in `CONTRIBUTING.md` and stable enough that Phase 3's PR cadence does not need to renegotiate the conventions mid-implementation.

The Phase 3 workstream's first task is its own scope document: which subset of the cohesive lift lands first, which post-lift primitive demonstrations follow, what testing surface validates the lift's integrity. This roadmap is the architectural reference Phase 3 implementation cites; Phase 3's scope document is the operational plan that turns the architecture into landed commits.

---

## Where to next

- **`docs/runtime-primitives.md`** — per-primitive foundation, demonstration, status, extensions, and open work. Section 1 (Atomicity) and section 4 (Hot reload) carry the Phase 1 example references; sections 2, 3, 5 await the lift; sections 6 and 7 await Phase 3 demonstrations beyond the lift; section 8 (State introspection) is closed independently of the lift.
- **`docs/architecture.md`** — the platform's tier model, daemons, the auto-inheritance chain, the host-driver extension surface. Read first if you have not seen the structural model.
- **`docs/lpc-essentials.md`** — LPC language vocabulary including the `atomic` modifier semantics referenced throughout this roadmap.
- **`docs/code-lifecycle.md`** — how source becomes a running master, how recompilation propagates, how the object manager tracks the dependency graph. The lift's reactive-property surface layers on top of this lifecycle.
- **`examples/atomic-demo/`** and **`examples/hot-reload-demo/`** — the Phase 1 reference applications. Both deploy at the kernel-defined HTTP/1 mount point; deploying either replaces `examples/http-app/`'s deployment at the same path.
- **`CONTRIBUTING.md`** — review, voice, and license conventions for Phase 3 PRs.
