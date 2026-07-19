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

What identity means on this platform, surface by surface, and what an application still builds for itself. The architecture for binding external credentials to durable identities is now decided and shipped: a shared identity substrate with passkey (WebAuthn) authentication. `docs/identity.md` is the full doctrine; this section places it against the surfaces an application author already knows.

**Code identity is structural.** Every object runs under an owner within a capability tier, and the capability store's principals are ambient-derived strings (owner, program path, object name -- `docs/capability.md` The mechanism). This is the identity model that gates *code*, and it is distinct from the identity of a *person or agent* using an application.

**Human and agent identity is the shared substrate.** One identity record per human or agent, minted by `identityd`, carrying typed credential rows and a principal string `identity:<uuid>` (`docs/identity.md`, `docs/system-daemons.md`). Every application consumes the same record rather than keeping its own user table. Passkeys (WebAuthn, TOFU registration) are the human credential type; `webauthnd` runs those ceremonies. An agent -- an automated process acting for a human -- is the same record shape with an immutable controller edge, authenticating by domain-separated challenge-sign or a hashed expiring token through `agentauthd`, and reaching a session through the same `authd` facade an application already binds (`docs/identity.md` Agent identities). To an application, an authenticated agent session looks exactly like a human one: the same `identity:<uuid>` principal arrives and the same `validate` call proves it -- agent-ness is a substrate query, not a new transport concern. An authenticated identity's principal is the seam into authorization: an application gates its own actions on it (application-tier authorization), an operator may grant it a platform capability through the store, and a controller may delegate its own capabilities to its agents (`docs/identity.md` The three-layer authorization split).

**Operator identity is a separate circuit.** A registered operator is a kernel access-list entry plus a password hash held in the operator's user object, authenticated by password over the tunneled console (`docs/admin-console.md` Connecting). Operators are administrators, not application users, and the two circuits never cross: a passkey identity is never minted as a kernel user or owner and never grants console access (`docs/identity.md` The boundary).

**HTTP connections are anonymous until the application binds a session.** The HTTP/1 bootstrap clones the application server per connection and associates no identity with it. The identity daemons themselves are System-tier surfaces a tier-E application cannot call; the transport reaches ceremonies and sessions through the `authd` facade (`docs/system-daemons.md` authd), and the transport binding is the application's: no cookie handling and no HTTP bearer parsing ship, so an application threads a validated session onto its own request flow. The HTTP surface does carry RFC-shape parsers for the authentication headers (`Authorization` / `WWW-Authenticate` into value objects under `src/usr/HTTP/api/lib/Authentication.c`) that nothing in the HTTP layer enforces; `examples/composite-app` is the worked binding of all three pieces (`docs/composite-applications.md`).

**What an application still builds.** The platform now contributes the credential substrate, the ceremonies, the session primitive, and the capability discipline. An application still supplies: the transport binding (how a validated session token reaches its request handling -- cookie, bearer, or its own scheme), its application-tier authorization policy (gate its verbs the way the chat example gates rooms, `docs/chat-applications.md`), and any application-specific per-user state beyond the shared identity record. Quotas remain per-owner, not per-identity.

## Writing tick-aware code

The platform has no threads and no preemption. What bounds a runaway computation is the **tick budget**. Every entry into application code runs under per-owner resource limits, and exceeding them is a runtime error, not a hung platform. This section is the execution model an application author needs. All of it derives from the driver and kernel source (`src/kernel/lib/auto.c` `call_limited`, `src/kernel/sys/resource_daemon.c`, and the driver's interpreter).

**What a tick is.** A tick is the driver's unit of execution cost, charged as code runs: every function call costs a fixed overhead, every loop iteration is charged on the backward jump, global-variable reads and writes are charged per access, and aggregate operations (constructing or spreading arrays and mappings, expensive kfuns like `crypt` or `parse_string`) are charged in proportion to the data size. The exact per-operation costs are driver internals and may change between driver versions. The model to internalize is *work costs ticks, and data-proportional work costs data-proportional ticks*.

**Where the budget comes from.** The kernel routes every call into non-kernel code through `call_limited`, which asks the resource daemon for the owner's limits and runs the call under an `rlimits` envelope. The platform-wide default ceiling is 20,000,000 ticks per execution (the `ticks` resource, set at boot in `src/kernel/sys/driver.c`). An operator can lower or raise a specific owner's ceiling with `quota <owner> ticks <limit>` ([admin-console.md](admin-console.md) Managing resources). Application code cannot raise its own budget: the driver consults the kernel's `runtime_rlimits` gate for any non-kernel `rlimits` use, and the gate refuses requests for more than what remains. (Merry scripts cannot use `rlimits` at all: the grammar rejects it. See [merry-language.md](merry-language.md).)

**What exhaustion does.** Running past the budget raises the error `Out of ticks` in the offending call. The error propagates like any other: mutations inside the atomic context roll back, the platform keeps running, and other owners are unaffected. The budget is the platform's containment of runaway code: an unbounded loop costs its owner an error, not the system its liveness.

**Atomic functions cost double.** On entry to an `atomic` function the remaining tick budget is halved (restored at commit or rollback). Budget for an atomic operation accordingly: a traversal that fits comfortably outside `atomic` may exhaust inside it.

**Spreading work across timeslices.** Each fired `call_out` runs under a *fresh* budget, computed from the owner's quota at fire time. The kernel wraps every application call_out in the same `call_limited` envelope. This is the platform's idiom for work larger than one budget: process a bounded chunk, save the cursor in object state, re-arm with `call_out("continue_work", 0, cursor)`. Each completed slice's mutations stand on their own -- a fired handler is an ordinary non-atomic call, so declare it `atomic` if a mid-slice error must roll the slice back -- and design the chunk boundaries so each completed slice leaves consistent state. The continuation libraries under `src/lib/` ([kernel-libraries.md](kernel-libraries.md)) package this pattern.

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

**Watching consumption.** The resource daemon records ticks consumed per owner into a decaying usage counter (10% decay per hour by default), including consumption on calls that ended in an error. Operators read it with `rsrc ticks` (per-owner usage, ceiling, and throttle threshold) and can set a usage throttle with `quota <owner> ticks usage <float>`: an owner whose decayed usage exceeds the threshold is clamped to effectively no computation until usage decays back under it. For the investigation sequence when an owner is eating the platform, see [admin-console.md](admin-console.md) Debugging a stuck platform.

**The quota mindset.** Per-owner quotas cover more than ticks: objects, call_outs, and stack depth, all of which `quota <owner>` lists. For an application author the practical readings are: an error about exceeding the callout quota means the design leans on unbounded deferral. An `Out of ticks` in normal operation means a traversal needs the chunking idiom above, not a bigger quota. A quota raise is the right call only when the workload is legitimately that large and the operator owns the decision.

## Object tracking

The platform ships an object manager at `/usr/System/sys/objectd.c` that tracks every compiled object's identity, inheritance graph, included files, and active issues. The kernel driver registers `objectd` as the object manager at boot (via `driver->set_object_manager(this_object())` from objectd's own `create`), and the driver dispatches object-lifecycle events (`compile`, `clone`, `destruct`, `touch`, `include_file`, and related) to objectd.

