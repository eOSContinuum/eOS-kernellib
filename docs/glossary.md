# Glossary

Definitions for terms used inline across the eOS-kernellib doc set. Each entry names where the term is load-bearing so a reader landing on a term mid-document can find the doc that explains it in depth.

**Audience**: a reader cross-referencing a term used inline in one of the other docs.

## atomic / atomic operation / atomic context

A function or call envelope whose state mutations either all commit together (on normal return) or all roll back together (on error). DGD's host runtime enforces atomicity at the function-call boundary; an `atomic`-modifier function or a `call_limited` envelope establishes an atomic context. Load-bearing in [runtime-primitives.md](runtime-primitives.md) §1 (atomicity primitive) and [lpc-essentials.md](lpc-essentials.md) Type modifiers.

## auto-inheritance

The implicit `inherit` relationship every compiled LPC source carries against the auto object named by the `.dgd` config's `auto_object` line (typically `/kernel/lib/auto`). The compile-time effect is that every object's master inherits the auto's variables, functions, and access checks without an explicit `inherit` declaration. Load-bearing in [architecture.md](architecture.md) Auto-inheritance pattern.

## call_out

The DGD kfun for scheduling a delayed callback: `call_out(string fn, mixed delay, mixed... args)`. Returns an integer handle the caller can pass to `remove_call_out` for cancellation. The callback fires in its own atomic context after the delay elapses; it does NOT extend the caller's atomic context. Load-bearing in [lpc-essentials.md](lpc-essentials.md) call_out and [runtime-primitives.md](runtime-primitives.md) §6 (asynchronous events).

## call_touch

The kfun that triggers an `_F_touch` upgrade dispatch on an object whose master was recompiled while the object was idle. The lazy-upgrade pattern: the object stays on its old program until something calls `call_touch` (or it receives a function call), at which point the kernel's touch machinery migrates it to the new program. Load-bearing in [code-lifecycle.md](code-lifecycle.md) Touch via call_touch.

## capability tier

One of the five layered access levels (A, B, C, D, E) that bound what loaded code can call. Tier A is the host driver and extensions; tier B is the kernel layer; tier C is system daemons; tier D is privileged user domains; tier E is application code. The `previous_program()` call-chain mechanism is the runtime enforcement point. Load-bearing in [architecture.md](architecture.md) Capability tiers and [runtime-primitives.md](runtime-primitives.md) §2 (capability separation primitive).

## capability library

