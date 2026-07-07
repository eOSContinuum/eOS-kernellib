# Runtime primitives

eOS-kernellib's runtime platform exposes eight primitives the application above consumes directly: atomicity, capability separation, persistent state, hot reload, sandboxed code load, asynchronous events, multi-agent coherence, and state introspection. This document is the per-primitive foundation-and-proof statement: for each primitive, the platform mechanism behind it, the demonstration it works in practice, the current status, the supporting extensions, and the open work.

The architectural commitment behind this list — why these eight are surfaced as runtime primitives rather than left for applications to rebuild — is that each is a runtime guarantee an orthogonally-persistent server cannot fake at the application layer. Atomicity requires runtime cooperation with the transaction manager; persistence requires runtime cooperation with the storage manager; capability separation requires runtime cooperation with the access checks; hot reload requires runtime cooperation with the dispatcher. The remaining four (sandboxed code load, asynchronous events, multi-agent coherence, state introspection) layer on top of those four. Asking the application to provide them is asking it to reproduce the runtime in user space. The platform's stance is that these properties are the platform's responsibility; the sections below name the foundation, status, and pending proofs primitive by primitive.

**Audience**: a developer or architect deciding whether eOS-kernellib's runtime platform fits a use case, or auditing the platform's runtime guarantees against application requirements; wants the per-primitive foundation, demonstration status, supporting extensions, and open work for each of the eight primitives; assumes `docs/architecture.md` for the structural model and tier vocabulary.

**Section template.** Each primitive section below follows the same structure: a one-sentence claim opener, then **Foundation** (the platform mechanism that provides the property), **Demonstration** (evidence the property works in practice), **Status** (Validated / Partial / Foundation-only), **Extensions** (additional support shipped or proposed), and **Open** (unresolved questions). The bold prose-headers act as in-section anchors when reading or scanning.

**Status legend.** Each primitive's Status carries one of three values:

- **Validated** — foundation present in the platform, demonstrated empirically by a cited test or observation.
- **Partial** — foundation present, demonstration partial. Some surface of the primitive is observed; complete demonstration is not yet authored.
- **Foundation-only** — platform mechanism is present, but no empirical demonstration of the primitive exists yet.

**Tier vocabulary** used throughout this document is defined in the Appendix. Briefly: kernel-tier code lives in `/kernel/` (tier B); System-tier code in `/usr/System/` (tier C); shipped platform domains in `/usr/HTTP/`, `/usr/TLS/`, `/usr/LPC/`, etc. (tier D); application domains outside the kernel layer (tier E).

---

## 1. Atomicity

Operations commit wholly or roll back wholly. Partial effects do not escape on failure.

**Foundation**: DGD atomic-function semantics. A function declared `atomic` (or invoked through `call_limited` with an atomic envelope) that errors causes every state mutation performed inside it to roll back. The host runtime is the enforcement point; the application carries no roll-back code. The property is older than this repository — Christopher Allen's [2000 MUD-Dev description][allen-dgd-2000] names it: "atomic function calls allow full system-state rollback in the event of a run-time error."

**Demonstration**: a deliberate-failure probe in `examples/atomic-demo/`. The counter master declares `atomic void increment_with_failure()` whose body mutates `counter` and then `error()`s. The HTTP route catches the error and reports it in the response body; the next `GET /counter` returns the pre-call value, evidence of rollback. The `[atomic]` annotation in the boot log on the error trace is the runtime's own marker of the atomic envelope. The smoke script (`examples/atomic-demo/smoke.sh`) exercises the three-step probe and asserts the rollback.

**Status**: Validated. Foundation present; rollback demonstrated empirically by `examples/atomic-demo/`.

**Extensions**: None at the platform level. Atomicity is a host-runtime property; eOS-kernellib does not extend it beyond the host's contract.

**Open**:
- Compound-operation atomic envelopes spanning multiple sub-calls. The host envelope is per-call; multi-call transactional semantics are an application concern.
- Behavior under host-driver extensions that compile LPC bytecode to native code (see Appendix §Tier A: extensions). The atomic-commit guarantee is enforced by the host runtime's bytecode interpreter; whether an extension-loaded codepath that bypasses the interpreter preserves the rollback path is empirically unverified. Operators loading such an extension should treat the guarantee as conditional until measured.

---

