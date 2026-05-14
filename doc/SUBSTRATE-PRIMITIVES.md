<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Substrate Primitives

eOS-kernellib is the kernel layer for orthogonally-persistent servers. An orthogonally-persistent server treats in-memory state as the primary state of the system: objects survive restart without explicit serialization; operations commit wholly or roll back wholly; new code joins the running runtime under capability bounds. The substrate's properties are surfaced as eight named primitives that the application above consumes; this document is the per-primitive foundation-and-proof statement.

The architectural argument for why these eight is the framing question lives in [eOS-DeepContext]. This document is not the argument; it is the foundation, the status, and the pending proofs, primitive by primitive.

[eOS-DeepContext]: https://github.com/eOSContinuum/eOS-DeepContext

**Status legend.** Each primitive carries one of three statuses:

- **Validated** — foundation present in the substrate, demonstrated empirically by an Observation citation.
- **Partial** — foundation present, demonstration partial. Some surface of the primitive is observed; complete demonstration is not yet authored.
- **Foundation-only** — substrate mechanism is present, but no empirical demonstration of the primitive exists yet.

**Tier vocabulary** used throughout this document is defined in the Appendix. Briefly: kernel-tier code lives in `/kernel/` (tier B); System-tier code in `/usr/System/` (tier C); shipped substrate domains in `/usr/HTTP/`, `/usr/TLS/`, `/usr/LPC/`, etc. (tier D); application domains outside the kernel layer (tier E).

---

## 1. Atomicity

Operations commit wholly or roll back wholly. Partial effects do not escape on failure.

**Foundation**: DGD atomic-function semantics. A function declared `atomic` (or invoked through `call_limited` with an atomic envelope) that errors causes every state mutation performed inside it to roll back. The host runtime is the enforcement point; the application carries no roll-back code.

**Demonstration**: HTTP/1 substrate startup, observed in the bootstrap log. An HTTP1_SERVER clone attempt with mis-shaped arguments errors during the binary-port acceptor's `clone_object` call; the `[atomic]` annotation in the log marks the rollback firing; the substrate continues accepting subsequent connections from clean state.

**Status**: Partial. Foundation present; rollback observed in failure mode; a deliberate-failure demonstration with a user-authored handler is pending.

**Extensions**: None at substrate level. Atomicity is a host-runtime property; eOS-kernellib does not extend it beyond the host's contract.

**Open**: Compound-operation atomic envelopes spanning multiple sub-calls. The host envelope is per-call; multi-call transactional semantics are an application concern.

---

## 2. Capability separation

Code runs under a capability tier that bounds what it can call.

**Foundation**:
- Host-runtime per-object owner identity and tier-aware kfun access checks.
- The kernel auto (`/kernel/lib/auto.c`) and System auto (`/usr/System/lib/auto.c`) compose the access-control surface every user-tier object inherits.
- The tier model (Appendix) packages the boundary: B and C carry privileged kfun authority; D and E operate under bounds, gaining cross-domain visibility only through System's `set_global_access` mechanism.

**Demonstration**: HTTP/1 application server at the kernel-defined mount point `/usr/WWW/obj/server` (tier D) inherits `/usr/System/lib/user` (tier C) and `Http1Server` (tier D). The inheritance traverses System's global-access grant; the application receives binary-port connections only through System's `set_binary_manager` registration. The application cannot invoke that kfun directly — the privilege check fails at the host runtime, not at application logic.

**Status**: Partial. Tier model + ownership + inheritance work; per-request authorization within an application's surface (e.g., HTTP-route-level principal challenge) is absent.

**Extensions**:
- HTTP authentication and authorization sys objects at `/usr/HTTP/sys/authenticate.c` and `/usr/HTTP/sys/authorize.c`. RFC-aligned challenge / authorization primitives; compiled at boot, currently unconsumed by any application.
- Kernel-tier TLS API (port candidate): wraps the host's per-execution-context thread-local storage as a kernel-API library, useful for per-principal state visible only within an atomic execution slice.
- Unified clone management (port candidate): `clonable` library + System-tier auto extension providing per-master clone-list maintenance and clone-count tracking with capability-aware access.
- Runtime-configurable values daemon (port candidate, `configd`): typed configuration store (booleans, integers, strings, object references) with event subscription on configuration change, gated by tier-D ownership.

**Open**:
- Per-route HTTP authorization wiring (consume `authenticate`/`authorize` from an HTTP application).
- Capability-checked properties — when the property-graph layer (see §5) lands, property-write authorization layers with the existing tier model.

---

## 3. Persistent state

The in-memory object graph survives restart without explicit serialization.