The kernel layer's consolidated authority mechanism: a store daemon (`/kernel/sys/capabilityd`) holding namespaced approved-sets, plus an inheritable check face (`/kernel/lib/capability`), behind which six gating surfaces share one `is_allowed` / `require_member` choke-point. The platform's capability model is tier and owner-identity mediation — capability-*shaped* (privileged operations reachable only through mediating `/kernel/` objects) but enforced by **ambient authority** (the caller's tier, owning domain, and `previous_program()` chain), not strict no-ambient-authority object-capability, which LPC cannot reach without host-driver changes. The term "capability" across the doc set carries this meaning unless explicitly qualified. Load-bearing in [capability.md](capability.md) and [runtime-primitives.md](runtime-primitives.md) §2 (capability separation primitive).

## clone

An instance of a compiled program created via `clone_object(master)`. The clone shares the master's program but has its own dataspace (its own copy of the variables). Clones live under `/obj/` by convention and carry the same owner as the master. Load-bearing in [code-lifecycle.md](code-lifecycle.md) Clone.

## compile_object

The DGD kfun that compiles an LPC source file and registers the result as a program in the runtime: `compile_object(string path)` returns the master object. Calling it against a path that already has a master replaces the program in place — this is the hot-reload mechanism. Load-bearing in [code-lifecycle.md](code-lifecycle.md) Compile and [runtime-primitives.md](runtime-primitives.md) §4 (hot reload primitive).

## dataspace

The set of variables owned by a specific object instance (master or clone or LWO). Each object has its own dataspace; functions called in that object's context read and write the object's dataspace. Atomic rollback restores the dataspace to its pre-call values on error. Load-bearing in [persistence.md](persistence.md) What persists.

## dump_file

The `.dgd` config field naming the path the host runtime writes snapshots to and reads them back from on statedump-restore boot. Load-bearing in [operations.md](operations.md) .dgd configuration.

## dump_state

The DGD kfun (`dump_state(int incremental)`) that writes a snapshot of the in-memory object graph to the configured `dump_file`. Statedump occurs between timeslices, never inside an atomic operation. Load-bearing in [operations.md](operations.md) State persistence and [persistence.md](persistence.md) The statedump cycle.

## errord

The system daemon at `/usr/System/sys/errord` registered via `set_error_manager()`. Receives `runtime_error`, `atomic_error`, and `compile_error` events from the driver and routes them to log files or operator surfaces. Load-bearing in [operations.md](operations.md) Logging and diagnostics.

## _F_create

The compile-time-generated wrapper around an LPC source's `create()` function. The kernel layer's auto-inheritance generates `_F_create` to dispatch the right `create(...)` call depending on whether the object is a master or a clone. Load-bearing in [code-lifecycle.md](code-lifecycle.md) _F_create dispatch.

## hot boot / hotboot

Replacing the running DGD executable in place via `execv` (e.g., to pick up a new DGD binary or change a config value) while keeping the persistent connections open. The host runtime serializes per-connection state, the OS keeps the file descriptors across `execv` via POSIX fd inheritance, and the receiving binary reloads the snapshot. The kernel driver's `restored(int hotboot)` hook distinguishes hotboot-resume from statedump-resume. Load-bearing in [architecture.md](architecture.md) Boot sequence (Hot boot) and [persistence.md](persistence.md) Hot boot.

## initd

A domain's initialization daemon. The System initd at `/usr/System/initd.c` orchestrates cold boot. Each user-tier domain has its own `/usr/<Domain>/initd.c` that the System initd compiles during the domain-discovery phase of boot. Load-bearing in [architecture.md](architecture.md) Boot sequence and [application-authoring.md](application-authoring.md) Domain initd patterns.

## LWO (lightweight object)

An object created via `new_object` rather than `clone_object`, living inside a holder's dataspace rather than in the object table. Cannot be destructed; collected when the last reference is dropped. References alias within one dataspace; a reference exported to another object's dataspace becomes that dataspace's own copy, so cross-object handoff behaves as pass-by-value. Consolidated treatment in [code-lifecycle.md](code-lifecycle.md) LWO instantiation.

## master

The compiled program object at a given path. A master is created by `compile_object(path)` and serves as the template clones are made from. The master's master/clone distinction governs how `_F_create` dispatches to `create(...)`. Load-bearing in [code-lifecycle.md](code-lifecycle.md) and throughout the doc set.

## mount point

A registered application-server object at a path the kernel-layer's HTTP machinery looks up via `status(O_INDEX)`. The convention is `/usr/WWW/obj/server` for the HTTP/1 mount point; the server object receives parsed requests and returns responses. Load-bearing in [http-applications.md](http-applications.md) Application server mount.

## objectd

The system daemon that intercepts twelve compile-and-load lifecycle events (compiling, compile, compile_failed, clone, destruct, remove_program, etc.). Tier-D and tier-E domains MAY register an objectd-side observer for their own programs without modifying the System daemon. Load-bearing in [code-lifecycle.md](code-lifecycle.md) and [architecture.md](architecture.md) Tier-C daemons.

## observer

A compiled Merry script registered at a `(path, timing)` slot on a property-bearing host, fired by the dispatcher when that property is written (pre / main / post timings). Registrations live in the host's own `merry:on:<path>:<timing>` property as an ordered list -- registration order is firing order; the Merry daemon holds the gates, the lookup walk, and a cache, never the registrations themselves. Load-bearing in [dispatcher.md](dispatcher.md) (firing semantics) and [observers.md](observers.md) (the storage-and-lifecycle contract).

## orthogonal persistence

The architectural property that an object's lifetime is decoupled from the lifetime of the program that created it. The same code operates on transient values and persistent values; the persistence machinery is the runtime's concern, not the application's. Atkinson and Morrison's 1995 paper is the canonical academic statement; the KeyKOS and EROS literature extends the model with capability-based access control. See [references.md](references.md) for citation details. Load-bearing in [persistence.md](persistence.md) Orthogonal persistence and [runtime-primitives.md](runtime-primitives.md) §3 (persistent state primitive).

## principal

The capability-bearing identity under which code runs. Principals are tier-bound (a tier-E principal can only call tier-E or tier-D APIs the access daemon grants it); the `previous_program()` chain is how a callee determines the caller's principal. In the capability library a principal is the opaque string key a grant is recorded under — a domain, a caller-program path, or an object name — supplied by the gating surface, never inferred by the store. Load-bearing in [architecture.md](architecture.md) Capability tiers and [capability.md](capability.md).

## restore_object

The DGD kfun that reads a single-object save file (written by `save_object`) and restores the object's variables. Distinct from the substrate-wide statedump-restore boot path. Load-bearing in [persistence.md](persistence.md) save_object / restore_object.

## rollback

The discarding of state mutations performed inside an atomic operation that errored. The host runtime restores the dataspaces of every object touched in the failed envelope to their pre-call values; the runtime guarantees atomic-or-nothing semantics. Load-bearing in [runtime-primitives.md](runtime-primitives.md) §1 (atomicity).

## save_object

The DGD kfun that writes an object's variables to a single-object save file. Variables declared `static` are excluded (in-memory only). Distinct from substrate-wide statedump. Load-bearing in [persistence.md](persistence.md) save_object / restore_object and [lpc-essentials.md](lpc-essentials.md) Type modifiers.

## snapshot

A statedump file (the term and `statedump` are used interchangeably across the doc set). See `statedump`.

## statedump

The on-disk image of the entire in-memory object graph written by `dump_state` to the `dump_file` path. Statedumps occur between timeslices, never inside an atomic operation; the runtime guarantees that the snapshot represents a consistent state-graph commit boundary. Load-bearing in [persistence.md](persistence.md) The statedump cycle.

## tier-A / tier-B / tier-C / tier-D / tier-E

The five capability layers. Tier A: host driver and dlopen-loaded extensions. Tier B: kernel layer (the inheritable code under `src/kernel/`). Tier C: System daemons. Tier D: privileged user domains (e.g., the bundled `Test` domain). Tier E: application code. See `capability tier`. Load-bearing in [architecture.md](architecture.md) Capability tiers.

## timeslice

The execution-time unit DGD's scheduler uses between atomic operations. Statedumps fire between timeslices (never inside an atomic operation); long-running atomic envelopes are charged ticks against the timeslice budget and may be terminated if they exceed `tick_max`. Load-bearing in [operations.md](operations.md) Resource limits.

## ur-parent / ur-child / ur-hierarchy

The data-inheritance cluster, distinct from LPC's code inheritance (`inherit`). An object's **ur-parent** is its data ancestor: a clone designates another object as its ur via `set_ur_object` (the Merry `Spawn` merryfun stamps the relationship at clone time), forming the **ur-hierarchy**. Lookups that honor the hierarchy — Merry script resolution (`find_merry`) and the dispatcher's observer-ancestry walk — search the object first and then walk `query_parent()` up the chain, so behavior bound at an ancestor serves every descendant (one observer on a base room reacts for the whole cohort; see the chat example's ancestry phase). Plain property reads do not walk the chain. The cluster lives in `/lib/util/ur`. Load-bearing in [merry-applications.md](merry-applications.md) the ancestry walk and [dispatcher.md](dispatcher.md).

