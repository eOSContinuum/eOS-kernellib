# Application authoring

A tier-E application on eOS-kernellib lives in a single directory under `src/usr/`, declares its bootstrap in an `initd.c`, and inherits the platform's System auto to gain owner identity and cross-tier API access. The patterns below apply regardless of transport: domain layout, owner / creator / access conventions, the object-manager event lifecycle, code upgrade through `call_touch`, and non-HTTP transport bindings.

**Audience**: an LPC author writing a tier-E application on top of eOS-kernellib, comfortable with LPC syntax (or read `docs/lpc-essentials.md` first), with the platform running locally per `docs/getting-started.md`.

The architecture document (`docs/architecture.md`) covers the platform's tier model, daemons, and inheritance chain. The HTTP/1-specific application pattern is covered separately in `docs/http-applications.md` with a runnable reference at `examples/http-app/`.

## Domain layout

A tier-E application lives in a single directory under `src/usr/`. The directory's name is the application's owner identity. The System initd discovers the domain at boot via the `get_dir("/usr/[A-Z]*")` iteration and compiles the domain's `initd.c`.

A typical tier-E domain layout:

```text
src/usr/<App>/
    initd.c            — compiled at boot; bootstraps the rest of the domain
    lib/               — inheritable / abstract library objects (/lib/ enforced for inheritance)
        <library>.c
    obj/               — cloneable objects (master = class; clones = instances)
        <object>.c
    sys/               — singleton daemons (one master per file; no clones)
        <daemon>.c
    data/              — Light-Weight Objects (structured value types)
        <record>.c
```

The four subdirectory conventions (`lib`, `obj`, `sys`, `data`) are not just style. The host driver enforces the `/lib/` requirement on `inherit_program`. The System auto's `find_object` discipline depends on path-based type recognition. The System auto's `clone_object` wrapper only accepts paths containing `/obj/`. An application that puts a cloneable under `lib/` cannot clone it. An application that tries to inherit from an object under `obj/` will get a compile-time rejection.

Where the data inside those objects belongs -- the property table versus plain typed members, per datum -- is placement doctrine with its own consequences (`docs/where-code-belongs.md` Where state belongs). Modeling domain data below covers the shapes and lookup patterns that sit on top of that choice.

## Modeling domain data

The platform's pitch -- the object graph IS the storage layer -- leaves the author the half a database schema used to do: choosing entity shapes, finding things again, and enumerating safely. The patterns:

**Entity shape: rows in a daemon, or clones.** Small and uniform domain data lives as mappings in the owning `sys/` daemon (the composite example's inventory: one `items` mapping, ids to rows). Entities with real identity -- per-instance behavior, cross-references, independent lifecycle -- are clones, each its own persistent dataspace. The fork is sizing as much as style: one mapping caps at the driver's pair ceiling and swaps as one unit, while clones spend `objects`-table slots (`docs/kernel-libraries.md` Choosing a collection; `docs/operations.md` Sizing a workload).

**Identity lookup: logical names.** An entity that other domains find by name registers one: `set_object_name("App:things:name")` at create, `find_named(name)` from anywhere -- global and O(1) through the Index daemon, and the same registry the Vault and `OBJ(...)` references resolve through (`docs/kernel-libraries.md` /lib/util/named.c).

**Field lookup: a secondary index is a mapping you maintain at the write site.** There is no query planner; "find items by creator" is a second mapping kept beside the first, updated in the same atomic function that mutates the store:

```c
private mapping items;          /* id : row */
private mapping byCreator;      /* creator : ({ ids }) */

atomic int create_item(string name, int qty, string creator)
{
    id = nextId++;
    items[id] = ([ "name" : name, "qty" : qty, "creator" : creator ]);
    if (!byCreator[creator]) byCreator[creator] = ({ });
    byCreator[creator] += ({ id });
    return id;
}
```

Atomicity is what makes this cheap where it was hard elsewhere: the store write and the index write share one atomic task, so they commit or roll back together -- an index can never half-agree with its store, and no lock or reconciliation job exists to write. When the write site is not yours to edit (the writes are dispatched properties), attach the index as an observer instead (`docs/dispatcher.md` register_observer) and it updates synchronously in the same envelope.

**Enumeration under the tick budget.** Walking a large store in one task risks `Out of ticks`; the idiom is the sliced sweep -- process a bounded chunk, save the cursor, re-arm with `call_out` -- shown at Spreading work across timeslices below, with the cross-task staleness rule it carries (`docs/execution-model.md` What serialization does not give you).

**When the mapping outgrows.** The collection fork (`/lib/Array`, `KVstore`, `BTree`) trades dataspace-granularity paging against `objects`-table slots; take it before the first real domain model outgrows the demo shapes (`docs/kernel-libraries.md` Choosing a collection).

## The initd's role

Every tier-E domain must have an `initd.c` at its root. The System initd compiles each domain's `initd.c` during cold boot. The domain's `initd::create()` runs inside the System initd's create envelope.

A minimal `initd.c` (derived from `examples/http-app/initd.c`):

```c
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("obj/<server>");
    compile_object("sys/<daemon>");
}
```

Two points the example carries:

- **`inherit "/usr/System/lib/auto"`**: inherits the System auto, which adds the System-tier API surface to the initd's master. Tier-E domains generally inherit System auto in their `initd.c` so the initd can reach the cross-domain helpers System exposes.
- **`::create()`**: chains to the inherited `create()` so the System auto's bootstrap completes before the application-specific compile calls run.

The initd's responsibilities:

1. **Compile boot-time objects.** Singletons under `/usr/<App>/sys/`, libraries under `/usr/<App>/lib/`, and any cloneable masters that need to exist before the first request arrives. Compilation happens inside the System initd's create envelope at cold boot. Statedump-restored boots skip this entirely.
2. **Register cross-domain hooks if any.** If the application registers an object manager (see below) or binds to a connection port the kernel did not bind, the registration happens here.

The initd is not a long-running daemon. Once `create()` returns, the initd's master remains in the object table but is not normally called again at runtime.

The System initd has already called `add_owner(<App>)` for the domain before compiling the domain's initd. The domain does not register itself. Access bits for the domain's own directory tree are set at owner creation. User-tier code does not call `set_access` on its own tree.

## Owner, creator, and clone identity

Three identities matter in tier-E code, all derived from the object's path:

- **Owner**: the user identity that owns the object's storage. For a tier-E object at `/usr/<App>/.../X.c`, the owner is `<App>`.
- **Creator**: typically the same as owner for tier-E. The creator is the user identity granted the privilege to compile, clone, and modify the object.
- **Clone owner**: for cloned objects, the runtime tracks which object did the cloning. The clone's path is `/usr/<App>/obj/X#N` where `N` is the unique clone index. The clone owner is the caller of `clone_object`, which may differ from the master's owner.

The kernel layer's access checks use these identities on the masked file and compile operations. An application running under owner `<App>` cannot, by default, read or write files in another tier-E domain. It reads another domain only when that domain is published for global read (as the shipped structured-state domains are at boot) or through a specific access-daemon grant.

`previous_object()` and `previous_program()` (host-driver kfuns) report which object and program made the current call. Capability-bounded API surfaces use these to check the caller's tier before dispatching: a System-tier-only function can guard with `if (sscanf(previous_program(), "/usr/System/%*s") != 1) error("permission denied")`.

## Owner and access

The default access posture for a tier-E domain at boot:

- The domain's owner has read+write to its own directory tree, established by the System initd's `add_owner(<App>)` call before the domain's own `initd.c` runs.
- Other domains have no access to this domain's files without explicit grant.
- The System tier's cross-domain reach is ambient (the kernel auto's checks exempt System-creator code). `set_global_access` runs the other way, publishing a named domain's own tree for global read: the System initd publishes `/usr/System` and the shipped structured-state domains at boot. Other user tiers read across domains only via specific grants.

Cross-domain reach for tier-E code is mediated at every relevant kfun call by the access daemon at `/kernel/sys/access_daemon.c`. The access primitives themselves (the `set_access`, `query_user_access`, `query_file_access`, `set_global_access` methods on `/kernel/lib/api/access.c`) are kernel-tier. An application that needs to expose a public read-only library typically requests the access grant through a System-tier helper rather than calling kernel access primitives directly. Applications that ship a binary or HTTP service do not need to manipulate access bits at all: the platform routes cross-tier calls through the System-tier libraries the application inherits, and access checks happen automatically inside those.

## Identity and request authentication

`docs/identity.md` is the doctrine for the shared identity substrate -- records, passkey and agent credentials, ceremonies, sessions, and the three-layer authorization split; this section keeps only the map an application author needs, and what remains the application's to build.

