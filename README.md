<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# eOS-kernellib

eOS-kernellib is the kernel layer for orthogonally-persistent servers built on the [DGD] driver. It is the substrate above the driver and below an application -- providing capability tiers, daemons, and runtime primitives the application uses to express its own logic.

An orthogonally-persistent server treats in-memory state as the primary state of the system. Objects survive restart without explicit serialization; transactions roll back partial effects on failure; loaded code joins the running runtime under capability bounds. eOS-kernellib makes these properties available to the application above it.

## What it provides

- **Atomicity** -- Operations commit wholly or roll back wholly. Partial effects do not escape on failure.
- **Capability separation** -- Code runs under a capability tier that bounds what it can call.
- **Persistent state** -- The in-memory object graph survives restart without explicit serialization.
- **Hot reload** -- Code recompiles into the live runtime; existing objects update in place.
- **Sandboxed code load** -- New code compiles into the runtime under capability bounds set at load time.
- **Asynchronous events** -- Event delivery is atomic with the state change that produced it.
- **Multi-agent coherence** -- Multiple callers see a consistent view of state without user-land coordination.
- **State introspection** -- The state graph is queryable directly through runtime calls.

`doc/architecture.md` covers the architecture: capability tiers, daemons, boot sequence, auto-inheritance, System global-access, and host-driver extensions. `doc/substrate-primitives.md` covers each primitive's foundation, demonstration status, supporting extensions, and open work.

Treating these eight as substrate primitives is the architectural commitment of eOS-kernellib: each one is a runtime guarantee the application inherits rather than a pattern the application reimplements. An orthogonally-persistent server cannot fake them at the application layer — atomicity requires runtime cooperation with the transaction manager; persistence requires runtime cooperation with the storage manager; capability separation requires runtime cooperation with the access checks; hot reload requires runtime cooperation with the dispatcher. Asking the application to provide them is asking it to reproduce the runtime in user space.

## Getting started

`doc/getting-started.md` covers installing DGD, fetching this repository, building, and running a minimal configuration. `doc/building.md` is the deeper build reference.

## How it composes

eOS-kernellib sits between the [DGD] driver and the application built on top:

- **Below**: DGD provides the LPC runtime, the storage manager, and the atomic-commit boundary.
- **Above**: An application uses the kernel layer's capability tiers, daemons, and primitives to express its own logic.

The kernel layer is application-neutral. Long-running stateful workflows, customer-authored automation, durable memory for long-lived processes, agent-runtime harnesses, and other orthogonally-persistent-server use cases all build on the same substrate primitives.

## License

`LICENSE.md` carries the full text. Files in this repository are released under either the Unlicense or BSD-2-Clause-Patent; `LICENSE.md` identifies which applies where.

[DGD]: https://github.com/dworkin/dgd
