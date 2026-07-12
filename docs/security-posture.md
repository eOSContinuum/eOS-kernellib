# Security posture

The platform's security boundaries in one place: what the tiers protect, what the operator is responsible for, and what the platform does not defend against. For the authority mechanism in depth, read `docs/capability.md`. For how to report a vulnerability, see `SECURITY.md`.

**Audience**: an operator or evaluator who needs the platform's trust boundaries and their own operational responsibilities before deploying.

## Trust boundaries

The platform's authority spine is the capability tier model. A file's tier follows its path and bounds what it may call, enforced in kernel-tier LPC (`docs/architecture.md` Capability tiers, `docs/capability.md`).

- **Kernel (tier B, `/kernel/`) and System (tier C, `/usr/System/`)** are the trusted base. Application code reaches a driver primitive only through a mediating `/kernel/` object that checks the caller first; System-tier code carries the privileged API surface that user-tier domains inherit.
- **Platform and application domains (both under `/usr/<Domain>/`, tiers D and E)** are each isolated from every other domain. A domain reads another only through a specific grant or a boot-time global-read publication (`docs/architecture.md` System global access).
- **Untrusted scripts** run in the Merry sandbox, a restricted LPC dialect that cannot call arbitrary kfuns (`docs/merry-language.md`).

Two boundaries carry more weight than their surface suggests:

- **The admin console is host-shell-equivalent.** An operator who reaches the console and can run `code` evaluates arbitrary LPC at that operator's tier; for `admin`, that reaches every kfun in the runtime. Treat console access as equivalent to shell access on the platform's process (`docs/admin-console.md` Console security posture).
- **A loaded host-driver extension is trusted native code.** It adds kfuns at the host-driver level, subject to the same per-tier access checks as built-ins, and it binds into the statedump. Loading one is a durable architectural commitment, not an opt-in convenience (`docs/operations.md` Loading host-driver extensions).

## What the platform enforces

- **Tier and owner mediation.** Three kernel-tier mechanisms enforce authority in parallel: the access daemon checks every file and compile operation (the kernel auto routes them there), the resource daemon enforces per-owner rlimits, and the capability library backs six gating surfaces, five through one shared store and membership check plus one fixed-principal gate (`docs/capability.md`).
- **Per-call access checks.** A `code` invocation hits a per-call access check at every privileged kfun boundary (file, compile, connection, and resource operations), so a non-admin operator cannot exceed its owner's grants through the console (`docs/admin-console.md` Console security posture).
- **Atomic isolation (opt-in).** An operation declared `atomic`, or run inside an atomic envelope, commits wholly or rolls back wholly on error (`docs/runtime-primitives.md` Atomicity). Atomicity is per-function, not ambient: a non-atomic operation that errors can leave partial state, and even an atomic one is unverified through an extension-compiled codepath (see Non-goals).

## What the operator is responsible for

The platform enforces the authority model; the deployment enforces the perimeter. Before exposing a platform:

- **Bind the console to a private interface.** `telnet_port` speaks plain telnet, so the wire carries the operator password and every command in clear text. Bind it to loopback or a maintenance network and reach it through an SSH or TLS tunnel (`docs/operations.md` Network boundary and transport security).
- **Terminate application TLS at a reverse proxy.** The shipped posture is a reverse proxy terminating TLS in front of the platform and forwarding cleartext HTTP to `binary_port` on the loopback (`docs/operations.md`).
- **Protect the state files.** The snapshot, the swap file, and `src/kernel/data/` (the admin password hash and access grants) hold the entire application state and the platform credentials. Run the platform as a dedicated unprivileged user, keep the files readable only by that user, and protect backups the same way (`docs/operations.md` State file locations and permissions).
- **Guard the admin credential.** The admin password is set on first console connection and written via `save_object` to `/kernel/data/admin.pwd`, independent of the image dump (`docs/runtime-primitives.md` persistence). Anyone who can reach the console and authenticate holds host-shell-equivalent authority. It cannot be rotated in place: resetting it means a cold boot from a pre-credential snapshot or from scratch, which loses accumulated state (`docs/admin-console.md`).

## Non-goals and known limits

- **This is not strict object-capability.** Authority is ambient (tier, owner, and call chain), not carried by an unforgeable held reference. LPC alone has no revocation or per-reference attenuation, and pure-LPC caretaker patterns are bypassable once the underlying object is reachable by name (`docs/capability.md` What "capability" means here).
- **The console wire is unencrypted.** Confidentiality of a console session depends on the tunnel around it, not on the platform.
- **Extension-loaded behavior is unverified against two primitives.** Whether atomicity and hot reload hold through an extension-compiled codepath is an open empirical question (`docs/operations.md` Open empirical questions). An operator who loads an extension in production owns that risk.
- **The Merry sandbox is a language restriction, not a separate process.** It bounds what a script may call; it is not an operating-system isolation boundary.
- **Only Merry source is sandboxed.** Plain LPC loaded through `compile_object` runs at the loading object's tier, unsandboxed. An application that exposes a compile path (a `POST /compile` route, say) runs that code at its own tier; bounded loading of arbitrary plain LPC is not yet available (`docs/runtime-primitives.md`).
- **This is not a hardened multi-tenant boundary.** Peer domains are separated by discretionary access control inside one process, not an isolation barrier, and the single coherence domain means one runaway domain is a shared-fate risk to the others. The containment story for untrusted code is the Merry sandbox, not domain separation.

## Where to next

- [`docs/capability.md`](capability.md): the authority mechanism, the six gating surfaces, and the object-capability limitation.
- [`docs/operations.md`](operations.md): the deployment perimeter (network boundary, state-file permissions, extension loading).
- [`docs/admin-console.md`](admin-console.md): the console's security posture and the weight of console access.
- [`docs/architecture.md`](architecture.md): the capability tiers and the System global-access model.
- [`SECURITY.md`](../SECURITY.md): how to report a vulnerability, and the reporting scope.