**Three identity circuits, one of which is yours to bind.** *Code* identity is structural: every object runs under an owner within a capability tier, and the store's principals are ambient-derived strings (`docs/capability.md` The mechanism). *Operator* identity is the separate console circuit that never crosses into applications (`docs/identity.md` The boundary). *Human and agent* identity is the shared substrate: one record per identity, the `identity:<uuid>` principal, and the same `authd` facade and `validate` call whether the session's subject is a human or an agent -- agent-ness is a substrate query, not a transport concern (`docs/identity.md`).

**HTTP connections are anonymous until the application binds a session.** The identity daemons are System-tier surfaces a tier-E application cannot call; the transport reaches ceremonies and sessions through the `authd` facade, and the binding is the application's -- no cookie handling and no bearer parsing ship, so an application threads a validated session onto its own request flow. (The HTTP surface does carry RFC-shape parsers for the `Authorization` and `WWW-Authenticate` headers -- value objects under `src/usr/HTTP/api/lib/Authentication.c` -- that nothing in the HTTP layer enforces.) `examples/composite-app` is the worked binding (`docs/composite-applications.md` Authenticating a wire request).

**What an application still builds.** The platform contributes the credential substrate, the ceremonies, the session primitive, and the capability discipline. An application still supplies: the transport binding (cookie, bearer, or its own scheme), its application-tier authorization policy (gate its verbs the way the chat example gates rooms, `docs/chat-applications.md`), and any per-user state beyond the shared record. Quotas remain per-owner, not per-identity.

## Writing tick-aware code

The platform has no threads and no preemption. What bounds a runaway computation is the **tick budget**: every entry into application code runs under per-owner resource limits, and exceeding them is a runtime error (`Out of ticks`), not a hung platform. The mechanics -- what a tick charges, where the 20,000,000-tick default ceiling comes from, what exhaustion does, atomic functions costing double, and the operator's consumption and quota surface -- live with the task model in [execution-model.md](execution-model.md) The tick budget, mechanically. What this page keeps is the author-facing idiom.

**Spreading work across timeslices.** Each fired `call_out` runs under a *fresh* budget, computed from the owner's quota at fire time. The kernel wraps every application call_out in the same `call_limited` envelope. This is the platform's idiom for work larger than one budget: process a bounded chunk, save the cursor in object state, re-arm with `call_out("continue_work", 0, cursor)`. Each completed slice's mutations stand on their own -- a fired handler is an ordinary non-atomic call, so declare it `atomic` if a mid-slice error must roll the slice back -- and design the chunk boundaries so each completed slice leaves consistent state. Treat values read in an earlier slice as stale: other tasks run between slices, so re-read at write time ([execution-model.md](execution-model.md) What serialization does not give you). The continuation libraries under `src/lib/` ([kernel-libraries.md](kernel-libraries.md)) package this pattern.

```c
static void rebuild_index(int cursor)
{
    int end;

    end = (cursor + CHUNK < sizeof(items)) ? cursor + CHUNK : sizeof(items);
    /* ... process items[cursor .. end - 1] ... */
    if (end < sizeof(items)) {
        call_out("rebuild_index", 0, end);   /* next slice, fresh budget */
    }
}
```

Watching consumption and the quota surface (`rsrc ticks`, usage throttles, the quota mindset for callout and tick errors) moved with the mechanics: [execution-model.md](execution-model.md) The tick budget, mechanically.

## Object tracking

The platform ships an object manager (`/usr/System/sys/objectd.c`) that tracks every compiled object's identity, inheritance graph, included files, and active issues; the driver dispatches the object-lifecycle events to it. A tier-E application consumes objectd's query API -- enumerate objects by owner, walk an inheritance graph, find objects sharing a header, identify what a recompile leaves to touch -- and does not normally register a replacement or touch the driver's raw callbacks: the platform routes those to the specialized System daemons (`upgraded`, `errord`). The lifecycle model and event vocabulary are [code-lifecycle.md](code-lifecycle.md)'s; the interface surfaces are the daemon sources (`src/usr/System/sys/objectd.c`, `upgraded.c`, `errord.c`).

## Live code upgrade through call_touch

`call_touch(obj)` (wrapped in the kernel auto at `/kernel/lib/auto.c::call_touch`) marks an object as needing an upgrade. The next time `obj` is called, the kernel driver intercepts the call, dispatches `touch(obj, function)` to the object manager (objectd), and objectd in turn calls `obj->_F_touch()` on the marked object. Only after `_F_touch()` returns does the originally-intended call proceed.

