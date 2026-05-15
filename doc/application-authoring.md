<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Application Authoring

A tier-E application on eOS-kernellib lives in a single directory under `src/usr/`, declares its bootstrap in an `initd.c`, and inherits the substrate's System auto to gain owner identity and cross-tier API access. The patterns below apply regardless of transport: domain layout, owner / creator / access conventions, the object-manager event lifecycle, code upgrade through `call_touch`, and non-HTTP transport bindings.

**Audience**: an LPC author writing a tier-E application on top of eOS-kernellib; comfortable with LPC syntax (or read `doc/lpc-essentials.md` first); has the substrate running locally per `doc/getting-started.md`.

The architecture document (`doc/architecture.md`) covers the substrate's tier model, daemons, and inheritance chain. The HTTP/1-specific application pattern is covered separately in `doc/http-applications.md` with a runnable reference at `examples/http-app/`.

## Domain layout

A tier-E application lives in a single directory under `src/usr/`. The directory's name is the application's owner identity. The System initd discovers the domain at boot via the `get_dir("/usr/[A-Z]*")` iteration and compiles the domain's `initd.c`.

A typical tier-E domain layout:

```
src/usr/<App>/
    initd.c            -- compiled at boot; bootstraps the rest of the domain
    lib/               -- inheritable / abstract library objects (/lib/ enforced for inheritance)
        <library>.c
    obj/               -- cloneable objects (master = class; clones = instances)
        <object>.c
    sys/               -- singleton daemons (one master per file; no clones)
        <daemon>.c
    data/              -- Light-Weight Objects (structured value types)
        <record>.c
```

The four subdirectory conventions (`lib`, `obj`, `sys`, `data`) are not just style. The host driver enforces the `/lib/` requirement on `inherit_program`; the System auto's `find_object` discipline depends on path-based type recognition; and the kernel's `clone_object` only accepts paths containing `/obj/`. An application that puts a cloneable under `lib/` cannot clone it; an application that tries to inherit from an object under `obj/` will get a compile-time rejection.

## The initd's role

Every tier-E domain must have an `initd.c` at its root. The System initd compiles each domain's `initd.c` during cold boot; the domain's `initd::create()` runs inside the System initd's create envelope.

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

- **`inherit "/usr/System/lib/auto"`** -- inherits the System auto, which adds the System-tier API surface to the initd's master. Tier-E domains generally inherit System auto in their `initd.c` so the initd can reach the cross-domain helpers System exposes.
- **`::create()`** -- chains to the inherited `create()` so the System auto's bootstrap completes before the application-specific compile calls run.

The initd's responsibilities:

1. **Compile boot-time objects.** Singletons under `/usr/<App>/sys/`, libraries under `/usr/<App>/lib/`, and any cloneable masters that need to exist before the first request arrives. Compilation happens inside the System initd's create envelope at cold boot; statedump-restored boots skip this entirely.
2. **Register cross-domain hooks if any.** If the application registers an object manager (see below) or binds to a connection port the kernel did not bind, the registration happens here.

The initd is not a long-running daemon. Once `create()` returns, the initd's master remains in the object table but is not normally called again at runtime.

The System initd has already called `add_owner(<App>)` for the domain before compiling the domain's initd; the domain does not register itself. Access bits for the domain's own directory tree are set at owner creation; user-tier code does not call `set_access` on its own tree.

## Owner, creator, and clone identity

Three identities matter in tier-E code, all derived from the object's path:

- **Owner** -- the user identity that owns the object's storage. For a tier-E object at `/usr/<App>/.../X.c`, the owner is `<App>`.
- **Creator** -- typically the same as owner for tier-E. The creator is the user identity granted the privilege to compile, clone, and modify the object.
- **Clone owner** -- for cloned objects, the runtime tracks which object did the cloning. The clone's path is `/usr/<App>/obj/X#N` where `N` is the unique clone index; the clone owner is the caller of `clone_object`, which may differ from the master's owner.

The System auto's access checks use these identities at every kfun call. An application running under owner `<App>` cannot, by default, read or write files in another tier-E domain; cross-domain access is per-call mediated by the access daemon, not blanket-granted.

