# Coming from contemporary infrastructure

A translation guide for developers arriving from the contemporary service stack: an application language (Python, TypeScript, Go) plus managed services (a database, a cache, a queue, a deploy pipeline, an identity provider). eOS-kernellib provides most of what that stack assembles, but as runtime properties rather than external services. This document maps each familiar component to the platform mechanism that replaces it, and lists the habits to unlearn.

**Audience**: a developer evaluating or starting on the platform whose instincts were trained on cloud-service architectures. This reader has skimmed [persistence.md](persistence.md) Why orthogonal persistence or is willing to take the persistence model on credit for the next ten minutes.

## The translation table

| You reach for | The platform provides | Where it's covered |
|---|---|---|
| Database / cache | The persistent image: objects are durable by default, and the "cache" and the "database" are the same in-memory state | [persistence.md](persistence.md) |
| ORM / serialization layer | Nothing: there is no second representation to map to | [persistence.md](persistence.md) Compared with common alternatives |
| Queue / webhook / pub-sub | Property-change observers firing inside the same atomic envelope as the write | [dispatcher.md](dispatcher.md), [signal-applications.md](signal-applications.md) |
| Deploy pipeline | Hot reload: `compile_object` replaces a master's program in the running system | [code-lifecycle.md](code-lifecycle.md) Hot reload |
| Migration framework | `call_touch` lazy upgrade: restored or live objects migrate to recompiled programs on next touch | [code-lifecycle.md](code-lifecycle.md) Touch |
| IAM / service roles | Capability tiers: code runs under a tier that bounds what it can call, enforced at the kfun boundary | [architecture.md](architecture.md) Capability tiers |
| Transactions (database-side) | Atomic functions over program state itself: commit-or-rollback covers the same objects your code computes with | [lpc-essentials.md](lpc-essentials.md) Atomicity |
| Backup / disaster recovery | The statedump cycle: a typed, versioned snapshot of the whole image | [persistence.md](persistence.md) The statedump cycle |
| Process supervisor / restart policy | Snapshot restore and hot boot: pending deferred work survives the restart with the state it belongs to | [persistence.md](persistence.md) Hot boot |

## What each translation actually means

**The database and the cache collapse into the image.** State lives in objects. Objects are durable because the runtime snapshots the whole image. There is no read path that hydrates from storage and no write path that flushes to it: reads and writes are variable access. The mental model shift: stop asking "where is this stored?" and start asking "which object owns this state?" Consistency between "the cache" and "the store" is not a problem you manage. There is one copy.

**Events are not a queue.** On the service stack, reacting to a state change means publishing to a queue and processing it later, with delivery semantics (at-least-once? exactly-once?) as a contract you defend in application code. Here, observers registered on a property fire *inside the same atomic envelope as the write*: when the write commits, the reactions have already run. When the write rolls back, the reactions un-happened with it. There is no delivery window, no retry policy, no dead-letter queue, and also no cross-machine fan-out. This is a single-coherence-domain runtime by design. Work that should *not* share the write's envelope is scheduled explicitly with `call_out` or a `$delay()` continuation, which is the platform's version of "enqueue for later."

**Deployment is a function call.** `compile_object` against a path with an existing master replaces the program in the running system. The next call dispatches to the new code, and state survives the transition because state was never inside the code. The pipeline stages the service stack builds (build, ship artifact, roll instances, warm caches, replay traffic) exist because code and state live in separate places there. Here they don't. What remains of "deployment" is judgment about the change itself: recompiling a library does not automatically upgrade its existing children, and `call_touch` is the migration tool ([code-lifecycle.md](code-lifecycle.md)).

**Identity is structural, not a service.** There is no token validation on each request against an external provider. Code runs under an owner within a capability tier. What it can call is bounded by the tier model and the access daemon's grants, enforced at the kfun boundary on every call. The closest service-stack analogy is IAM roles, except the "role" is where the code lives, the enforcement is in the runtime, and there is no policy document to drift out of sync with the code.

**Schema migration becomes code migration.** With no storage schema there is no schema migration. What evolves is the program: recompile, and existing objects (including objects restored from a snapshot written months ago) migrate lazily via `call_touch` as they are next touched. The discipline migrates too: instead of writing `ALTER TABLE` scripts, you write upgrade-aware code paths in the new program version.

## The unlearning list

Habits that produce redundant or wrong code on this platform:

- **No serialization API.** Writing `to_json`/`from_json` pairs for durability re-implements what the image already does. (Serialization at the *transport* boundary, talking to external clients, is real and lives at the edge, e.g. [xml.md](xml.md) and the HTTP surface.)
- **No message queue.** Registering an observer is the reaction mechanism. `call_out` is the deferral mechanism. Building a polling loop over a "pending work" table re-implements `call_out` badly.
- **No deploy step.** If your change process ends with "restart the server," you have re-imported the habit the platform removed. Restarts are for host-binary upgrades (hot boot), not code changes.
- **No external identity service.** Capability checks are structural. An "auth middleware" layer guarding internal calls duplicates what the tier boundary already enforces. Put authorization logic at the transport edge where external principals enter, not between internal objects.
- **No migration framework.** There is no schema to version. Version the *code's* handling of old state shapes, and lean on `call_touch`.

## What does not translate

In the interest of honesty about the other direction, here are the needs the service stack serves that this platform deliberately does not meet:

- **Horizontal scale-out and multi-machine redundancy.** Single coherence domain, one process. If the workload needs concurrent writers across machines, this is the wrong platform. The single-coherence-domain commitment is named in [runtime-primitives.md](runtime-primitives.md) §7.
- **Polyglot services.** The runtime's guarantees hold for LPC (and Merry) code inside the image. External services integrate at the transport boundary like any other client -- and for the outbound direction (your code calling them), read `application-authoring.md` Outbound connections first: the client surface ships but is unproven today.
- **Declarative cross-entity queries.** There is no query planner over the image. Enumeration and indexing are application structures (see the Index and Vault subsystems for what ships).

## Where to next

- **[persistence.md](persistence.md)**: the model underneath everything in this table: why orthogonal persistence, what persists, the statedump cycle, the boundaries, and Getting data out, for the exit-cost question.
- **[getting-started.md](getting-started.md)**: boot the platform and see the properties firsthand.
- **[lpc-essentials.md](lpc-essentials.md)**: the language bridge, including the "if you come from dynamic languages" notes.
- **[debugging-applications.md](debugging-applications.md)** The working environment, plainly: what a team's day-to-day looks like -- the editor reality (C mode, no LSP), why git stays the source of truth structurally, and CI as the regression harness run headless.
- **[runtime-primitives.md](runtime-primitives.md)**: the per-primitive foundation-and-evidence statement for the eight runtime guarantees.