**Foundation**:
- Host-runtime orthogonal persistence. The substrate's statedump mechanism captures the entire image to disk; restore reconstructs it. Objects in the image survive restart without application-level serialize / deserialize code.
- `save_object` / `restore_object` provide a complementary per-object snapshot mechanism for daemons that need an explicit save point independent of full image dumps.

**Demonstration**: Admin authentication credentials persist across restarts. Bootstrap writes the password hash; subsequent boots find the hash without an explicit save call from the admin_console object.

**Status**: Validated. Mechanism present; demonstrated empirically in the bootstrap and across reboot.

**Extensions**:
- `KVstore` at `/lib/KVstore.c` and `KVnode` at `/obj/kvnode.c`: persistent keyed-tree structure built on the persistence primitive. Compiled at boot; provides a structured collection suitable as backing storage for higher-level data primitives.
- Two-level mapping (port candidate, `bigmap` / `bigmap_iterator`): bypasses the host runtime's per-mapping size limit, with iterator pattern for subclass masking. Useful when a persistent collection grows past the host-language ceiling.

**Open**:
- Statedump scheduling and integrity policy (when does the substrate write a full image dump? What happens on dump failure?). Operator-facing; outside the substrate's enforcement surface.

---

## 4. Hot reload

Code recompiles into the live runtime; existing objects update in place.

**Foundation**: Host-runtime `compile_object` kfun. Passing a path that already has a master in memory replaces the master with the recompiled version. Existing references to the old master continue to function for the in-flight call; subsequent calls dispatch to the new version. No deploy step.

**Demonstration**: An HTTP application exposing a baseline `GET` route and a `POST /compile` route that calls `compile_object` on the route handler's source path. Sequence: baseline `GET` returns the cold-boot string; `POST /compile` with revised LPC source on the same path returns `200 OK`; immediate `GET` returns the new string. No DGD restart, no application-layer reload mechanism — `compile_object` is the substrate mechanism, exercised through the HTTP application surface.

**Status**: Validated for single-object replacement. Library-inheritance cascade (recompiling a parent library and observing dependents pick up the new parent) is not yet addressed.

**Extensions**:
- LPC self-compiler at `/usr/LPC/`: full LPC parser, AST classes, and compiler implemented in LPC. Tier D, compiled at boot, currently unconsumed. Enables AST-level transformation passes during the compile path — useful for safety transforms (see §5), authorship-time validation, or code analysis applied as part of hot-reload rather than as a separate deploy step.
- Program dependency database (port candidate, `progdb`): a daemon tracking inherits and clones across the live image, enabling recompile cascades. When a parent library recompiles, dependents are recompiled (or queued for recompile) so the inheritance graph stays coherent. Generalizes single-object hot-reload to library hot-reload.

