<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Architecture

eOS-kernellib is the LPC kernel layer for orthogonally-persistent servers on the [DGD] driver. It sits between DGD and an application, providing the substrate primitives the application uses to express its own logic.

This document describes the architecture: capability tiers, the daemons that run at the kernel level, the boot sequence, the auto-inheritance pattern, the System global-access mechanism, the library modules shipped under `src/usr/`, the host-driver extension surface, and the points where an application plugs in. The architectural argument behind these choices -- why these primitives belong in the substrate rather than the application layer -- lives at [eOS-DeepContext]. The per-primitive foundation-and-proof statement lives in `doc/substrate-primitives.md`.

## Capability tiers

The kernel layer divides code into capability tiers that bound what each tier can call. The tier of a piece of code is determined by its file path and is enforced by the host driver's access checks at every kfun call.

Five-tier vocabulary (canonical for cross-document precision):

| Tier | Location | Role |
|---|---|---|
| **A** | C code (host driver) | Parses, executes, manages atomicity / persistence / swap / call_outs. Not LPC; not part of eOS-kernellib's source. |
| **B** | `/kernel/` | Hard-trusted kernel-tier LPC. The driver hooks (`driver.c`, the user / access / resource daemons, the kernel auto, kernel API libraries). Hand-edited rarely. |
| **C** | `/usr/System/` | Privileged user-tier code, owner `System`, with `set_global_access("System", TRUE)`. The System auto (`/usr/System/lib/auto.c`) is the inheritance root for every user-tier object. |
| **D** | `/usr/HTTP/`, `/usr/TLS/`, `/usr/LPC/`, etc. | Shipped substrate domains. Each domain has its own owner and is isolated from other user-tier code unless explicit cross-domain access is granted. These domains are shipped together with eOS-kernellib as the substrate's distribution. |
| **E** | Application-supplied `/usr/<Domain>/` | Same user-tier mechanics as tier D -- owner, access bits, inheritance chains -- but distributed by the consuming application, not by eOS-kernellib. |

Three-tier vocabulary (coarse synonym used elsewhere in the kernel source): **Kernel** = tier B; **System** = tier C; **User** = tiers D and E. The five-tier provides more resolution where boundary discrimination matters; the three-tier groups tiers that share enforcement semantics.

Two implications matter for builders authoring on top of the kernel layer:

1. **What "is" the kernel layer** is tiers B + C + D. Tier A is the host driver; tier E is the consumer's responsibility.
2. **Tier E inherits the substrate through tier C**. The System auto is the inheritance root for every user-tier object, regardless of whether that object lives in tier D or tier E. The substrate's enforcement reaches tier E via the C-to-E inheritance path.

Adding a new tier-E domain means creating a subdirectory under `src/usr/` and an `initd.c` at its root. The System initd iterates `/usr/[A-Z]*/initd.c` at boot and compiles each domain under its tier.

### Path conventions inside a domain

Within each user-tier domain (tiers C, D, E), four subdirectories carry conventional meanings the host driver and the kernel libraries rely on:

- **`/lib/`** -- inheritable / abstract library objects. Cannot be cloned. The driver's `inherit_program` kfun requires the inherited path to contain `/lib/`.
- **`/obj/`** -- cloneable objects. The master at `/usr/X/obj/Y` is the class; clones have names like `/usr/X/obj/Y#37`. The master carries no instance data.
- **`/sys/`** -- singleton daemons. Compiled at domain init; one master object per file; no clones.
- **`/data/`** -- Light-Weight Objects (LWOs). Structured value types without separate identity; pass-by-value semantics.

The same shape applies in `/kernel/` for kernel-tier code. These conventions are not just style; the driver enforces the `/lib/` requirement on inheritance, and the System auto's `find_object` discipline depends on path-based type recognition.

## Daemons

The kernel layer runs daemons at two tiers.

**Tier B (kernel) daemons** -- instantiated by the driver during boot:

- **driver** (`src/kernel/sys/driver.c`) -- the object DGD itself calls into for fundamental events (login, logout, error). Required by the driver's contract with the kernel.
- **access_daemon** (`src/kernel/sys/access_daemon.c`) -- the access-control daemon. Manages file-system and command access for users; backs the host driver's per-call access checks.
- **resource_daemon** (`src/kernel/sys/resource_daemon.c`) -- the resource daemon. Tracks resource consumption (file blocks, objects, stack, ticks) per owner; enforces limits via the host driver's rlimits mechanism.
- **userd** (`src/kernel/sys/userd.c`) -- the kernel user daemon. Manages connection acceptors (telnet, binary, datagram) and the user objects bound to them.

The administrative shell `admin_console` (`src/kernel/lib/admin_console.c` and `src/kernel/obj/admin_console.c`) is the default command handler bound to privileged user logins. It exposes a command set for inspecting and modifying runtime state.

**Tier C (System) daemons** -- compiled at the System domain's initd:

- **System userd** (`src/usr/System/sys/userd.c`) -- registers as the telnet manager for the telnet port; routes connections to the configured user handlers.
- **objectd** (`src/usr/System/sys/objectd.c`) -- tracks per-owner object lists; supports object traversal for auditing.
- **errord** (`src/usr/System/sys/errord.c`) -- error reporting and logging.
- **upgraded** (`src/usr/System/sys/upgraded.c`) -- live code upgrade coordination. Drives `compile_object` cascades when a parent is recompiled.
- **http_server** (`src/usr/System/sys/http_server.c`) -- registers as the binary-port manager; clones an application server at the kernel-defined mount point `/usr/WWW/obj/server` on each incoming HTTP/1 connection.

Adding tier-D or tier-E daemons follows the same pattern: a singleton under `/usr/<Domain>/sys/`, compiled by the domain's initd at boot.

## Boot sequence

The kernel layer recognizes three boot modes.

**Cold boot.** The host driver starts, reads the `.dgd` configuration file, and compiles the path named by the config's `auto_object` (typically `/kernel/lib/auto`). The driver then compiles `src/kernel/sys/driver.c` (the object DGD will call into for fundamental events) and the three other tier-B daemons in dependency order. The driver then compiles `src/usr/System/initd.c`, which:

1. Grants System global access (`set_global_access("System", TRUE)`).
2. Compiles the System-tier daemons and bootstrap objects (userd, objectd, errord, upgraded, http_server, etc.).
3. Iterates `get_dir("/usr/[A-Z]*")` to discover user-tier domains (tiers D and E in alphabetical order).
4. For each discovered domain, calls `add_owner(<Domain>)` and compiles the domain's `initd.c`.

The boot completes when the System initd's `create()` returns. The host driver then accepts external connections on the ports declared in the config.

**Statedump-restore boot.** The host driver loads the snapshot file named by the config's `dump_file` and restores the in-memory object graph from it. No domain initds run; the objects, their owners, their access bits, and their internal state are present from the snapshot. The driver then resumes serving connections on the configured ports. Persistent state is the substrate's default mode; cold boot is the recovery path when no snapshot is available.

**Hot boot.** The host driver writes a snapshot, replaces the running executable via `execv` (e.g., to pick up a new DGD binary or change a config value), reloads the snapshot, and continues serving the same persistent connections that were open before the replace. The connection file descriptors survive the `execv` via POSIX fd inheritance; the host runtime serializes per-connection state during hotboot and restores it on the receiving side. The kernel driver's `restored(int hotboot)` hook distinguishes hotboot-resume from statedump-resume so userd and initd can re-attach to the surviving connections. Hot boot enables substrate-level deployment without disconnecting active sessions; cold boot is the fallback when the snapshot is unrecoverable.

Statedump cadence is governed by the config's `dump_interval`. Statedumps occur between timeslices, never inside an atomic operation; the runtime guarantees that the snapshot represents a consistent state-graph commit boundary.