Tier-E applications consume objectd's query API. They do not normally register a replacement. The query surface lets an application enumerate objects by owner, walk an object's inheritance graph, find objects sharing an included header, and identify which objects need to be touched after a recompile.

Replacing the shipped object manager is possible (`driver->set_object_manager(<replacement>)` is the platform's hook), but rare. The shipped objectd handles the common cases (object tracking, upgrade coordination, include-file mapping). Applications that need additional behavior typically register their own daemon that calls into objectd, rather than replacing it.

Applications also do not normally manipulate the driver's lifecycle callbacks directly. The platform's objectd routes the events to specialized System-tier daemons (`upgraded` for live upgrades, `errord` for error reporting). Tier-E applications interact with those daemons rather than with the driver's raw callback set. See `src/usr/System/sys/objectd.c`, `src/usr/System/sys/upgraded.c`, and `src/usr/System/sys/errord.c` for the actual interface surfaces.

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

The platform can initiate connections outward, and the surface is smaller and less proven than the inbound one. Stated plainly:

**What ships.** `Http1Client` (and its TLS variant `Http1TlsClient`) compiles at boot: an inheritable client library whose `create(object client, string host, int port, string responsePath, string headersPath)` initiates the connection, with the signatures in `docs/http-applications.md` API signatures. The first in-tree consumer is `examples/composite-app`'s loopback test client (`Inventory/obj/client.c`), and being first it surfaced three latent defects in the plain-client path (a double connect in `obj/client1.c`, a driver-level `MODE_BLOCK` nothing lifts, and a line-framing race on single-segment responses -- all documented in that client's header). Adopt the shape that works: compose `Http1Client` with `/usr/HTTP/api/lib/BufferedConnection1` and keep the driver-level connection raw, as that client and the TLS variants do.

**The call chain and its gate.** An application cannot open a socket directly: the kernel auto's `connect()` is gated to the kernel's connection machinery, and the socket opens through a kernel connection object bound to a user object -- the same user-library pattern as inbound (Non-HTTP transports above). Inheriting `Http1Client` beside `/usr/System/lib/user` is the packaged form of that route.

**The callback lifecycle.** `connect` is fire-and-forget; no call blocks. The outcome arrives as callbacks: `connected()` on success, `connectFailed(int errorcode)` on failure, with the error codes in `src/include/connect.h` (`CONNECT_REFUSED`, `CONNECT_HOST_UNREACH`, `CONNECT_NET_UNREACH`, `CONNECT_TIMEOUT`, and the unspecified `CONNECT_FAILURE`). Response data then arrives through the flow contract as new tasks -- every received chunk is its own task under run-to-completion (`docs/execution-model.md`), so a pending response never holds a task open.

**Timeouts.** Two exist, one is yours: the established-connection inactivity timeout is the client class's `inactivityTimeout()` override (default 120 seconds), while the pending-connect timeout is the operating system's TCP timeout, which the host driver detects and surfaces as `connectFailed(CONNECT_TIMEOUT)` -- there is no LPC-side knob for it. The idiom for a request deadline is a `call_out` armed when the request is sent, cancelled in `receiveResponse`; if it fires first, `terminate()` the connection and treat the exchange as failed.

**The atomic envelope.** How an in-flight outbound connect interacts with an enclosing atomic function is undocumented and untested today: no doc states whether the initiation is deferred, refused, or performed immediately, and no shipped code initiates one from atomic context. Treat it in the same register as the platform's other unproven interactions (`docs/operations.md` Open empirical questions): do not initiate outbound work inside an atomic function until someone has proven the behavior.

The roadmap's transport posture carries the activation status for the outbound clients (`docs/runtime-platform-roadmap.md` Transport posture).

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
