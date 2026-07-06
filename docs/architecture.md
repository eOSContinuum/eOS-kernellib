# Architecture

eOS-kernellib is the LPC kernel layer for orthogonally-persistent servers on the [DGD] driver. It sits between DGD and an application, providing the runtime primitives the application uses to express its own logic.

This document describes the architecture: capability tiers, the daemons that run at the kernel level, the boot sequence, the auto-inheritance pattern, the System global-access mechanism, the library modules shipped under `src/usr/`, the host-driver extension surface, and the points where an application plugs in. The per-primitive foundation-and-proof statement lives in `docs/runtime-primitives.md`, which also names the platform's commitment behind each primitive.

**Audience**: a developer or architect orienting to the platform's structural model; wants to know how the kernel layer is organized — what runs at which tier, which daemons mediate which surfaces, how the boot sequence reaches a steady state, where an application plugs in — before writing or operating code on the platform.

## Capability tiers

The kernel layer divides code into capability tiers that bound what each tier can call. The tier of a piece of code is determined by its file path and is enforced in kernel-tier LPC: the kernel auto's kfun wrappers route file and compile operations through the access daemon, and privileged surfaces check the `previous_program()` call chain (see `docs/glossary.md` on ambient authority). The host driver dispatches the masked kfuns but carries no tier policy of its own.

Five-tier vocabulary (canonical for cross-document precision):

| Tier | Location | Role |
|---|---|---|
| **A** | C code (host driver) | Parses, executes, manages atomicity / persistence / swap / call_outs. Not LPC; not part of eOS-kernellib's source. |
| **B** | `/kernel/` | Hard-trusted kernel-tier LPC. The driver hooks (`driver.c`, the user / access / resource daemons, the kernel auto, kernel API libraries). Hand-edited rarely. |
| **C** | `/usr/System/` | Privileged user-tier code, owner `System`; publishes `/usr/System` for global read at boot so every domain can inherit the System auto. The System auto (`/usr/System/lib/auto.c`) is the inheritance root for every user-tier object. |
| **D** | `/usr/HTTP/`, `/usr/TLS/`, `/usr/LPC/`, etc. | Shipped platform domains. Each domain has its own owner and is isolated from other user-tier code unless explicit cross-domain access is granted. These domains are shipped together with eOS-kernellib as the platform's distribution. |
| **E** | Application-supplied `/usr/<Domain>/` | Same user-tier mechanics as tier D — owner, access bits, inheritance chains — but distributed by the consuming application, not by eOS-kernellib. |

Three-tier vocabulary (coarse synonym used elsewhere in the kernel source): **Kernel** = tier B; **System** = tier C; **User** = tiers D and E. The five-tier provides more resolution where boundary discrimination matters; the three-tier groups tiers that share enforcement semantics.

Two implications matter for builders authoring on top of the kernel layer:

1. **What "is" the kernel layer** is tiers B + C + D. Tier A is the host driver; tier E is the consumer's responsibility.
2. **Tier E inherits the platform through tier C**. The System auto is the inheritance root for every user-tier object, regardless of whether that object lives in tier D or tier E. The platform's enforcement reaches tier E via the C-to-E inheritance path.

Adding a new tier-E domain means creating a subdirectory under `src/usr/` and an `initd.c` at its root. The System initd iterates `/usr/[A-Z]*/initd.c` at boot and compiles each domain under its tier.

### Path conventions inside a domain

Within each user-tier domain (tiers C, D, E), four subdirectories carry conventional meanings the host driver and the kernel libraries rely on:

- **`/lib/`** — inheritable / abstract library objects. Cannot be cloned. The driver object's `inherit_program` hook requires the inherited path to contain `/lib/`.
- **`/obj/`** — cloneable objects. The master at `/usr/X/obj/Y` is the class; clones have names like `/usr/X/obj/Y#37`. The master carries no instance data.
- **`/sys/`** — singleton daemons. Compiled at domain init; one master object per file; no clones.
- **`/data/`** — Light-Weight Objects (LWOs). Structured value types without separate identity; copied rather than shared when they cross dataspaces (consolidated treatment in `docs/code-lifecycle.md` LWO instantiation).

The same shape applies in `/kernel/` for kernel-tier code. These conventions are not just style; the driver enforces the `/lib/` requirement on inheritance, and the System auto's `find_object` discipline depends on path-based type recognition.