## 2. Capability separation

Code runs under a capability tier that bounds what it can call.

**Foundation**:
- Host-runtime per-object owner identity and tier-aware kfun access checks.
- The kernel auto (`/kernel/lib/auto.c`) and System auto (`/usr/System/lib/auto.c`) compose the access-control surface every user-tier object inherits.
- The tier model (Appendix) packages the boundary: B and C carry privileged kfun authority; D and E operate under bounds, gaining cross-domain visibility only through System's `set_global_access` mechanism.
- The capability library (`/kernel/sys/capabilityd` plus the inheritable `/kernel/lib/capability`) consolidates the kernel layer's discretionary authority checks behind one membership store and one check API: of the six gating surfaces — dispatcher registrar approval, script-space registration, observer-property (`merry:on:*`) writes, the persistence dump-and-exit gate, the HTTP acceptor binding, and console verb elevation — five route through the store-backed `is_allowed` / `require_member` choke-point, and the dump-and-exit gate uses the library's fixed-principal `require()` (it consults no stored set and is not grantable). The model is tier and owner-identity mediation: capability-shaped but enforced by ambient authority, not strict no-ambient-authority object-capability. `docs/capability.md` is the full statement, including the limitations and the path toward stricter mediation.

**Demonstration**: HTTP/1 application server at the kernel-defined mount point `/usr/WWW/obj/server` (tier D) inherits `/usr/System/lib/user` (tier C) and `Http1Server` (tier D) — the pattern the reference application at `examples/http-app/` implements. The inheritance traverses System's global-access grant; the application receives binary-port connections only through System's `set_binary_manager` registration. The application cannot invoke that registration directly — the privilege check refuses callers outside the System tier, independent of application logic. The capability library's registrar gate is exercised by `examples/merry-app` (the REGISTRAR REJECT phase refuses a cross-domain registration; the DISPATCH GATE phase proves a direct `merry:on:*` write is gated identically across the `set_property` and `batched_set` paths).

**Status**: Partial. Tier model, ownership, inheritance, and the consolidated capability library (six surfaces, including the `merry:on:*` property-write gate) work; per-request authorization within an application's surface (e.g., HTTP-route-level principal challenge) is absent.

**Extensions**:
- HTTP authentication and authorization sys objects at `/usr/HTTP/sys/authenticate.c` and `/usr/HTTP/sys/authorize.c`. RFC-aligned challenge / authorization primitives; compiled at boot and consumed by the HTTP API library's header-field parsing (`RemoteFields` / `RemoteAuthentication`); no application exercises a challenge flow.
- Kernel-tier TLS API (port candidate): wraps the host's per-execution-context thread-local storage as a kernel-API library, useful for per-principal state visible only within an atomic execution slice.
- Unified clone management (port candidate): `clonable` library + System-tier auto extension providing per-master clone-list maintenance and clone-count tracking with capability-aware access.
- Runtime-configurable values daemon (port candidate, `configd`): typed configuration store (booleans, integers, strings, object references) with event subscription on configuration change, gated by tier-D ownership.

**Open**:
- Per-route HTTP authorization wiring (consume `authenticate`/`authorize` from an HTTP application).
- Stricter object-capability at the untrusted boundary — held-reference, attenuable, revocable authority for the Merry sandbox and observer surfaces, in place of ambient tier identity. Pure LPC reaches only a partial, bypassable form; a faithful version needs host-driver primitives the platform does not expose today. The capability library's `principal` argument is the seam it would attach to. See `docs/capability.md` (Toward stricter object-capability).

---

## 3. Persistent state

The in-memory object graph survives restart without explicit serialization.

**Foundation**:
- Host-runtime orthogonal persistence. The platform's statedump mechanism captures the entire image to disk; restore reconstructs it. Objects in the image survive restart without application-level serialize / deserialize code. Allen's [2000 description][allen-dgd-2000] names the property concisely: "DGD maintains persistence as a characteristic of its runtime environment ... full system state dump files implement persistence across reboots as well as snapshot-style state backups." Atkinson and Morrison's "Orthogonally Persistent Object Systems" (VLDB Journal 4, 1995) is the canonical academic statement of this architectural property.
- `save_object` / `restore_object` provide a complementary per-object snapshot mechanism for daemons that need an explicit save point independent of full image dumps.

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html