The application-implemented hook is `patch()`. `_F_touch()` itself is the `nomask` platform gate in the System auto library (`src/usr/System/lib/auto.c`). Applications cannot override it. Its body calls `this_object()->patch()`. Applications that need pre-upgrade migration logic implement `patch()`. The platform guarantees it runs at most once per `call_touch`, before the next call against the object (`docs/code-lifecycle.md` `call_touch` and `_F_touch`). The working demonstration is `examples/upgrade-cascade/`.

Why not upgrade immediately when a library recompiles? Two reasons:

1. **Volume.** A widely-inherited library may have hundreds or thousands of dependent instances. Touching every instance at recompile time can exceed the runtime's tick budget for a single atomic envelope. The upgrade aborts and the recompile is wasted.
2. **Locality.** Upgrading just before next-use distributes the migration cost across the rate at which instances are actually used. Long-idle objects pay the cost only when they next matter.

The trade-off: long-idle objects can accumulate multiple pending upgrades without being touched. If the migration logic depends on the most-recent prior version's data shape, a long-idle object may have an upgrade chain that no longer matches. Mitigations:

- **Periodic global touch.** A scheduled job (via `call_out`) walks all objects within a domain at low frequency (daily, weekly) and ensures each has been touched at least once per cycle.
- **Snapshot-and-restore.** Statedump preserves all object state. A planned restart cycle that statedumps and restores can pair with an explicit touch pass during the restore phase.

The `call_touch` primitive itself is host-driver-level (tier A). The kernel auto wraps it. Objectd dispatches it. The System auto's `_F_touch()` gate forwards to the application's `patch()` hook.

## Non-HTTP transports

The kernel binds the telnet and binary ports at boot via the kernel userd's connection acceptors. The System http_server registers as the binary-port manager so HTTP/1 connections route through `/usr/WWW/obj/server`. A tier-E application that needs a different protocol can either:

1. **Bind an additional port.** Add a new entry to the `.dgd` configuration's `telnet_port`, `binary_port`, or `datagram_port` mapping (one host:port pair per entry). At boot the kernel creates an acceptor for each port. The application registers a manager for the new port via the kernel userd.

2. **Layer on the binary port.** Reuse the binary port the kernel already binds and dispatch by content shape. The kernel ships HTTP/1 wired through `/usr/WWW/obj/server`. A non-HTTP protocol on the same port would require an application-supplied content-shape sniff. Binding a separate port is the common choice.

The connection-handling contract for non-HTTP applications is the binary-manager pattern: applications inherit a user library and override the methods that fire when bytes or lines arrive. The reference surfaces:

- **`/kernel/lib/user.c`**: the kernel user library, the platform-side contract for what a connection-handling object provides. Methods include `set_mode(int mode)` to switch between `MODE_LINE` and `MODE_RAW` framing, plus `login(string)`, `logout(string)` for the connection lifecycle.
- **`/usr/System/lib/user.c`**: the System user library, which inherits the kernel user library plus the System auto.
- **`/usr/System/sys/userd.c`**: the System userd, which handles the binary-manager protocol on the kernel's behalf.

For the canonical HTTP/1 worked example of this pattern, see `examples/http-app/obj/server.c`: an application server that inherits `/usr/HTTP/api/lib/Server1` plus `/usr/System/lib/user`, replicates the binary-manager glue (`login`, `flow_*`, `timeout`, `_logout`), and uses the kernel's mode-switching to consume HTTP requests. The same shape generalizes to other line- or frame-oriented protocols.

For datagram-shaped applications (UDP), the platform ships the datagram transport scaffolding (`docs/runtime-primitives.md` Supporting surfaces), but no canonical datagram application yet.

## Outbound connections

The outbound client surface now lives beside its signatures: `docs/http-applications.md` Outbound connections states what ships, the call chain and its gate, the callback lifecycle, the two timeouts, and the untested atomic-envelope caveat. The short form for this page's reader: initiation rides the same user-library pattern as inbound (Non-HTTP transports above), the proven shape is `Http1Client` composed with `BufferedConnection1` as `examples/composite-app`'s loopback client does, and every callback is its own task, so re-validate state where the callback fires rather than trusting a pre-connect read.

## Worked example sketch: an in-memory KV service

The platform's persistence + atomicity guarantees make an in-memory key-value service near-trivial to implement at the application layer. The shape:

```text
src/usr/KV/
    initd.c            — compiles the daemon at boot
    sys/
        kv_daemon.c    — singleton holding the mapping
```

The `kv_daemon.c` carries a single `private mapping store` variable. `put(key, value)` assigns, `get(key)` reads, and `remove(key)` deletes. (A single mapping caps at 32,767 entries on a stock build; when a real store approaches that, `docs/kernel-libraries.md` Choosing a collection and `docs/operations.md` Sizing a workload carry the decision rule.) The platform guarantees:

- **Persistence**: the `store` mapping survives restart without explicit serialization.
- **Atomicity**: a multi-step write (e.g., transactional rename of a key) is atomic if wrapped in a single function call. A partial failure rolls back.
- **Multi-agent coherence**: callers see a consistent view. The runtime serializes commits.
- **Capability separation**: only callers reachable from the KV domain (or from System) can call into `kv_daemon`. The access discipline is enforced by the kernel's per-call access checks against the inherited System auto.

The application code is short: the daemon itself is roughly 30 lines of LPC, plus the initd's compile call. The platform carries the rest.

A counter-with-deliberate-failure variant (the canonical atomicity demonstration) is similar shape: a single counter object with `increment_and_fail()` exercises the rollback path. The post-failure read confirms the counter is unchanged.

## Reference applications

This section walks through three of the examples that ship under `examples/` (nine in total, with the rest documented in `docs/runtime-primitives.md`, `docs/chat-applications.md`, and `docs/signal-applications.md`). Each one exercises **one** runtime primitive end-to-end. `examples/vault-app/` and `examples/merry-app/` verify against a sentinel-file assertion the operator can check with `cat src/usr/<Domain>/data/test-result.log` after cold boot. `examples/http-app/` verifies over HTTP instead (see its table entry). They are deliberately minimal: the sentinel-file examples' test driver writes a log line, not a network packet. The data persisted is contrived. The demonstration target is one property at a time.

| Example | Primitive demonstrated | Walkthrough |
|---|---|---|
| `examples/http-app/` | Transport (HTTP/1 server with `GET /health`, `POST /echo`, 404 fallback) | `docs/http-applications.md` |
| `examples/vault-app/` | Persistent state (XML round-trip via stateimpex, restart-survival) | `docs/vault-applications.md` |
| `examples/merry-app/` | Sandboxed code load (Merry source bound to a property, ancestry walk, five-phase exercise of sandbox firing, Spawn, $delay, LabelCall) | `docs/merry-applications.md` |

`examples/vault-app/` and `examples/merry-app/` share a layout shape (`examples/http-app/` is simpler, just `initd.c`, `obj/server.c`, and `README.md`, documented in `docs/http-applications.md`):

```text
examples/<name>/
    README.md           — what the example demonstrates; deploy + run instructions
    initd.c             — domain bootstrap; compiles lib/app, obj/thing, sys/test
    lib/app.c           — domain-level library inherited by clones
    obj/thing.c         — the demonstrated object surface (script-bearing, persistent, transport-bound)
    sys/test.c          — boot-time test driver; writes sentinel to test-result.log
```

The shape is **intentional and minimal**, not canonical for all applications. The reference examples each demonstrate one primitive. They do not show what an application looks like when several primitives compose. A composite application (say, a multi-user persistent service that accepts connections, persists user and message state via Vault, dispatches scripted reactions via Merry, and serves history over HTTP) has a different shape: multiple sys/ daemons (one per concern), multiple lib/ files (one per inherited surface), a connection-handling object distinct from the persistent-data object, often a transport-specific subdirectory alongside `obj/`. The `obj/<thing>.c` + `sys/<daemon>.c` split widens into a family of files coordinating around the application's persistent surface.

When to copy the reference shape directly:

- The application demonstrates or exercises **one** primitive at a time (a test fixture, a debug harness, a per-primitive smoke test in CI).
- The application is genuinely simple: one daemon, one cloneable kind, no transport, sentinel-file or log-line is the natural assertion surface.

When to adapt the shape:

- Multiple primitives compose. Add files. Do not force everything into the four-file skeleton.
- Production-style application with users, sessions, transport, persisted state across multiple object kinds, scripted behavior. The example pattern is a starting point for boot-up wiring and access posture, not a layout template.
- The application needs a long-running operator surface (admin console verbs, configuration daemons, monitoring hooks). These are System-tier or co-tier additions that the reference examples deliberately omit.

The shape is a teaching surface for the platform's individual primitives, not the recommended posture for a tier-E application that means to do useful work. Read `docs/runtime-primitives.md` for what each example assumes about the runtime. Read this document's earlier sections (`Domain layout`, `The initd's role`, `Owner and access`, `Live code upgrade through call_touch`) for the patterns that apply regardless of primitive count.

## Testing your application

### The sentinel-driver pattern