## Auto-inheritance pattern

Every compiled LPC source automatically inherits the auto object named by the `.dgd` config's `auto_object` line. The auto-inheritance is implicit; an LPC file does not declare it. The compile-time effect: every object's master inherits the auto's variables, functions, and access checks.

The kernel layer ships two autos that compose:

- **Kernel auto** at `/kernel/lib/auto.c` -- the host's `auto_object` setting points here. Provides per-object owner identity, the driver-facing kfun wrappers, and the kernel-tier access checks. Inherited by every compiled object regardless of tier.
- **System auto** at `/usr/System/lib/auto.c` -- inherits the kernel auto. Adds the System-tier API surface that user-tier objects need: `find_object` semantics that hide `/obj/` masters from user-tier callers, the principal-management helpers, the cross-domain access discipline. Inherited by every user-tier object (tiers C, D, E).

User-tier objects pick up the System auto transitively: their `auto_object` resolves to the kernel auto, which is extended by the System auto via the inheritance chain rooted at System's `lib/auto.c`. The chain is structural; an application cannot opt out.

The kernel auto also defines the inheritance discipline: a class declared `inherit "/<path>"` must have `/lib/` in its path. The driver's `inherit_program` kfun (kernel/sys/driver.c) enforces this. The discipline implies that cloneable objects (`/obj/`) and singleton daemons (`/sys/`) cannot be inherited; only `/lib/` and `/kernel/lib/` objects can.

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

The System tier (tier C) is the only user-tier domain authorized to reach across user-domain boundaries by default. The mechanism is two-part:

1. **The `set_global_access` kfun** -- a host-driver primitive (kfun) that grants the named domain cross-domain read access to all other user-tier domains.
2. **The System initd grants itself global access at boot** -- `set_global_access("System", TRUE)` runs early in the System initd's `create()` body.

Once granted, System-tier objects can:

- Inherit user-tier libraries from any domain.
- Look up objects in any domain via `find_object`.
- Compile objects on behalf of any user-tier domain via `compile_object`.

Other user-tier domains do not get global access by default. Cross-domain access from tier D or tier E is granted per-call via the access_daemon, not blanket-granted via `set_global_access`. The substrate's discipline: System mediates; user tiers consume.

The System auto inherits this reach automatically. Because every user-tier object inherits `/usr/System/lib/auto.c`, every user-tier object has structural access to the System-defined API surface -- without each user domain having to be granted its own cross-domain access. The System auto is the cross-tier integration point.

## Modules under src/usr/

The kernel layer ships substrate domains (tier D) under `src/usr/`:

- **`src/usr/HTTP/`** -- HTTP/1 client and server library objects (`api/obj/server1.c`, `api/obj/client1.c`) and their TLS variants. The kernel ships these as libraries; binding the HTTP/1 server on the binary port is handled by `src/usr/System/sys/http_server.c`.
- **`src/usr/TLS/`** -- TLS 1.3 record layer, handshake, and extension support. Gated on the host being built with `KF_SECURE_RANDOM`; otherwise compiled-but-inert.
- **`src/usr/LPC/`** -- shared LPC utilities (parsers, AST helpers, and a full LPC compiler in LPC, currently unconsumed) used by other modules and available for application-side use.

`src/usr/System/` (tier C) is the kernel-tier-adjacent privileged domain described under Daemons above.

The System initd's iteration auto-discovers and loads any other domains a builder adds under `src/usr/<Name>/initd.c`. A new tier-E domain follows the same conventions: `initd.c` at the root; `lib/`, `obj/`, `sys/`, `data/` subdirectories as needed.

## Host-driver extensions (tier A)

The host driver (tier A, C code) supports dlopen-loaded extension modules registered in the `.dgd` configuration's `modules` mapping. Extension modules add kfuns to the runtime; the LPC layer above sees them as additional built-ins.

```
modules = ([
    "/path/to/some-extension.1.5" : ([ ... ]),
])
```