`previous_object()` and `previous_program()` (host-driver kfuns) report which object and program made the current call. Capability-bounded API surfaces use these to check the caller's tier before dispatching: a System-tier-only function can guard with `if (sscanf(previous_program(), "/usr/System/%*s") != 1) error("permission denied")`.

## Owner and access

The default access posture for a tier-E domain at boot:

- The domain's owner has read+write to its own directory tree, established by the System initd's `add_owner(<App>)` call before the domain's own `initd.c` runs.
- Other domains have no access to this domain's files without explicit grant.
- The System tier has cross-domain reach via its `set_global_access("System", TRUE)` grant; other user tiers do not get blanket cross-domain access.

Cross-domain reach for tier-E code is mediated at every relevant kfun call by the access daemon at `/kernel/sys/access_daemon.c`. The access primitives themselves (the `set_access`, `query_user_access`, `query_file_access`, `set_global_access` methods on `/kernel/lib/api/access.c`) are kernel-tier; an application that needs to expose a public read-only library typically requests the access grant through a System-tier helper rather than calling kernel access primitives directly. Applications that ship a binary or HTTP service do not need to manipulate access bits at all -- the substrate routes cross-tier calls through the System-tier libraries the application inherits, and access checks happen automatically inside those.

## Object tracking

The substrate ships an object manager at `/usr/System/sys/objectd.c` that tracks every compiled object's identity, inheritance graph, included files, and active issues. The kernel driver registers `objectd` as the object manager at boot (via `driver->set_object_manager(this_object())` from objectd's own `create`), and the driver dispatches object-lifecycle events (`compile`, `clone`, `destruct`, `touch`, `include_file`, and related) to objectd.

Tier-E applications consume objectd's query API; they do not normally register a replacement. The query surface lets an application enumerate objects by owner, walk an object's inheritance graph, find objects sharing an included header, and identify which objects need to be touched after a recompile.

Replacing the shipped object manager is possible -- `driver->set_object_manager(<replacement>)` is the substrate's hook -- but rare. The shipped objectd handles the common cases (object tracking, upgrade coordination, include-file mapping); applications that need additional behavior typically register their own daemon that calls into objectd, rather than replacing it.

Applications also do not normally manipulate the driver's lifecycle callbacks directly. The substrate's objectd routes the events to specialized System-tier daemons (`upgraded` for live upgrades, `errord` for error reporting); tier-E applications interact with those daemons rather than with the driver's raw callback set. See `src/usr/System/sys/objectd.c`, `src/usr/System/sys/upgraded.c`, and `src/usr/System/sys/errord.c` for the actual interface surfaces.

## Live code upgrade through call_touch

`call_touch(obj)` (wrapped in the kernel auto at `/kernel/lib/auto.c::call_touch`) marks an object as needing an upgrade. The next time `obj` is called, the kernel driver intercepts the call, dispatches `touch(obj, function)` to the object manager (objectd), and objectd in turn calls `obj->_F_touch()` on the marked object. Only after `_F_touch()` returns does the originally-intended call proceed.

The application-implemented hook is `_F_touch()` on the object's class. Applications that need pre-upgrade migration logic implement this method; the substrate guarantees it runs at most once per `call_touch`, before the next call against the object.

Why not upgrade immediately when a library recompiles? Two reasons:

1. **Volume.** A widely-inherited library may have hundreds or thousands of dependent instances. Touching every instance at recompile time can exceed the runtime's tick budget for a single atomic envelope; the upgrade aborts and the recompile is wasted.
2. **Locality.** Upgrading just before next-use distributes the migration cost across the rate at which instances are actually used. Long-idle objects pay the cost only when they next matter.

The trade-off: long-idle objects can accumulate multiple pending upgrades without being touched. If the migration logic depends on the most-recent prior version's data shape, a long-idle object may have an upgrade chain that no longer matches. Mitigations:

- **Periodic global touch.** A scheduled job (via `call_out`) walks all objects within a domain at low frequency (daily, weekly) and ensures each has been touched at least once per cycle.
- **Snapshot-and-restore.** Statedump preserves all object state; a planned restart cycle that statedumps and restores can pair with an explicit touch pass during the restore phase.

The `call_touch` primitive itself is host-driver-level (tier A); the kernel auto wraps it; objectd dispatches it; the `_F_touch()` method is the application hook.

## Non-HTTP transports