Seven of the nine bundled examples ship their regression as a boot-time test driver at `sys/test.c` (`atomic-demo` and `http-app` verify over live HTTP instead), deferred to a `call_out` from `create()` so every domain's `initd` has finished before it calls cross-domain daemons. Each phase of the driver is wrapped in `catch{}` and appends one line to a result-log file (conventionally `/usr/<App>/data/test-result.log`): `"<App>:test: <PHASE> OK"` on success, `"<App>:test: FAIL: <reason>"` on failure. Wrapping each phase separately means one phase's failure does not mask a different failure in another. `scripts/run-example.sh` (`scripts/README.md`) reads the result log back, counts `" OK"` lines against a per-example expected count, and fails the run on any `FAIL` line or a count mismatch.

`examples/chat-app/sys/test.c` is the most heavily annotated of the bundled drivers: its header comment enumerates every phase by sentinel name and what it exercises, and each phase in the body carries an inline comment naming the primitive under test. It is the reference to read first when writing a new one. The shared `log_line` helper every driver repeats:

```c
private void log_line(string msg)
{
    mixed *info;
    int size;

    catch {
        info = file_info(RESULT_FILE);
        size = info ? info[0] : 0;
        write_file(RESULT_FILE, msg + "\n", size);
    }
}
```

### Two-boot snapshot-restore

A driver that asserts survival across a restart follows the two-boot recipe `merry-app` and `chat-app` use for their restart phases (`vault-app`'s round-trip phases stay within one boot: they exercise export/import, not process death). `/usr/System/sys/persist_helper::trigger_dump_and_exit()` schedules a full `dump_state(FALSE)` plus `shutdown()` via a `call_out`, so the caller's stack unwinds before the snapshot is taken. Boot 1 runs its phases, keeps whatever objects need to survive as non-static globals (so the dump captures them), schedules a verify `call_out` far enough out to still be pending at dump time, then calls `trigger_dump_and_exit`. The process exits when `shutdown()` runs. Boot 2 restarts DGD against the written snapshot (`state/snapshot`). DGD's orthogonal persistence restores the object graph including the pending call_out, which fires as soon as the system is back up and asserts the restored state.

`examples/merry-app/sys/test.c` (phases 16/17, PERSIST SETUP / PERSIST VERIFY) and `examples/chat-app/sys/test.c` (phases 10/11, PERSIST-SETUP / PERSIST-VERIFY) both follow this shape. Chat-app adds a third, cold, no-snapshot boot afterward to show the contrast: an on-disk marker file survives, but the in-image session does not, without a snapshot to restore from.

### Adapting run-example.sh to a new domain

`scripts/run-example.sh` resolves each example's boot recipe through its `example_profile()` shell function: one hardcoded case line per example naming the deploy directory, boot count, boot-1 mode, and expected OK count. There is no directory-convention or config-file discovery: pointing the runner at a new domain means adding a case to `example_profile()` in `scripts/run-example.sh` itself, which is exactly what its own error message says when the profile is missing: `no profile for '<example>'; add one to example_profile()`. The rest of the script (clean-slate deploy, boot orchestration, sentinel counting) needs no change.

## Where to next

- [`docs/where-code-belongs.md`](where-code-belongs.md) covers the placement doctrine behind this document's mechanics: plain LPC versus a Merry script, and which compiled shape fits a new piece of behavior.
- [`docs/architecture.md`](architecture.md) covers the tier model, daemons, boot sequence, and auto-inheritance chain this document builds on.
- [`docs/runtime-primitives.md`](runtime-primitives.md) gives the per-primitive foundation-and-proof statement (atomicity, persistence, hot reload, capability separation, the rest).
- [`docs/lpc-essentials.md`](lpc-essentials.md) gives LPC language orientation: types, type modifiers, inheritance, atomicity, `call_out`, error handling. The bridge to the formal spec at [LPC.md].
- [`docs/kernel-libraries.md`](kernel-libraries.md) documents inheritable libraries shipped under [`src/lib/`](../src/lib/): String / StringBuffer, KVstore, Iterator family, Continuation family, Time, and the small `/lib/util/` set.
- [`docs/http-applications.md`](http-applications.md) covers the HTTP/1-specific application pattern with [`examples/http-app/`](../examples/http-app/) as the runnable reference.
- [`docs/operations.md`](operations.md) gives the operator's view (admin_console use, statedump cadence, rlimits, JIT deployment posture).

[LPC.md]: https://github.com/dworkin/lpc-doc/blob/master/LPC.md