**Demonstration**: both mechanisms have working demonstrations. The per-object snapshot path: admin authentication credentials persist across restarts because the kernel user object writes the password hash to a `.pwd` file via `save_object` (`/kernel/obj/user.c`) and restores it on the next login — an explicit save point, independent of image dumps. The image-persistence path: the `examples/merry-app` smoke verifies a richer composition: a property-stored reference to a compiled Merry-script clone, the host's LPC global referencing that host, and a scheduled `call_out` against the host all survive a `dump_state` / `shutdown` / restart cycle, and the observer's compiled source fires correctly against the restored host (phases 16 and 17). `docs/dispatcher.md` Persistence and `docs/persistence.md` Substrate verification walk the composition.

**Status**: Validated. Image persistence demonstrated empirically by the merry-app dump / restart cycle and the dispatcher's snapshot+restore verification; the `save_object` path demonstrated by credential persistence across reboots.

**Extensions**:
- `KVstore` at `/lib/KVstore.c` and `KVnode` at `/obj/kvnode.c`: persistent keyed-tree structure built on the persistence primitive. Compiled at boot; provides a structured collection suitable as backing storage for higher-level data primitives.
- **Property-change dispatcher** (`/usr/Merry/sys/merry` + `/lib/util/properties::set_property` hook): observer-state-bearing composition with DGD's orthogonal persistence. Observers stored as property-bound script references; the on-disk compile artifacts at `/usr/Merry/merry/<md5>.c` reload on demand after restore; pre/main/post observer firing resumes against restored hosts. See `docs/dispatcher.md`.
- Two-level mapping (port candidate, `bigmap` / `bigmap_iterator`): bypasses the host runtime's per-mapping size limit, with iterator pattern for subclass masking. Useful when a persistent collection grows past the host-language ceiling.

**Open**:
- Statedump scheduling and integrity policy (when does the platform write a full image dump? What happens on dump failure?). Operator-facing; outside the platform's enforcement surface.

---

## 4. Hot reload

Code recompiles into the live runtime; existing objects update in place.

**Foundation**: Host-runtime `compile_object` kfun. Passing a path that already has a master in memory replaces the master with the recompiled version. Existing references to the old master continue to function for the in-flight call; subsequent calls dispatch to the new version. No deploy step.

**Demonstration**: the `examples/hot-reload-demo/` reference application. A greeting master exposes a `greet()` method; an HTTP server routes `GET /greet` to the method and `POST /compile` to `compile_object` on the greeting's path. The smoke script (`examples/hot-reload-demo/smoke.sh`) exercises the three-step probe: cold-boot `GET /greet` returns the on-disk string; `POST /compile` with new LPC source returns a `200 OK` response whose body reports `Compiled /usr/WWW/greeting`; immediate `GET /greet` returns the new string. No DGD restart, no application-layer reload mechanism — `compile_object` is the platform mechanism, exercised through the HTTP application surface. Two headless regressions drive the same mechanism without an HTTP client, under `scripts/run-example.sh`: `examples/hot-reload-demo/sys/test.c` recompiles the greeting in-process and asserts the new program from the next dispatch (the recompiling thread's own in-flight call still finishes on the old program); `examples/hot-reload-master/` clones a master, advances per-clone state, recompiles the master, and confirms the existing clone runs the new program while keeping its dataspace — clone code follows the recompiled master, clone data survives. A third headless regression, `examples/upgrade-cascade/`, drives the library-inheritance cascade: it upgrades a parent library through the upgrade daemon and confirms the recompiled inheritor's existing clones report the new library's version and behavior, keep their per-clone counts, and each run their `patch()` hook exactly once through the `call_touch` gate.

**Status**: Validated for single-object replacement and for clone propagation with dataspace survival — a recompiled master's existing clones run the new program and keep their per-clone state (the `hot-reload-master` regression). The library-inheritance cascade ships as a platform mechanism — the object manager (`/usr/System/sys/objectd.c`) records the inheritance and include graph at compile time, and the upgrade daemon (`/usr/System/sys/upgraded.c`) walks dependents, recompiles them (optionally atomically), and optionally `call_touch`-patches live clones, driven by the operator `upgrade` command or programmatically through the System auto library's `upgrade()` wrapper — validated end-to-end by the `upgrade-cascade` regression. Concurrent-in-flight behavior (the guarantee that calls already dispatched against the old master finish on the old program) is asserted by the host runtime but not exercised by the sequential regressions; a concurrent-request probe would close that half.