**Open**:
- Library-cascade behavior in the absence of progdb (does an existing clone of an old parent become stale, or does the host's late binding catch the new parent on next dispatch?).
- Concurrent in-flight calls during recompile (what's the substrate's guarantee about a method executing in the old version while the new version compiles?).

---

## 5. Sandboxed code load

New code compiles into the runtime under capability bounds set at load time.

**Foundation**:
- Host-runtime `compile_object` plus the access checks invoked during compilation (the driver validates that the caller's tier permits compiling a path in the target tier).
- Capability separation (§2) bounds what compiled code can call once running.

**Demonstration**: None at substrate level. A `POST /compile` route on an application server compiles unsandboxed; the bounds it carries are the inheriting application's tier-D bounds, not bounds derived from the load operation.

**Status**: Foundation-only. Mechanism present; bounded-load demonstration pending.

**Extensions**:
- LPC self-compiler at `/usr/LPC/` (as in §4): enables grammar-pass safety transforms — denylist host kfuns, refuse dangerous productions, rewrite decorated source into capability-bounded AST nodes — before handing the result to `compile_object`.
- Decoration-and-compile sandbox pattern (port candidate, the canonical Meriadoc reference in `eOS-DeepContext/nodes/References/Meriadoc (ChatTheatre, 2026).md`): decorated host-language source compiles into pure host AST via a grammar pass; AST is denylisted of dangerous kfuns at the dispatch boundary; result runs at host-native speed inside the host's atomic envelope. The five-axis containment claim (language / location / invocation / capability / atomic rollback) is grounded in the source.
- Property-graph substrate layer (must-build): the Meriadoc pattern requires a property system providing four capabilities:
  - Keyed property map per object.
  - Set-time hook firing an event on property write (so the compile-from-source step is triggered atomically with state change).
  - Data-object inheritance on read (ur-parent chain), distinct from host-language code inheritance.
  - Persistent property storage.
  
  Of these, only persistent storage (§3) is present in eOS-kernellib. The other three are absent. A tier-D domain (`/usr/Properties/` or equivalent) providing the missing three is a prerequisite for the Meriadoc port. The layer is **cross-primitive**: set-time hooks are an instance of §6 (asynchronous events that are atomic with state change); ur-parent inheritance is an instance of §8 (state introspection via the data graph). The property-graph substrate layer is named under §5 because Meriadoc is the most demanding consumer; it deepens §6 and §8 as well.

**Open**:
- Re-entrancy detection: does the runtime prevent a handler from modifying the property it is reacting to?
- Signal-loop detection: when a handler emits a signal that re-fires itself, is the loop terminated cleanly?
- Compilation non-reentrancy: when a property write triggers compilation, can that compilation itself trigger nested compilation through a meta-handler?

(All three are from the Meriadoc Reference's `Open verification questions` section and remain open for any deployment of the pattern.)

---

## 6. Asynchronous events

Event delivery is atomic with the state change that produced it.

**Foundation**:
- Host-runtime `call_out` kfun. Schedules a future invocation; the deferred call runs in its own atomic envelope after the scheduling call commits.
- Atomic function semantics (§1) bound each call_out's execution slice. A failed deferred call rolls back its own mutations, not the dispatching state change.
- Single-coherence-domain architecture: one execution slot at a time, no concurrent state mutation, deterministic ordering without cross-domain coordination.

**Demonstration**: Cross-domain route registration uses a deferred `call_out("registerRoutes", 0)` from a user-tier handler's `create()` to defer registration with another domain (e.g., `/usr/WWW/sys/router`) until after the System initd has finished iterating all domains. The deferred call runs after the registering domain's `create()` commits, demonstrating atomic-with-state-change dispatch.

**Status**: Partial. Mechanism + simple deferred-dispatch demonstration. Richer event surfaces (property-change events, module-load events, configuration-change events) are absent.

**Extensions**:
- Continuation classes at `/lib/{Chained,Delayed,Iterative,Dist}Continuation.c`: continuation-passing primitives compiled at boot, currently unconsumed. Enable longer-running chains of deferred operations without nested call_out scaffolding.
- Configuration-change events (port candidate, `configd`): subscriber pattern for runtime-configurable values, dispatching on configuration change.
- Module-load events (port candidate, `moduled`): per-module load tracking with `module_loaded` events, useful for staged bootstrap and domain-specific code load.
- Relationship managers (port candidate, `n_to_n` / `n_to_one`): bidirectional relationship state with atomic update semantics, complementing event delivery for graph-shaped data.

**Open**:
- Multi-subscriber event semantics (when multiple subscribers exist, what is the ordering guarantee?).
- Event-during-compile semantics (an event firing while the substrate is compiling new code — atomic envelope behavior?).

---

## 7. Multi-agent coherence

Multiple callers see a consistent view of state without user-land coordination.

**Foundation**:
- Single-coherence-domain architecture: one address space, one execution slot at a time, no concurrent state mutation. Cross-domain coordination is unnecessary because the substrate provides serializability natively.
- Atomicity (§1) prevents partial-state visibility: a reader observing an in-progress write sees either pre-state or post-state, never a torn intermediate.

**Demonstration**: None at substrate level beyond single-call traces. Multi-caller / multi-agent demonstrations are pending.

**Status**: Foundation-only. Emergent from §1 plus the single-coherence-domain architecture; multi-call demonstration pending.

**Extensions**: None specific. Multi-agent coherence is an architectural property; substrate primitives that strengthen it appear elsewhere (atomicity, asynchronous events).

**Open**:
- Concurrent HTTP requests against the same application state (two POST /compile requests to the same path; two GET /counter requests during a POST /increment) with serializable behavior visible at the HTTP responses.

---

## 8. State introspection

The state graph is queryable directly through runtime calls.

**Foundation**:
- Host-runtime introspection kfuns: `find_object`, `object_name`, `status`, `query_owners`, `query_users`, `get_dir`. Available to any tier that has the access bit; constrained per tier.
- Object registry at `/usr/System/sys/objectd.c`: a System-tier daemon maintaining a populated registry of object existence, called during compilation and destruction. The registry is populated; a structured query surface for application code is not currently exposed.

**Demonstration**: admin_console (the binary-port REPL on the kernel telnet port) provides interactive introspection: `compile <path>` returns the master object reference, `code <expression>` evaluates an arbitrary LPC expression against the live image, `status` returns the system-wide health table (server / swap / memory / objects / users / uptime / call_outs). Substrate smoke testing exercises this surface over telnet and asserts the expected responses.

**Status**: Partial. Foundation kfuns work; structured object-graph queries (per-owner enumeration, inheritance-chain walks, clone enumeration) require either ad-hoc LPC code or a daemon exposing them.

**Extensions**:
- Object-registry query API (port candidate, `objregd` plus a kernel-tier API library): per-owner linked lists of live objects with traversal primitives (`first_link`, `next_link`, `prev_link`). Useful for GC, auditing, and enumeration without grovelling through all owners.
- Interactive operator console (port candidate, richer than admin_console): command history, directory navigation, integrated compile / clone / destruct / access-grant operations, and resource-state queries.

**Open**:
- Structured inheritance-chain query: given an object, list its inherits transitively.
- Live-clone enumeration: given a master, list its live clones.

---

## Supporting surfaces

The following are not primitives; they are surfaces through which primitives manifest.

### Transport tier

- **HTTP/1 server library** at `/usr/HTTP/api/lib/Server1.c` + `/usr/HTTP/lib/Connection1.c` and supporting parsers. Application subclass pattern: inherit `Http1Server` and `/usr/System/lib/user`, override `receiveRequest`, and call inherited `expectEntity(length)` for body-bearing methods to opt into body receipt. Validated end-to-end via eos-harness HW-2 / HW-2b.
- **HTTP/1 TLS server and client** at `/usr/HTTP/api/lib/{TlsServer1, TlsClient1}.c` (extends `BufferedConnection1`). Compiled at boot; depends on the TLS 1.3 stack (gated on the host being built with `KF_SECURE_RANDOM`). No application subclass currently exists.
- **WebSocket framing** in `/usr/HTTP/lib/Connection1.c`: `expectWsFrame`, `receiveWsFrame`, `receiveWsChunk`, `sendWsChunk`, frame masking. Same opt-in subclass-callback pattern as `expectEntity`. No HTTP-to-WebSocket upgrade application currently exists.
- **TLS 1.3 stack** at `/usr/TLS/`: record layer, handshake, all extensions. Entire `TLS/initd::create()` body is gated on `# ifdef KF_SECURE_RANDOM`. The HTTP TLS variants are the only declared consumers.
- **BufferedConnection1** at `/usr/HTTP/lib/BufferedConnection1.c`: consumed only by the TLS HTTP variants.
- **Datagram** (port candidate, kernel-tier `/kernel/obj/datagram.c`): UDP-style binary datagram support, complementing the binary connection handler for connection-oriented ports.

### Bootstrap infrastructure

- **`find_or_load`** (port candidate): safe compile-or-locate pattern, returning an existing master if present and compiling otherwise, with auto-init via `call_other(obj, "???")`.
- **`sequencer`** (port candidate): staged daemon-init via `call_out` chaining, when bootstrap requires N daemons started in a specific order without busy-wait.
- **`clonable` + `sys_auto`** (port candidate): per-master clone-list maintenance, clone-count tracking, and unified accessors layered on top of the kernel auto pattern.

---

## Appendix: Tier model

This document uses a five-tier vocabulary (A/B/C/D/E) that refines the three-tier vocabulary in `doc/ARCHITECTURE.md` (Kernel / System / User). The five-tier splits the C-language host driver from the LPC kernel (A vs B) and the shipped substrate domains from the application-supplied domains (D vs E). Both vocabularies describe the same boundaries; the five-tier provides more resolution where boundary discrimination matters in the per-primitive analysis below.

The substrate organizes code into five tiers. Tier identity is determined by the path under which the source compiles, and is enforced by the host driver's access checks at every kfun call.

| Tier | Location | Role |
|---|---|---|
| **A** | C code (host driver) | Parses, executes, manages atomicity / persistence / swap / call_outs. Not LPC; not part of eOS-kernellib's source. |
| **B** | `/kernel/` | Hard-trusted kernel-tier LPC. The driver hooks (`driver.c`, the user / access / resource daemons, the kernel auto, kernel API libraries). Hand-edited rarely. |
| **C** | `/usr/System/` | Privileged user-tier code, owner `System`, with `set_global_access("System", TRUE)`. The System auto (`/usr/System/lib/auto.c`) is the inheritance root for everything in tiers D and E. |
| **D** | `/usr/HTTP/`, `/usr/TLS/`, `/usr/LPC/`, etc. | Shipped substrate domains. Each domain has its own owner and is isolated from other user-tier code unless explicit cross-domain access is granted. These domains are shipped together with eOS-kernellib as the substrate's distribution. |
| **E** | Application-supplied `/usr/<Domain>/` | Same user-tier mechanics as tier D — owner, access bits, inheritance chains — but distributed by the consuming application, not by eOS-kernellib. |

Two implications matter for builders authoring on top of eOS-kernellib:

1. **What "is" eOS-kernellib** is tiers B + C + D. Tier A is the driver; tier E is the consumer's responsibility.
2. **Tier D items are technically the same kind of thing as tier E items.** A user-tier domain is a user-tier domain. The boundary between D and E is packaging convention. A primitive that currently exists as a tier-E pattern in some consumer's distribution can be promoted to tier D in eOS-kernellib by adding the domain to the shipped substrate set.