## userd

The System daemon mediating incoming connections per binding. Each `.dgd`-declared port has a userd-attached connection manager; a new connection drives `select(str)` on the userd, which clones a per-connection user object. Load-bearing in [architecture.md](architecture.md) Tier-C daemons.

## wiztool

The interactive wizard-tier shell that ships with cloud-server's upstream. eOS-kernellib's analogous interactive surface is the admin_console (the term "harness-shell" is the eOS-DeepContext graph's vocabulary refresh; either name refers to the same kind of tier-C operator surface). Load-bearing in [admin-console.md](admin-console.md).

## Vocabulary bridges

Readers arrive from two adjacent vocabularies: the SkotOS/kernellib lineage this platform descends from, and the eOS-DeepContext companion graph that documents the platform's design commitments in its own terms. The bridges:

**From the SkotOS lineage**:

| Lineage term | Platform term | Note |
|---|---|---|
| Signal phases `pre` / `prime` / `post` | Dispatcher timings `pre` / `main` / `post` | `prime` corresponds to `main`; the veto/mutate/audit roles carry over ([dispatcher.md](dispatcher.md)). SkotOS also ran a fourth `desc` phase (description rendering) between prime and post -- presentation-tier, no dispatcher counterpart |
| `patch()` | `_F_touch()` | Same platform dispatch, different hook name ([code-lifecycle.md](code-lifecycle.md) Terminology note) |
| Wiztool | admin_console | See the `wiztool` entry above |
| Meriadoc / Merry (SkotOS subsystem) | The Merry subsystem (`src/usr/Merry/`) | The shipped implementation of the same decoration-and-compile pattern ([runtime-primitives.md](runtime-primitives.md) §5) |

**From the eOS-DeepContext graph**:

| Graph term | Platform term |
|---|---|
| substrate | runtime platform |
| harness-shell | admin_console |
| atomic envelope | atomic context (see `atomic` above) |
| persistence image | the in-memory image; statedump / snapshot capture it |
| code-as-state | hot reload semantics — code becomes live by state mutation, no deploy step ([code-lifecycle.md](code-lifecycle.md)) |
| data inheritance | the ur-parent chain (see `ur-parent` above) |

## Where to next

- [references.md](references.md) — citations for the orthogonal-persistence literature, DGD-list discussions, and upstream documentation referenced inline across the doc set.
- [architecture.md](architecture.md) — the structural reference for capability tiers, daemons, boot sequence, and host-driver extensions.
- [runtime-primitives.md](runtime-primitives.md) — the eight runtime primitives the platform provides, each with foundation, demonstration status, supporting extensions, and open work.