**Extensions**:
- LPC self-compiler at `/usr/LPC/`: full LPC parser, AST classes, and compiler implemented in LPC. Tier D, compiled at boot, currently unconsumed. Enables AST-level transformation passes during the compile path — useful for safety transforms (see §5), authorship-time validation, or code analysis applied as part of hot-reload rather than as a separate deploy step.
- **Library upgrade cascade**, as shipped: the object manager registers inheritance and include dependencies during compilation; the upgrade daemon recompiles dependents and, when a patch tool is supplied, queues `call_touch` patching so existing clone state migrates on next reference, without a maintenance window. One piece is not implemented: a stored per-master clone list — clone patching sweeps the object table rather than enumerating a registry.

**Open**:
- Behavior of a clone live during the recompile window (between a parent issue's destruct and the dependent's recompile completing) is unverified; the upgrade daemon's `call_touch` patching covers the post-upgrade path.
- Concurrent in-flight calls during recompile (what's the platform's guarantee about a method executing in the old version while the new version compiles?).
- Interaction with host-driver extensions that maintain a compiled-code cache (see Appendix §Tier A: extensions). When `compile_object` recompiles a path, an extension's per-program code cache must invalidate or re-key the corresponding entry, otherwise stale compiled code shadows the new bytecode and hot reload silently fails. Empirically unverified for any specific extension; operators loading such an extension should test the recompile path against their workload before relying on hot reload in production.

---

## 5. Sandboxed code load

New code compiles into the runtime under capability bounds set at load time.