## Daemons

The kernel layer runs daemons at two tiers.

**Tier B (kernel) daemons** — instantiated by the driver during boot:

- **driver** (`src/kernel/sys/driver.c`) — the object DGD itself calls into for fundamental events (login, logout, error). Required by the driver's contract with the kernel.
- **access_daemon** (`src/kernel/sys/access_daemon.c`) — the access-control daemon. Manages file-system and command access for users; backs the host driver's per-call access checks.
- **resource_daemon** (`src/kernel/sys/resource_daemon.c`) — the resource daemon. Tracks resource consumption (file blocks, objects, stack, ticks) per owner; enforces limits via the host driver's rlimits mechanism.
- **userd** (`src/kernel/sys/userd.c`) — the kernel user daemon. Manages connection acceptors (telnet, binary, datagram) and the user objects bound to them.
- **capabilityd** (`src/kernel/sys/capabilityd.c`) — the capability store: the shared approved-set tables behind the platform's gating surfaces, checked through `is_allowed` / `require_member`. Compiled by the driver at boot, before any domain initd runs, so the surfaces that consult it find it seeded. See `docs/capability.md`.

The administrative shell `admin_console` (`src/kernel/lib/admin_console.c` and `src/kernel/obj/admin_console.c`) is the default command handler bound to privileged user logins. It exposes a command set for inspecting and modifying runtime state.

**Tier C (System) daemons** — compiled at the System domain's initd:

- **System userd** (`src/usr/System/sys/userd.c`) — registers as the telnet manager for the telnet port; routes connections to the configured user handlers.
- **objectd** (`src/usr/System/sys/objectd.c`) — records the compile-time program graph: per-path program issues and their include and inherit relationships, the record the upgrade daemon consults when driving recompile cascades. Per-owner object lists and object traversal are roadmap candidates (`docs/runtime-platform-roadmap.md`), not shipped capabilities.
- **errord** (`src/usr/System/sys/errord.c`) — error reporting and logging.
- **upgraded** (`src/usr/System/sys/upgraded.c`) — live code upgrade coordination. Drives `compile_object` cascades when a parent is recompiled.
- **http_server** (`src/usr/System/sys/http_server.c`) — registers as the binary-port manager; clones an application server at the kernel-defined mount point `/usr/WWW/obj/server` on each incoming HTTP/1 connection.

Adding tier-D or tier-E daemons follows the same pattern: a singleton under `/usr/<Domain>/sys/`, compiled by the domain's initd at boot. The shipped tier-D daemon that integrates structurally with the property primitive every user-tier object inherits is:

- **`/usr/Merry/sys/merry`** — the property-change dispatcher. Every `set_property` write on a host that inherits `/lib/util/properties` routes through `MERRY->dispatch_set` for pre/main/post observer firing, cascade-depth bounding, cycle detection, and implicit batching. The dispatcher reuses the Merry script-binding mechanism for observer source. See `docs/merry-applications.md` for the script-binding mechanism and `docs/dispatcher.md` for the dispatcher's full surface.

## Boot sequence

The kernel layer recognizes three boot modes.

### Cold boot

The host driver starts, reads the `.dgd` configuration file, and compiles `src/kernel/sys/driver.c` (the object DGD calls into for fundamental events), whose initialization compiles the path named by the config's `auto_object` (typically `/kernel/lib/auto`) and then the other tier-B objects in dependency order: the resource daemon, the access daemon, the kernel user daemon, the default admin console, and the capability store (`/kernel/sys/capabilityd`, compiled before any domain so the gating surfaces that consult it find it seeded). The driver then compiles `src/usr/System/initd.c`, which:

1. Publishes the boot-time global-read grants (`set_global_access` for `System` and the shipped structured-state domains — see System global access below).
2. Compiles the System-tier daemons and bootstrap objects (userd, objectd, errord, upgraded, logd, etc.).
3. Builds the domain list: a fixed dependency prefix (`TLS`, `HTTP`, `LPC`) followed by the remaining `get_dir("/usr/[A-Z]*")` directories, System excluded.
4. Walks that list twice: a first pass registers every domain owner (`add_owner`) so cross-domain inherits resolve regardless of load order, then a second pass compiles each domain's `initd.c`.
5. Loads `sys/http_server` last, after every domain initd, so the binary-port manager binds once the domains it may route to exist.

