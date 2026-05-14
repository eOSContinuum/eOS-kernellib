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

`doc/architecture.md` covers the architecture: capability tiers, daemons, boot sequence, auto-inheritance, System global-access, and host-driver extensions. `doc/substrate-primitives.md` covers each primitive's foundation, demonstration status, supporting extensions, and open work. The architectural argument for treating these properties as substrate primitives -- rather than as application-layer patterns or external glue -- lives at [eOS-DeepContext].

## Getting started

`doc/getting-started.md` covers installing DGD, fetching this repository, building, and running a minimal configuration. `doc/building.md` is the deeper build reference.

## How it composes

eOS-kernellib sits between the [DGD] driver and the application built on top:

- **Below**: DGD provides the LPC runtime, the storage manager, and the atomic-commit boundary.
- **Above**: An application uses the kernel layer's capability tiers, daemons, and primitives to express its own logic.

eos-harness is one example of an application built on eOS-kernellib -- an agent-runtime harness that exercises the substrate primitives directly. Other applications -- long-running stateful workflows, customer-authored automation, durable memory for stateful long-lived processes, and other orthogonally-persistent-server use cases -- can build on the same kernel layer.

## License

`LICENSE.md` carries the full text. Files in this repository are released under either the Unlicense or BSD-2-Clause-Patent; `LICENSE.md` identifies which applies where.

[DGD]: https://github.com/dworkin/dgd
[eOS-DeepContext]: https://github.com/eOSContinuum/eOS-DeepContext
