<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Kernel Layer

eOS-kernellib is the LPC kernel layer for orthogonally-persistent servers on the [DGD] driver. It sits between DGD and an application, providing the substrate primitives the application uses to express its own logic.

This document describes what the kernel layer provides: the capability tiers, the daemons that run at the kernel level, the library modules shipped under `src/usr/`, and the points where an application plugs in. The architectural argument behind these choices -- why these primitives belong in the substrate rather than the application layer -- lives at [eOS-DeepContext].

## Capability tiers

The kernel layer divides code into capability tiers that bound what each tier can call. The tier of a piece of code is determined by its file path and is enforced by the driver:

- **Kernel** -- code under `src/kernel/`. The driver, the access daemon, the resource daemon, the user daemon, and the kernel-level libraries the other tiers inherit. Has unrestricted access to driver primitives.
- **System** -- code under `src/usr/System/`. The administrative shell entry, the HTTP/1 server bootstrap, the System user daemon, the upgrade server, and other system-level objects. Authorized to make system-only kernel calls (the `SYSTEM()` macro check).
- **User** -- application code under user-layer domains in `src/usr/` other than `System`. Constrained: cannot call kernel daemons directly except through declared API surfaces.

Adding a new user-layer domain means creating a subdirectory under `src/usr/` and an `initd.c` at its root. The System initd iterates `/usr/[A-Z]*/initd.c` at boot and compiles each domain under its tier.

## Daemons

Four daemons run at the kernel tier. They are instantiated by the driver during boot:

- **driver** (`src/kernel/sys/driver.c`) -- the object DGD itself calls into for fundamental events (login, logout, error). Required by the driver's contract with the kernel.
- **access_daemon** (`src/kernel/sys/access_daemon.c`) -- the access-control daemon. Manages file-system and command access for users.
- **resource_daemon** (`src/kernel/sys/resource_daemon.c`) -- the resource daemon. Tracks resource consumption (file blocks, objects, stack, ticks) per owner; enforces limits.
- **userd** (`src/kernel/sys/userd.c`) -- the user daemon. Manages connections (telnet, binary, datagram) and the user objects bound to them.

The administrative shell `admin_console` (`src/kernel/lib/admin_console.c` and `src/kernel/obj/admin_console.c`) is the default command handler bound to privileged user logins. It exposes a command set for inspecting and modifying runtime state.

System-tier objects under `src/usr/System/` extend the kernel: the System userd registers as the telnet manager, the HTTP/1 server bootstrap registers as the binary manager, and the objectd, errord, and upgraded daemons handle object tracking, error reporting, and live code upgrade.

## Modules under src/usr/

The kernel layer ships library modules under `src/usr/`:

- **`src/usr/HTTP/`** -- HTTP/1 client and server library objects (`api/obj/server1.c`, `api/obj/client1.c`) and their TLS variants. The kernel ships these as libraries; binding the HTTP/1 server on the binary port is handled by `src/usr/System/sys/http_server.c`.
- **`src/usr/TLS/`** -- TLS support for HTTP/1 connections.
- **`src/usr/LPC/`** -- shared LPC utilities used by other modules.
- **`src/usr/System/`** -- the system-tier daemons and bootstrap objects.

The System initd's iteration auto-discovers and loads any other domains a builder adds under `src/usr/<Name>/initd.c`.

## Substrate primitives

The kernel layer surfaces eight runtime primitives:

### Atomicity

Operations commit wholly or roll back wholly. DGD provides the atomic-commit boundary; the kernel layer exposes it. If an operation raises an error before completing, the runtime restores in-memory state to the snapshot taken at the start of the operation. Side effects -- writes to other objects, resource consumption -- roll back atomically.

### Capability separation

Code runs under one of the three tiers above. The driver enforces tier boundaries at every cross-tier call. Application code that calls a kernel-level function directly is rejected at compile time. The access daemon mediates intentional cross-tier access through declared APIs.

### Persistent state

The in-memory object graph survives restart without explicit serialization. The runtime writes the graph to the configured `dump_file` at each `dump_interval` checkpoint; on boot, it restores the graph from the last snapshot. Application code does not implement save/load logic for its state.

### Hot reload

A `compile_object()` call against an existing object's path recompiles the source and updates the object's behavior in place. Existing instances inherit the new code on next dispatch. State held in instance variables survives the recompile.

### Sandboxed code load

A `compile_object()` call against a new path adds the new object to the runtime under the capability tier set by its path. The new code joins the running runtime without restart. The object's tier is determined by where the path resolves; the runtime cannot be tricked into elevating tier through a load-time argument.

### Asynchronous events

Events fire as part of the atomic commit that produced them. A receiver that errors during event handling rolls the producer's commit back along with its own.

### Multi-agent coherence

Multiple callers see a consistent view of state. Each thread of execution proceeds against the same snapshot until it commits or rolls back. There is no user-land synchronization protocol; the runtime serializes commits.

### State introspection

The state graph is queryable directly through driver primitives (`status()`, `find_object()`, `object_name()`, `query_owner()`, and related calls). The runtime does not require an application-layer query API for inspecting object identity, ownership, or memory shape.

## Where an application plugs in

An application built on eOS-kernellib adds:

1. A user-layer domain under `src/usr/<App>/` with an `initd.c` that compiles the domain's objects at boot.
2. An `auto.c` library (optional) inherited by the domain's objects to declare common API.
3. Connection handlers (optional) bound to ports in the `.dgd` configuration that are not already bound by the kernel.

The kernel daemons (driver, access_daemon, resource_daemon, userd) require no modification. The application registers its objects, hooks into events, and consumes the substrate primitives.

For HTTP-based applications, the kernel's HTTP/1 server is already bound on the binary port. The kernel-defined mount point for the application's per-connection server is `/usr/WWW/obj/server` -- `src/usr/System/sys/http_server.c` looks up that path at every incoming connection and, if present, clones it. `doc/HTTP-APPLICATIONS.md` walks through writing the application server; `examples/http-app/` is a runnable reference.

[DGD]: https://github.com/dworkin/dgd
[eOS-DeepContext]: https://github.com/eOSContinuum/eOS-DeepContext