The boot completes when the System initd's `create()` returns. The host driver then accepts external connections on the ports declared in the config.

The property-layer hook in `/lib/util/properties` checks at every `set_property` call whether `/usr/Merry/sys/merry` is loaded; if so, the write routes through `MERRY->dispatch_set`, otherwise it falls through to `set_raw_property` directly. The check is per-call, so early-bootstrap code that runs before the Merry domain's initd compiles (the System-tier daemons in step 2, and the domains ahead of Merry in the step-3 order) sees direct writes; once Merry's initd has compiled the dispatcher daemon, subsequent writes flow through it. Hosts that need to bypass the dispatcher post-boot (raw schema initialization, fixture seeding) call `set_raw_property` directly.

### Statedump-restore boot

The host driver loads the snapshot file named by the config's `dump_file` and restores the in-memory object graph from it. No domain initds run; the objects, their owners, their access bits, and their internal state are present from the snapshot. The driver then resumes serving connections on the configured ports. Persistent state is the platform's default mode; cold boot is the recovery path when no snapshot is available.

### Hot boot

The host driver writes a snapshot, replaces the running executable via `execv` (e.g., to pick up a new DGD binary or change a config value), reloads the snapshot, and continues serving the same persistent connections that were open before the replace. The connection file descriptors survive the `execv` via POSIX fd inheritance; the host runtime serializes per-connection state during hotboot and restores it on the receiving side. The kernel driver's `restored(int hotboot)` hook distinguishes hotboot-resume from statedump-resume so userd and initd can re-attach to the surviving connections. Hot boot enables platform-level deployment without disconnecting active sessions; cold boot is the fallback when the snapshot is unrecoverable.

Statedump cadence is governed by the config's `dump_interval`. Statedumps occur between timeslices, never inside an atomic operation; the runtime guarantees that the snapshot represents a consistent state-graph commit boundary.

## Auto-inheritance pattern

Every compiled LPC source automatically inherits the auto object named by the `.dgd` config's `auto_object` line. The auto-inheritance is implicit; an LPC file does not declare it. The compile-time effect: every object's master inherits the auto's variables, functions, and access checks.

The kernel layer ships two autos that compose:

- **Kernel auto** at `/kernel/lib/auto.c` — the host's `auto_object` setting points here. Provides per-object owner identity, the driver-facing kfun wrappers, and the kernel-tier access checks. Inherited by every compiled object regardless of tier.
- **System auto** at `/usr/System/lib/auto.c` — inherits the kernel auto. Adds the System-tier API surface that user-tier objects need: `find_object` semantics that hide `/obj/` masters from user-tier callers, the principal-management helpers, the cross-domain access discipline. Inherited by every user-tier object (tiers C, D, E).

User-tier objects pick up the System auto transitively: their `auto_object` resolves to the kernel auto, which is extended by the System auto via the inheritance chain rooted at System's `lib/auto.c`. The chain is structural; an application cannot opt out.

The kernel auto also defines the inheritance discipline: a class declared `inherit "/<path>"` must have `/lib/` in its path. The driver object's `inherit_program` hook (`src/kernel/sys/driver.c`) enforces this. The discipline implies that cloneable objects (`/obj/`) and singleton daemons (`/sys/`) cannot be inherited; only `/lib/` and `/kernel/lib/` objects can.

### Lifecycle dispatch

The host driver invokes a configured "create" hook on every object's first function call. The `.dgd` config's `create` line names the hook; eOS-kernellib uses `_F_create`. The hook is implemented in the kernel auto at `src/kernel/lib/auto.c`:

```c
nomask void _F_create()
{
    if (!creator) {
        /* set creator and owner from object_name */
        /* if this is a clone, register with the driver's clone manager */
        /* call System-tier creator if defined, otherwise call create() */
        create();
    }
}
```

The `if (!creator)` guard ensures the hook fires once per object: at first call after compile, owner and creator identity are set from the object's path, the object is registered with the appropriate manager (clone manager for clones; the upgraded daemon for masters when an upgrade is in progress), and finally the object's own `create()` function is invoked. Application code writes `create()`; the kernel auto wires the dispatch.

A clone's `create()` runs in its own dataspace at clone time. The master's `create()` runs once when the program is first compiled. The kernel auto handles the master/clone distinction via its registration logic, so most application authors write a single `create()` body that runs on both master and clones.

## System global access

Two distinct mechanisms give the platform its cross-domain read model:

1. **System-tier privilege is ambient.** The kernel auto's access checks exempt System-creator (and kernel-tier) code, so System-tier objects can inherit user-tier libraries from any domain, look up objects in any domain via `find_object`, and compile objects on behalf of any user-tier domain via `compile_object` — no per-domain grant involved.
2. **`set_global_access` publishes a domain for reading.** An LPC function on the access daemon (`src/kernel/sys/access_daemon.c`, exposed through the kernel access API and the admin console), it marks the *named domain's own* `/usr/<dir>` tree readable by every user — the inverse of a reach-out grant. At cold boot the System initd publishes seven domains this way (`System`, `XML`, `Schema`, `Marshal`, `Index`, `Vault`, `Merry` — `src/usr/System/initd.c`): publishing `/usr/System` is what lets every domain inherit the System auto, and publishing the structured-state domains is what lets tier-D/E code read those shipped libraries without per-call mediation.

Beyond those published trees, a tier-D or tier-E domain reads another domain only through a specific grant made via the access daemon. The platform's discipline: System mediates; user tiers consume.

The System auto inherits this reach automatically. Because every user-tier object inherits `/usr/System/lib/auto.c`, every user-tier object has structural access to the System-defined API surface — without each user domain having to be granted its own cross-domain access. The System auto is the cross-tier integration point.

## Modules under src/usr/

The kernel layer ships platform domains (tier D) under `src/usr/`:

- **`src/usr/HTTP/`** — HTTP/1 client and server library objects (`api/obj/server1.c`, `api/obj/client1.c`) and their TLS variants. The kernel ships these as libraries; binding the HTTP/1 server on the binary port is handled by `src/usr/System/sys/http_server.c`.
- **`src/usr/TLS/`** — TLS 1.3 record layer, handshake, and extension support. Gated on the host being built with `KF_SECURE_RANDOM`; otherwise compiled-but-inert.
- **`src/usr/LPC/`** — shared LPC utilities (parsers, AST helpers, and a full LPC compiler in LPC, currently unconsumed) used by other modules and available for application-side use.
- **`src/usr/Merry/`** — the Merry script-binding subsystem and property-change dispatcher. The compile pipeline turns Merry source strings into light-weight `/usr/Merry/data/merry` wrappers with MD5-named programs at `/usr/Merry/merry/<md5>`; the `find_merry` / `run_merry` family in `/usr/Merry/lib/merryapi` looks scripts up by ancestry walk over `query_parent()`; the dispatcher daemon at `/usr/Merry/sys/merry` routes `set_property` writes through registered observers at pre/main/post timings. See `docs/merry-applications.md` for the application surface and `docs/dispatcher.md` for the dispatcher.
- **`src/usr/Schema/`** — the schema daemon and node objects: declared state shapes (node names, member types, callbacks) that the structured-persistence pipeline validates against. See `docs/schema.md`.
- **`src/usr/Vault/`** — structured-object persistence: per-node XML export and import of schema-declared state. See `docs/vault-applications.md`.
- **`src/usr/Marshal/`** — the state import/export binding between property tables and the XML transport, used by the Vault pipeline.
- **`src/usr/XML/`** — XML parse and generate transport (element/pcdata/samref LWO wrappers) consumed by the Vault layout. See `docs/xml.md`.
- **`src/usr/Index/`** — the logical-name registry daemon: name-to-object bindings consumed by Schema, Vault, and the console's clone addressing.
- **`src/usr/MerryApp/`** — the reference Merry application (a schema-registered clonable with script slots plus its boot-time test driver); the same tree the `examples/merry-app` walkthrough documents.

`src/usr/System/` (tier C) is the kernel-tier-adjacent privileged domain described under Daemons above.

The System initd's iteration auto-discovers and loads any other domains a builder adds under `src/usr/<Name>/initd.c`. A new tier-E domain follows the same conventions: `initd.c` at the root; `lib/`, `obj/`, `sys/`, `data/` subdirectories as needed.

## Host-driver extensions (tier A)

The host driver (tier A, C code) supports dlopen-loaded extension modules registered in the `.dgd` configuration's `modules` mapping. Extension modules add kfuns to the runtime; the LPC layer above sees them as additional built-ins.

```text
modules = ([
    "/path/to/some-extension.1.5" : ([ ... ]),
])
```

