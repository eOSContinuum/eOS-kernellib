# Evaluating the platform

One page for the fit decision: what the platform proves today, its measured envelope and ceilings, where it does not fit, and the adoption risks to price before building on it. Every claim here restates a depth doc and links it; nothing on this page is the authoritative statement of anything.

**Audience**: a decider with a limited evaluation budget, before the depth docs. The reading path at the end orders the deeper material by how much budget remains.

## Fit and anti-fit

The platform is a fit when the workload wants what the runtime guarantees: long-lived stateful objects that survive restart without serialization code, mutations that commit atomically or roll back wholly, code hot-reloaded into the live image, reactions fired synchronously with the state change that caused them, and multiple actors reading coherent state inside one process ([`runtime-primitives.md`](runtime-primitives.md)).

It is the wrong platform, by design and not by immaturity, when any of these holds ([`coming-from-contemporary-infrastructure.md`](coming-from-contemporary-infrastructure.md#what-does-not-translate) What does not translate; [`../README.md`](../README.md#where-it-does-not-fit) Where it does not fit):

- The workload needs **horizontal scale-out or multi-machine redundancy**. Single coherence domain: one process on one machine.
- The workload needs **more than 255 concurrent connections** on a stock driver build, or a working set beyond the stock ceilings below. The cap counts concurrent streams, not registered users -- what holds a slot, and what a pooling proxy does and does not buy, is [`operations.md`](operations.md#connection-slot-economics) Connection-slot economics.
- The team needs **polyglot code inside the state domain**. LPC (and the Merry dialect) is the in-image language; everything else integrates at the transport boundary as a client.
- The workload needs **declarative cross-entity queries**. There is no query planner over the image; enumeration and indexing are application structures.
- The workload needs **multi-core CPU parallelism inside the state domain**. Exactly one task runs in the image at any instant, to completion; more cores do not parallelize in-image work, and added concurrency buys queueing, not parallelism ([`execution-model.md`](execution-model.md#run-to-completion) Run to completion; measured in [`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity).
- The workload is **CPU-bound computation in-image**. In-image code is interpreted LPC under a per-task tick budget: a full default budget is roughly 90-120 ms of wall clock on the measured hardware, and every task holds the single serialization point while it runs ([`execution-model.md`](execution-model.md#the-price-head-of-line-latency) The price). Compute-heavy stages -- analytics passes, media transforms, heavy parsing -- belong in a client at the transport boundary, or in a host-driver kfun extension with its documented open questions ([`operations.md`](operations.md#loading-host-driver-extensions) Loading host-driver extensions).

## From your system to the nearest example

Start from the example nearest the system you are picturing. Each runs in one command (`scripts/run-example.sh <name>`), except [`http-app`](../examples/http-app) and [`https-app`](../examples/https-app), which verify with live curl probes per their own READMEs:

| You are picturing | Start from |
|---|---|
| An HTTP service with a health endpoint | [`examples/http-app`](../examples/http-app) (its TLS twin: [`examples/https-app`](../examples/https-app)) |
| A full authenticated service: users, sessions, agents, live event streams | [`examples/composite-app`](../examples/composite-app) ([`composite-applications.md`](composite-applications.md) walks it in stages) |
| Passkey/WebAuthn ceremony mechanics alone | [`examples/webauthn-app`](../examples/webauthn-app) |
| Agents acting on a person's behalf | [`examples/agent-app`](../examples/agent-app) |
| Rooms, users, and capability-gated admin actions | [`examples/chat-app`](../examples/chat-app) |
| Reactions that commit or roll back with the state change | [`examples/signal-app`](../examples/signal-app) |
| Sandboxed scripting over platform state | [`examples/merry-app`](../examples/merry-app) |
| Typed records persisted to exportable on-disk XML | [`examples/vault-app`](../examples/vault-app) |
| Live code upgrade on a running system | [`examples/hot-reload-demo`](../examples/hot-reload-demo), [`examples/hot-reload-master`](../examples/hot-reload-master), [`examples/upgrade-cascade`](../examples/upgrade-cascade) |
| The atomic rollback guarantee in its smallest form | [`examples/atomic-demo`](../examples/atomic-demo) |

## What is proven today

[`runtime-primitives.md`](runtime-primitives.md) states per-primitive status honestly rather than claiming the set wholesale: each of the eight primitives is either **Validated** or **Partial** (foundation present, demonstration incomplete), and its At-a-glance table is the authoritative split -- the counts are deliberately not restated here, so they cannot drift. The shape of the split, for a decider who stops at this page: the fully-validated tier today is the persistence core -- atomicity, persistent state, and hot reload -- while the capability, sandbox, events, coherence, and introspection primitives have their foundations present with demonstrations still incomplete; each phrase here matches the per-primitive Status line the table links. The same table carries a one-command proof per primitive; the fastest single check is:

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh merry-app
```

which exercises persistence across a real dump-and-restart cycle among its assertions. The whole surface is provable in one sitting: the Full regression sweep in [`scripts/README.md`](../scripts/README.md) runs every example and smoke in about fifteen minutes end to end on the measured-baseline hardware, with the crypto-gated steps documented skips on a module-less build. The roadmap ([`runtime-platform-roadmap.md`](runtime-platform-roadmap.md#what-ships-today)) commits forward surfaces on named activation triggers, not dates; What ships today is its authoritative shipped inventory.

## The measured envelope

One machine, one workload shape, a rig and a datum rather than a guarantee ([`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity, which names the hardware and the rig):

- Snapshot pause ran 0.003-0.037 s from a 2.4 MB through a 506 MB image at the stock `sector_size`, and, with `sector_size` raised, reached 1.233 s at a 1.08 GB image; restore boot reaches console-ready in 0.06 s across that same range, and cold boot in roughly 0.1 s.
- The bundled HTTP example answered 1,425-1,637 sequential one-connection-per-request requests per second cleartext, and about 470 over native TLS 1.3 (median handshake roughly 1.5 ms).
- Concurrency has been measured twice, at moderate scale and again up to the 255-connection cap: aggregate throughput saturates at the same level across the measured client counts -- added concurrency buys queueing, not parallelism -- and the head-of-line worst case under a saturated queue is bounded by the tick budget, clearing immediately after the burst ([`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity carries the numbers). Sustained behavior near the ceilings remains unmeasured.
- A state-touching workload has been measured once: sequential authenticated inventory writes through the composite example -- bearer-token validation, a persistent daemon mutation, and the synchronous audit observer per request -- ran at about 970 requests per second (median 1.0 ms), against about 2,100 for the same boot's zero-work health route; and driver resident memory at the snapshot-pause steps ran several times the on-disk image, 7 MB to 5.3 GB over 2.4 MB to 506 MB snapshots at the stock `sector_size` ([`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity carries both).

## The ceilings

Stock-build compiled bounds, not tuning knobs ([`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity for the full table and which rows have config headroom): 255 users and 32767 `array_size` are already at the stock ceiling; `objects` has headroom to 65535 and `call_outs` to 65534; the swap device caps at 65535 sectors, about 64 MiB of pageable object storage at the demo config's 1 KiB `sector_size`, scaling only through `sector_size` -- whose own compiled range tops out at 65535 bytes, so total persistent state on a stock build tops out just under 4 GiB; LPC `int` is 32-bit signed; the per-execution tick budget defaults to 20,000,000. A driver rebuilt with wider index types raises the index ceilings; the platform runs against a stock build.

## Adoption risks, priced

- **The language.** In-image code is LPC, edited as C files. [`lpc-essentials.md`](lpc-essentials.md) is the bridge; it is a small language, but it is not one your team knows. To price the language by eye before committing to any tutorial, read one production-shape file cold: [`examples/composite-app/Inventory/sys/inventoryd.c`](../examples/composite-app/Inventory/sys/inventoryd.c), a persistent, observer-bearing, capability-gated daemon that fits in one sitting. The ramp itself is short and sequenced: [`lpc-essentials.md`](lpc-essentials.md) plus the three tutorials take a developer from a fresh boot to their own persistent HTTP endpoint ([`first-hour.md`](first-hour.md) is an hour by its own clock; [`first-application.md`](first-application.md) and [`first-http-endpoint.md`](first-http-endpoint.md) complete the chain).
- **The tooling.** No language server, no step debugger, no formatter; navigation is `rg` plus the source map, and the console's introspection verbs stand in for a debugger. Tests are boot-time sentinel drivers asserted by an external script, and CI is the same harness run headless ([`debugging-applications.md`](debugging-applications.md#the-working-environment-plainly) The working environment, plainly).
- **The ecosystem.** No package manager, no third-party library corpus, no registry: the in-image code available to an application is what this repository ships plus what its team writes, and external software integrates at the transport boundary as clients and servers, never as an import. What a service stack solves by adding a dependency, this platform solves with code you own -- price the build-versus-integrate line with that in view.
- **The exit cost.** Three paths export state to portable form today (`save_object` text, Vault+Schema XML, the property-table ascii marshal); a typed object graph with no schema and no property-table shape has no export walker ([`persistence.md`](persistence.md#getting-data-out) Getting data out).
- **The durability model.** Persistence is periodic statedump, not per-transaction commit: the recovery point is the operator-chosen `dump_interval`, a sizing decision rather than a guarantee, and an unclean stop loses everything since the last completed dump -- including writes the application already acknowledged to its clients ([`operations.md`](operations.md#availability-and-data-loss-model) Availability and data-loss model, which also prices the recurring dump-pause and the recovery-time shape). For a write that must be durable at acknowledge time, the options and their prices: trigger a statedump on the critical path (`dump_state`, the same call the `snapshot` verb makes -- costs the measured pause per acknowledged batch); write that record to a host file at the edge, the way the platform file-backs its own credentials and access bits so they survive without a snapshot -- costs a second representation for exactly that record; or keep the system of record external and treat the image as the working set -- costs re-importing the store-versus-cache split the platform removed. None is free; the honest default is sizing `dump_interval` to the loss the business can absorb. The in-platform mechanics of the first two options -- including the acknowledgment-ordering trap -- are the worked recipe ([`common-tasks.md`](common-tasks.md#make-one-write-durable-at-acknowledge-time) Make one write durable at acknowledge time).
- **The driver dependency.** The runtime driver is an unmodified AGPL-3.0 upstream with a single primary maintainer; the license boundary and the pin-plus-fork continuity posture are stated factually in [`architecture.md`](architecture.md#the-driver-dependency) The driver dependency. The fork in that posture is today a tracking mirror, not a staffed maintenance fork, so a sustained upstream pause leaves driver fixes to the adopter -- carried in-house against the pinned source, or funded.
- **The kernel layer's own maintenance posture.** The symmetric fact for this repository: eOS-kernellib has a single primary maintainer, review of external contributions is best-effort with no committed turnaround, and CI reruns the module-less regression bar on a pull request while the module-bearing steps lean on the PR's own evidence ([`CONTRIBUTING.md`](../CONTRIBUTING.md#maintenance) Maintenance). What bounds the risk if maintenance pauses: the BSD-2-Clause-Patent license permits carrying a fork without negotiation, and the regression harness ([`scripts/README.md`](../scripts/README.md)) defines what such a fork must keep passing. Facts to price, not commitments.
- **The security envelope.** Trust boundaries, the operator's responsibilities, and the non-goals (including what the capability model does not claim) are consolidated in [`security-posture.md`](security-posture.md) -- among them the native TLS stack's assurance status: a from-scratch interpreted-LPC implementation, unaudited, with the reverse proxy as the higher-assurance alternative.

## Spending the rest of the budget

In order, as budget allows: [`runtime-primitives.md`](runtime-primitives.md) At a glance (run a proof beside it), [`configuration.md`](configuration.md#limits-and-capacity) Limits and capacity, [`security-posture.md`](security-posture.md), [`runtime-platform-roadmap.md`](runtime-platform-roadmap.md) for the ships-today-versus-next boundary, [`coming-from-contemporary-infrastructure.md`](coming-from-contemporary-infrastructure.md) for what the platform replaces, and [`debugging-applications.md`](debugging-applications.md#the-working-environment-plainly) The working environment, plainly for the team's day-to-day.