The kernel binds the telnet and binary ports at boot via the kernel userd's connection acceptors; the System http_server registers as the binary-port manager so HTTP/1 connections route through `/usr/WWW/obj/server`. A tier-E application that needs a different protocol can either:

1. **Bind an additional port.** Add a new entry to the `.dgd` configuration's `telnet_port`, `binary_port`, or `datagram_port` mapping (one host:port pair per entry). At boot the kernel creates an acceptor for each port; the application registers a manager for the new port via the kernel userd.

2. **Layer on the binary port.** Reuse the binary port the kernel already binds and dispatch by content shape. The kernel ships HTTP/1 wired through `/usr/WWW/obj/server`; a non-HTTP protocol on the same port would require an application-supplied content-shape sniff. Binding a separate port is the common choice.

The connection-handling contract for non-HTTP applications is the binary-manager pattern: applications inherit a user library and override the methods that fire when bytes or lines arrive. The reference surfaces:

- **`/kernel/lib/user.c`** -- the kernel user library; the substrate-side contract for what a connection-handling object provides. Methods include `set_mode(int mode)` to switch between `MODE_LINE` and `MODE_RAW` framing, plus `login(string)`, `logout(string)` for the connection lifecycle.
- **`/usr/System/lib/user.c`** -- the System user library; inherits the kernel user library plus the System auto.
- **`/usr/System/sys/userd.c`** -- the System userd; handles the binary-manager protocol on the kernel's behalf.

For the canonical HTTP/1 worked example of this pattern, see `examples/http-app/obj/server.c`: an application server that inherits `/usr/HTTP/api/lib/Server1` plus `/usr/System/lib/user`, replicates the binary-manager glue (`login`, `logout`, `flow_*`, `timeout`), and uses the kernel's mode-switching to consume HTTP requests. The same shape generalizes to other line- or frame-oriented protocols.

For datagram-shaped applications (UDP), datagram support is a port candidate listed in `doc/substrate-primitives.md` Supporting surfaces; the substrate ships the transport scaffolding but no canonical datagram application yet.

## Worked example sketch: an in-memory KV service

The substrate's persistence + atomicity guarantees make an in-memory key-value service near-trivial to implement at the application layer. The shape:

```
src/usr/KV/
    initd.c            -- compiles the daemon at boot
    sys/
        kv_daemon.c    -- singleton holding the mapping
```

The `kv_daemon.c` carries a single `private mapping store` variable. `put(key, value)` assigns; `get(key)` reads; `remove(key)` deletes. The substrate guarantees:

- **Persistence**: the `store` mapping survives restart without explicit serialization.
- **Atomicity**: a multi-step write (e.g., transactional rename of a key) is atomic if wrapped in a single function call; a partial failure rolls back.
- **Multi-agent coherence**: callers see a consistent view; the runtime serializes commits.
- **Capability separation**: only callers reachable from the KV domain (or from System) can call into `kv_daemon`; the access discipline is enforced by the kernel's per-call access checks against the inherited System auto.

The application code is short: the daemon itself is perhaps 30 lines of LPC, plus the initd's compile call. The substrate carries the rest.

A counter-with-deliberate-failure variant (the canonical atomicity demonstration) is similar shape: a single counter object with `increment_and_fail()` exercises the rollback path; the post-failure read confirms the counter is unchanged.

## Where to next

- `doc/architecture.md` -- the tier model, daemons, boot sequence, and auto-inheritance chain this document builds on.
- `doc/substrate-primitives.md` -- the per-primitive foundation-and-proof statement (atomicity, persistence, hot reload, capability separation, the rest).
- `doc/lpc-essentials.md` -- LPC language orientation: types, type modifiers, inheritance, atomicity, `call_out`, error handling. The bridge to the formal spec at [LPC.md].
- `doc/kernel-libraries.md` -- inheritable libraries shipped under `src/lib/`: String / StringBuffer, KVstore, Iterator family, Continuation family, Time, and the small `/lib/util/` set.
- `doc/http-applications.md` -- the HTTP/1-specific application pattern with `examples/http-app/` as the runnable reference.
- `doc/operations.md` -- the operator's view (admin_console use, statedump cadence, rlimits, JIT deployment posture).

[LPC.md]: https://github.com/dworkin/lpc-doc/blob/master/LPC.md