The extension surface is intentionally separate from the host driver's own kfun set. The driver ships a small minimalist core (capped at 256 kfuns by the 1-byte kfun numbering) covering object lifecycle, compilation, atomicity, connections, and basic math / strings / arrays. Functionality that requires C-level access beyond that core — hardware-accelerated crypto, AOT compilation, native regex, system-database integration — is added at runtime via this mechanism rather than by growing the core.

The ecosystem provides extension bundles. The canonical one is [dworkin/lpc-ext], which includes modules such as an AOT-compiling JIT for performance, a regex kfun, a TLS-primitive kfun, and others. eOS-kernellib's runtime platform requires no extension and loads none; deployments choose what to load based on their needs.

An extension you load today is one your statedump binds to: a snapshot taken with an extension active will require that same extension to restore. Removing an extension means losing state. That makes extension loading a durable architectural commitment, not an opt-in convenience. See `docs/operations.md` for deployment-time guidance on loading and managing extensions, including the open empirical questions about how extension-loaded codepaths interact with the platform's atomicity and hot-reload guarantees.

Extension kfuns sit alongside built-in kfuns at the host-driver level, with the same per-tier access checks. An LPC file calling some kfun cannot tell from the call shape whether the kfun is a host built-in or a dlopen-loaded extension; the deployment's `.dgd` config determines which kfuns are present.

## Runtime primitives

The kernel layer surfaces eight runtime primitives:

- **Atomicity** — operations commit wholly or roll back wholly.
- **Capability separation** — code runs under one tier that bounds what it can call.
- **Persistent state** — the in-memory object graph survives restart without explicit serialization.
- **Hot reload** — `compile_object` against an existing path updates behavior in place.
- **Sandboxed code load** — `compile_object` against a new path adds code under the capability tier set by its path.
- **Asynchronous events** — events fire as part of the atomic commit that produced them.
- **Multi-agent coherence** — multiple callers see a consistent view of state without user-land synchronization.
- **State introspection** — the state graph is queryable directly through host driver primitives.

Each primitive's foundation, demonstration status, supporting extensions, and open work are documented in `docs/runtime-primitives.md`. The reference there is the authoritative per-primitive statement; this section is a quick index.

## Where an application plugs in

An application built on the kernel layer adds:

1. A tier-E domain under `src/usr/<App>/` with an `initd.c` that compiles the domain's objects at boot.
2. An `auto.c` library (optional) inherited by the domain's objects to declare common API.
3. Connection handlers (optional) bound to ports in the `.dgd` configuration that are not already bound by the kernel.

The kernel daemons (driver, access_daemon, resource_daemon, userd, capabilityd) require no modification. The application registers its objects, hooks into events, and consumes the runtime primitives.

For HTTP-based applications, the kernel's HTTP/1 server is already bound on the binary port. The kernel-defined mount point for the application's per-connection server is `/usr/WWW/obj/server` — `src/usr/System/sys/http_server.c` looks up that path at every incoming connection and, if present, clones it. `docs/http-applications.md` walks through writing the application server; `examples/http-app/` is a runnable reference.

For non-HTTP applications, the patterns are covered in `docs/application-authoring.md`. The LPC language itself is covered in `docs/lpc-essentials.md` (an orientation that bridges the reader into [LPC.md], the formal language spec). The inheritable libraries shipped under `src/lib/` — string buffers, persistent collections, iterators, async continuations, time — are catalogued in `docs/kernel-libraries.md`. Operational concerns (admin_console use, statedump cadence, rlimits configuration, JIT deployment posture) are covered in `docs/operations.md`.

## Where to next

- `docs/runtime-primitives.md` — per-primitive foundation, demonstration, and status statement for the eight runtime guarantees the architecture surfaces.
- `docs/persistence.md` — the full orthogonal-persistence story (statedump cycle, hot boot mechanics, save_object semantics, boundaries).
- `docs/code-lifecycle.md` — compile, clone, destruct, recompile, and the object-manager event surface in detail.
- `docs/operations.md` — the operator-facing deployment surface (`.dgd` configuration, boot modes, extensions).
- `docs/application-authoring.md` — writing tier-E applications on top of this architecture.

[LPC.md]: https://github.com/dworkin/lpc-doc/blob/master/LPC.md

[DGD]: https://github.com/dworkin/dgd
[dworkin/lpc-ext]: https://github.com/dworkin/lpc-ext