The extension surface is intentionally separate from the host driver's own kfun set. The driver ships a small minimalist core (capped at 256 kfuns by the 1-byte kfun numbering) covering object lifecycle, compilation, atomicity, connections, and basic math / strings / arrays. Functionality that requires C-level access beyond that core -- hardware-accelerated crypto, AOT compilation, native regex, system-database integration -- is added at runtime via this mechanism rather than by growing the core.

The ecosystem provides extension bundles. The canonical one is [dworkin/lpc-ext], which includes modules such as an AOT-compiling JIT for performance, a regex kfun, a TLS-primitive kfun, and others. eOS-kernellib's substrate requires no extension and loads none; deployments choose what to load based on their needs.

An extension you load today is one your statedump binds to: a snapshot taken with an extension active will require that same extension to restore. Removing an extension means losing state. That makes extension loading a durable architectural commitment, not an opt-in convenience. See `doc/operations.md` for deployment-time guidance on loading and managing extensions, including the open empirical questions about how extension-loaded codepaths interact with the substrate's atomicity and hot-reload guarantees.

Extension kfuns sit alongside built-in kfuns at the host-driver level, with the same per-tier access checks. An LPC file calling some kfun cannot tell from the call shape whether the kfun is a host built-in or a dlopen-loaded extension; the deployment's `.dgd` config determines which kfuns are present.

## Substrate primitives

The kernel layer surfaces eight runtime primitives:

- **Atomicity** -- operations commit wholly or roll back wholly.
- **Capability separation** -- code runs under one tier that bounds what it can call.
- **Persistent state** -- the in-memory object graph survives restart without explicit serialization.
- **Hot reload** -- `compile_object` against an existing path updates behavior in place.
- **Sandboxed code load** -- `compile_object` against a new path adds code under the capability tier set by its path.
- **Asynchronous events** -- events fire as part of the atomic commit that produced them.
- **Multi-agent coherence** -- multiple callers see a consistent view of state without user-land synchronization.
- **State introspection** -- the state graph is queryable directly through host driver primitives.

Each primitive's foundation, demonstration status, supporting extensions, and open work are documented in `doc/substrate-primitives.md`. The reference there is the authoritative per-primitive statement; this section is a quick index.

## Where an application plugs in

An application built on the kernel layer adds:

1. A tier-E domain under `src/usr/<App>/` with an `initd.c` that compiles the domain's objects at boot.
2. An `auto.c` library (optional) inherited by the domain's objects to declare common API.
3. Connection handlers (optional) bound to ports in the `.dgd` configuration that are not already bound by the kernel.

The kernel daemons (driver, access_daemon, resource_daemon, userd) require no modification. The application registers its objects, hooks into events, and consumes the substrate primitives.

For HTTP-based applications, the kernel's HTTP/1 server is already bound on the binary port. The kernel-defined mount point for the application's per-connection server is `/usr/WWW/obj/server` -- `src/usr/System/sys/http_server.c` looks up that path at every incoming connection and, if present, clones it. `doc/http-applications.md` walks through writing the application server; `examples/http-app/` is a runnable reference.

For non-HTTP applications, the patterns are covered in `doc/application-authoring.md`. The LPC language itself is covered in `doc/lpc-essentials.md` (an orientation that bridges the reader into [LPC.md], the formal language spec). The inheritable libraries shipped under `src/lib/` -- string buffers, persistent collections, iterators, async continuations, time -- are catalogued in `doc/kernel-libraries.md`. Operational concerns (admin_console use, statedump cadence, rlimits configuration, JIT deployment posture) are covered in `doc/operations.md`.

[LPC.md]: https://github.com/dworkin/lpc-doc/blob/master/LPC.md

[DGD]: https://github.com/dworkin/dgd
[eOS-DeepContext]: https://github.com/eOSContinuum/eOS-DeepContext
[dworkin/lpc-ext]: https://github.com/dworkin/lpc-ext