**Foundation**:
- Host-runtime `compile_object` plus the access checks invoked during compilation (the driver validates that the caller's tier permits compiling a path in the target tier).
- Capability separation (§2) bounds what compiled code can call once running.
- The Merry subsystem (`src/usr/Merry/`): a restricted sublanguage of LPC whose grammar pass translates source to plain LPC and compiles it into a clonable whose inherit chain installs the sandbox (`docs/merry-language.md`).

**Demonstration**: the Merry sandbox is the decoration-and-compile pattern, shipped. Merry source parses through the yacc-derived grammar, translates to LPC, and compiles into an object that cannot reach the kfuns the sandbox forbids: a 51-entry deny list shadows each forbidden kfun locally, the `call_other` filter refuses direct cross-object calls, and `new_object` is denied outright. `examples/merry-app` asserts the sandbox firing (a script calling `clone_object` raises "not allowed in merry code"); `examples/chat-app` phases 3 and 4 demonstrate both halves — in-sandbox observers run against live state, while a sibling observer calling `write_file` compiles (the shadow exists, so compilation resolves) and is refused at first invocation, with no file created.

Five-axis containment, as shipped: **language** (the grammar refuses `->`, `rlimits`, and the other restricted constructs — `docs/merry-language.md` What is restricted), **location** (scripts live as property-bound values and run only where `run_merry` or the dispatcher routes them), **invocation** (the caller's tier bounds what the binding host can do), **capability** (the kfun deny list enforced by local shadows), and **atomic rollback** (an error inside a script reverts the dataspace mutations of its atomic envelope).

**Status**: Partial. Sandboxed load validated for Merry-compiled code by the cited example phases. Raw `compile_object` of plain LPC still loads at the inheriting object's tier bounds — a `POST /compile` route on an application server compiles unsandboxed; bounded loading of arbitrary plain LPC remains open. The pattern's reference ancestor is the Meriadoc subsystem of ChatTheatre's SkotOS deployment ([references.md](references.md#skotos)); the working implementation lives in this repository.

**Extensions**:
- LPC self-compiler at `/usr/LPC/` (as in §4): enables grammar-pass safety transforms over plain LPC — denylist host kfuns, refuse dangerous productions — before handing the result to `compile_object`; the candidate path for extending load-time bounds beyond Merry source.
- Property-graph support, as shipped: the keyed property map (`/lib/util/properties`), set-time observer dispatch atomic with the write (`docs/dispatcher.md`), ur-parent ancestry walks for script and observer resolution (`/lib/util/ur`; `find_merry` and the dispatcher's ancestry walk), and persistent property storage (§3). The subsystem is **cross-primitive**: set-time dispatch is an instance of §6 (events atomic with state change); the ur chain serves §8 (state introspection via the data graph). One piece is not yet implemented: generalized data-inheritance on plain property reads (a `query_property` that consults the ur chain). Ancestry applies to script lookup and observer resolution today; extending it to ordinary reads — inherited property defaults — is a committed roadmap surface (`docs/runtime-platform-roadmap.md`, Wave 2), activating with the first consumer that needs inherited defaults.

**Open**:
- Re-entrancy and signal loops are bounded on the dispatcher path: the cycle detector refuses a recursive dispatch on the same (object, keypath) chain, and cascade depth is bounded and operator-tunable (demonstrated by the merry-app DISPATCH CYCLE phase; `docs/dispatcher.md`). Whether the bounds suffice under adversarial observer graphs — deep cross-object cascades that stay below the depth limit — has no dedicated probe.
- Compilation non-reentrancy: scripts compile at registration time, not on property write. A meta-handler path, where a property write triggers compilation that itself triggers nested compilation, does not exist in the shipped surface and remains unverified for any deployment that builds one.

---

## 6. Asynchronous events

Event delivery is atomic with the state change that produced it.

**Foundation**:
- Host-runtime `call_out` kfun. Schedules a future invocation; the deferred call runs in its own atomic envelope after the scheduling call commits.
- Atomic function semantics (§1) bound each call_out's execution slice. A failed deferred call rolls back its own mutations, not the dispatching state change.
- Single-coherence-domain architecture: one execution slot at a time, no concurrent state mutation, deterministic ordering without cross-domain coordination.

**Demonstration**: Cross-domain route registration uses a deferred `call_out("registerRoutes", 0)` from a user-tier handler's `create()` to defer registration with another domain (e.g., `/usr/WWW/sys/router`) until after the System initd has finished iterating all domains. The deferred call runs after the registering domain's `create()` commits, demonstrating atomic-with-state-change dispatch. The property-change dispatcher (`docs/dispatcher.md`) adds a second demonstration at the property-state-change boundary: every `set_property` write fans out to registered pre/main/post observers within the same atomic envelope as the write itself, with cascade-depth bounding, cycle detection, and explicit batching. Observers are compiled Merry scripts dispatched via the script-binding mechanism (`docs/merry-applications.md`). The `examples/merry-app` smoke exercises pre-veto, main-cascade, post-audit, and cycle-detection paths against the host state. The `examples/chat-app` driver adds application-scale evidence: a post-timing observer writes a second user's mention tracker within the same `set_property` envelope that carried the message (phase 5), and a deliberate failure inside an `atomic` wrapper shows the notification unhappening together with the state change it rode on (phase 7) — the event is atomic with the change, in both directions.

**Status**: Partial. Mechanism + deferred-dispatch demonstration + the property-change event surface via the Merry dispatcher. Module-load events and configuration-change events remain absent.

**Extensions**:
- **Property-change dispatcher** (`/usr/Merry/sys/merry`): per-property-write observer fan-out at pre/main/post timings, with cascade-depth bounds, cycle detection, batching surface, and Merry-source observers. See `docs/dispatcher.md`.
- Continuation classes at `/lib/{Chained,Delayed,Iterative,Dist}Continuation.c`: continuation-passing primitives compiled at boot, currently unconsumed. Enable longer-running chains of deferred operations without nested call_out scaffolding.
- Configuration-change events (port candidate, `configd`): subscriber pattern for runtime-configurable values, dispatching on configuration change.
- Module-load events (port candidate, `moduled`): per-module load tracking with `module_loaded` events, useful for staged bootstrap and domain-specific code load.
- Relationship managers (port candidate, `n_to_n` / `n_to_one`): bidirectional relationship state with atomic update semantics, complementing event delivery for graph-shaped data.

**Open**:
- Multi-subscriber event semantics (when multiple subscribers exist, what is the ordering guarantee?).
- Event-during-compile semantics (an event firing while the platform is compiling new code — atomic envelope behavior?).

---

## 7. Multi-agent coherence

Multiple callers see a consistent view of state without user-land coordination.

**Foundation**:
- Single-coherence-domain architecture: one address space, one execution slot at a time, no concurrent state mutation. Cross-domain coordination is unnecessary because the platform provides serializability natively.
- Atomicity (§1) prevents partial-state visibility: a reader observing an in-progress write sees either pre-state or post-state, never a torn intermediate.

**Demonstration**: the `examples/chat-app` coherence phases exercise multi-caller semantics directly. Two users contending for a capacity-1 room slot serialize without a lock — each claim re-reads current membership at write time in its own atomic task, so exactly one wins (phase 9); the same contention run against a stale pre-read snapshot reproduces the lost-update the coherent read removes (phase 9b); three readers of one message log observe identical content because the log is one runtime property rather than per-reader copies, and a cached copy demonstrably diverges (phase 9c); a cross-room write through the batch surface commits or rolls back both sides as one unit, with the non-atomic contrast leaving the partial state the envelope removes (phase 9d).

**Status**: Partial. Serialization, lost-update contrast, read coherence, and cross-object atomic batching demonstrated at the LPC call surface by the cited phases; the same behavior observed through concurrent transport-level requests is pending.

**Extensions**:
- **Property-change dispatcher** (`docs/dispatcher.md`): bounded ordered fan-out of observer chains within an atomic envelope, with explicit batch identity surface (`batch` / `batched_set`, atomic-mode opt-in, batch-status log). Provides the per-batch serializability surface that future multi-agent demonstrations rely on for ordering claims.

**Open**:
- Concurrent HTTP requests against the same application state (two POST /compile requests to the same path; two GET /counter requests during a POST /increment) with serializable behavior visible at the HTTP responses.

---

## 8. State introspection

The state graph is queryable directly through runtime calls.

**Foundation**:
- Host-runtime introspection kfuns — `find_object`, `object_name`, `status`, `users`, `get_dir` — wrapped per tier by the kernel auto; owner and user enumeration (`query_owners`, `query_users`) through the kernel resource and user APIs. Available to any tier that has the access bit; constrained per tier.
- Object manager at `/usr/System/sys/objectd.c`: a System-tier daemon registered with the host driver, called during compilation and destruction. It records object paths and issues plus the inheritance and include graph, and answers structured queries (`query_inherits`, `query_inherited`, `query_includes`, `query_included`, `query_path`, `query_issues`) — all gated to the System tier; application code cannot reach the query surface.

**Demonstration**: admin_console (the binary-port REPL on the kernel telnet port) provides interactive introspection: `compile <path>` returns the master object reference, `code <expression>` evaluates an arbitrary LPC expression against the live image with command history and `$N` value recall, `status` returns the system-wide health table (server / swap / memory / objects / users / uptime / call_outs), alongside directory navigation (`cd` / `pwd` / `ls`), object lifecycle (`clone` / `destruct`), access management (`access` / `grant` / `ungrant`), and resource queries (`quota` / `rsrc`). No automated test in this repository exercises the console surface; verification is interactive.

**Status**: Partial. Foundation kfuns work; the object manager answers inheritance and include queries at the System tier; per-owner enumeration and clone enumeration are absent, and no structured query surface reaches application tiers.

**Extensions**:
- Object-registry query API (port candidate, `objregd` plus a kernel-tier API library): per-owner linked lists of live objects with traversal primitives (`first_link`, `next_link`, `prev_link`). Useful for GC, auditing, and enumeration without grovelling through all owners.

**Open**:
- Application-tier access to the object graph: the object manager's query surface is System-gated; exposing structured queries to applications needs a deliberate API decision.
- Live-clone enumeration: given a master, list its live clones without an object-table sweep.

---

## Supporting surfaces

The following are not primitives; they are surfaces through which primitives manifest.

### Transport tier

- **HTTP/1 server library** at `/usr/HTTP/api/lib/Server1.c` + `/usr/HTTP/lib/Connection1.c` and supporting parsers. Application subclass pattern: inherit `Http1Server` and `/usr/System/lib/user`, override `receiveRequest`, and call inherited `expectEntity(length)` for body-bearing methods to opt into body receipt. The reference application at `examples/http-app/` exercises the pattern end-to-end (GET, POST with body, 404 fallback) against a running platform.
- **HTTP/1 TLS server and client** at `/usr/HTTP/api/lib/{TlsServer1, TlsClient1}.c` (extends `BufferedConnection1`). Compiled at boot; depends on the TLS 1.3 stack (gated on the host being built with `KF_SECURE_RANDOM`). No application subclass currently exists.
- **WebSocket framing** in `/usr/HTTP/lib/Connection1.c`: `expectWsFrame`, `receiveWsFrame`, `receiveWsChunk`, `sendWsChunk`, frame masking. Same opt-in subclass-callback pattern as `expectEntity`. No HTTP-to-WebSocket upgrade application currently exists.
- **TLS 1.3 stack** at `/usr/TLS/`: record layer, handshake, all extensions. Entire `TLS/initd::create()` body is gated on `# ifdef KF_SECURE_RANDOM`. The HTTP TLS variants are the only declared consumers.
- **BufferedConnection1** at `/usr/HTTP/lib/BufferedConnection1.c`: consumed only by the TLS HTTP variants.
- **Datagram** (shipped, compiled at boot but unconsumed; kernel-tier `/kernel/obj/datagram.c`): UDP-style binary datagram support, complementing the binary connection handler for connection-oriented ports. The kernel userd provides `set_datagram_manager`; no shipped application consumes it.

### Bootstrap infrastructure

- **`find_or_load`** (port candidate): safe compile-or-locate pattern, returning an existing master if present and compiling otherwise, with auto-init via `call_other(obj, "???")`.
- **`sequencer`** (port candidate): staged daemon-init via `call_out` chaining, when bootstrap requires N daemons started in a specific order without busy-wait.
- **`clonable` + `sys_auto`** (port candidate): per-master clone-list maintenance, clone-count tracking, and unified accessors layered on top of the kernel auto pattern.

---

## Appendix: tier vocabulary used in this document

This document uses a five-tier vocabulary (A/B/C/D/E) that refines the three-tier vocabulary defined in `docs/architecture.md` Capability tiers (Kernel / System / User). The five-tier splits the C host driver from the LPC kernel (A vs B) and shipped platform domains from application-supplied domains (D vs E). Both vocabularies describe the same boundaries; the five-tier provides more resolution where boundary discrimination matters in the per-primitive analysis above. The canonical table lives in `docs/architecture.md`; this document does not duplicate it.

Two implications worth restating at this scope:

1. **What "is" eOS-kernellib** is tiers B + C + D. Tier A is the host driver; tier E is the consumer's responsibility.
2. **Tier D and tier E are mechanically identical.** The boundary is packaging convention, not enforcement. A primitive that currently exists as a tier-E pattern in a consumer's distribution can be promoted to tier D in eOS-kernellib by adding the domain to the shipped platform set.

### Extensions and the platform's contract

The structural model of the host-driver extension surface — dlopen-loaded modules registered in the `.dgd` configuration's `modules =` mapping, the 256-kfun cap that drives extension minimalism, the statedump-binding constraint — is documented in `docs/architecture.md` Host-driver extensions, with deployment-time mechanics in `docs/operations.md` Loading host-driver extensions. The platform-level point relevant to this document is narrower:

**eOS-kernellib's runtime platform requires no extension and loads none.** Every primitive above is foundation-and-status-stated against an extension-free deployment. The two Open entries on §1 Atomicity ("Behavior under host-driver extensions that compile LPC bytecode to native code") and §4 Hot reload ("Interaction with host-driver extensions that maintain a compiled-code cache") name what happens when a deployment chooses to load an extension whose codepaths interact with those primitives. In both cases the platform's contract becomes empirically unverified, not violated; operators loading such an extension should measure against their workload before relying on the corresponding primitive in production.

## Where to next

- [`docs/architecture.md`](architecture.md) — the platform's structural mechanics (tier model, daemons, boot sequence, auto-inheritance, host-driver extension surface) that the primitives above rest on.
- [`docs/persistence.md`](persistence.md) — the full orthogonal-persistence story behind §3 (statedump cycle, hot boot, save_object semantics, persistence boundaries).
- [`docs/code-lifecycle.md`](code-lifecycle.md) — the full compile / clone / destruct / touch story behind §4 and §5 (object-manager events, library upgrade cascade, `_F_touch` hook).
- [`docs/operations.md`](operations.md) Open empirical questions — the deployment-time interpretation of the Open entries on §1 and §4.
- [`docs/application-authoring.md`](application-authoring.md) — how a tier-E application consumes the primitives.
