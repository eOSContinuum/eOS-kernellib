# Evaluating the platform

One page for the fit decision: what the platform proves today, its measured envelope and ceilings, where it does not fit, and the adoption risks to price before building on it. Every claim here restates a depth doc and links it; nothing on this page is the authoritative statement of anything.

**Audience**: a decider with a limited evaluation budget, before the depth docs. The reading path at the end orders the deeper material by how much budget remains.

## Fit and anti-fit

The platform is a fit when the workload wants what the runtime guarantees: long-lived stateful objects that survive restart without serialization code, mutations that commit atomically or roll back wholly, code hot-reloaded into the live image, reactions fired synchronously with the state change that caused them, and multiple actors reading coherent state inside one process (`runtime-primitives.md`).

It is the wrong platform, by design and not by immaturity, when any of these holds (`coming-from-contemporary-infrastructure.md` What does not translate; `../README.md` Where it does not fit):

- The workload needs **horizontal scale-out or multi-machine redundancy**. Single coherence domain: one process on one machine.
- The workload needs **more than 255 concurrent connections** on a stock driver build, or a working set beyond the stock ceilings below.
- The team needs **polyglot code inside the state domain**. LPC (and the Merry dialect) is the in-image language; everything else integrates at the transport boundary as a client.
- The workload needs **declarative cross-entity queries**. There is no query planner over the image; enumeration and indexing are application structures.
- The workload needs **multi-core CPU parallelism inside the state domain**. Exactly one task runs in the image at any instant, to completion; more cores do not parallelize in-image work, and added concurrency buys queueing, not parallelism (`execution-model.md` Run to completion; measured in `operations.md` Limits and capacity).

## What is proven today

`runtime-primitives.md` states per-primitive status honestly rather than claiming the set wholesale: of the eight primitives, three are **Validated** (atomicity, persistent state, hot reload) and five are **Partial** (capability separation, sandboxed code load, asynchronous events, multi-agent coherence, state introspection) -- foundation present, demonstration incomplete. Its At-a-glance table carries a one-command proof per primitive; the fastest single check is:

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh merry-app
```

which exercises persistence across a real dump-and-restart cycle among its assertions. The whole surface is provable in one sitting: the Full regression sweep in `scripts/README.md` runs every example and smoke in about fifteen minutes end to end on the measured-baseline hardware, with the crypto-gated steps documented skips on a module-less build. The roadmap (`runtime-platform-roadmap.md`) commits forward surfaces on named activation triggers, not dates; What ships today is its authoritative shipped inventory.

## The measured envelope

One machine, one workload shape, a rig and a datum rather than a guarantee (`operations.md` Limits and capacity, which names the hardware and the rig):

- Snapshot pause stayed at or under 0.12 s from a 2 MB through a 237 MB image; cold boot and a 237 MB restore both reach console-ready in about 0.1 s.
- The bundled HTTP example answered about 1,600 sequential one-connection-per-request requests per second cleartext, and about 470 over native TLS 1.3 (median handshake roughly 1.5 ms).
- Concurrency has been measured once at moderate scale: aggregate throughput saturates at the same level across the measured client counts -- added concurrency buys queueing, not parallelism -- and the head-of-line worst case under a saturated queue is bounded by the tick budget, clearing immediately after the burst (`operations.md` Limits and capacity carries the numbers). Sustained behavior near the ceilings remains unmeasured.

## The ceilings

Stock-build compiled bounds, not tuning knobs (`operations.md` Limits and capacity for the full table and which rows have config headroom): 255 users and 32767 `array_size` are already at the stock ceiling; `objects` has headroom to 65535 and `call_outs` to 65534; the swap device caps at 65535 sectors, about 64 MiB of pageable object storage at the demo config's 1 KiB `sector_size`, scaling only through `sector_size`; LPC `int` is 32-bit signed; the per-execution tick budget defaults to 20,000,000. A driver rebuilt with wider index types raises the index ceilings; the platform runs against a stock build.

## Adoption risks, priced

- **The language.** In-image code is LPC, edited as C files. `lpc-essentials.md` is the bridge; it is a small language, but it is not one your team knows. The ramp is short and sequenced: `lpc-essentials.md` plus the three tutorials take a developer from a fresh boot to their own persistent HTTP endpoint (`first-hour.md` is an hour by its own clock; `first-application.md` and `first-http-endpoint.md` complete the chain).
- **The tooling.** No language server, no step debugger, no formatter; navigation is `rg` plus the source map, and the console's introspection verbs stand in for a debugger. Tests are boot-time sentinel drivers asserted by an external script, and CI is the same harness run headless (`debugging-applications.md` The working environment, plainly).
- **The exit cost.** Three paths export state to portable form today (`save_object` text, Vault+Schema XML, the property-table ascii marshal); a typed object graph with no schema and no property-table shape has no export walker (`persistence.md` Getting data out).
- **The durability model.** Persistence is periodic statedump, not per-transaction commit: the recovery point is the operator-chosen `dump_interval`, a sizing decision rather than a guarantee, and an unclean stop loses everything since the last completed dump (`operations.md` Availability and data-loss model).
- **The driver dependency.** The runtime driver is an unmodified AGPL-3.0 upstream with a single primary maintainer; the license boundary and the pin-plus-fork continuity posture are stated factually in `architecture.md` The driver dependency.
- **The security envelope.** Trust boundaries, the operator's responsibilities, and the non-goals (including what the capability model does not claim) are consolidated in `security-posture.md` -- among them the native TLS stack's assurance status: a from-scratch interpreted-LPC implementation, unaudited, with the reverse proxy as the higher-assurance alternative.

## Spending the rest of the budget

In order, as budget allows: `runtime-primitives.md` At a glance (run a proof beside it), `operations.md` Limits and capacity, `security-posture.md`, `runtime-platform-roadmap.md` for the ships-today-versus-next boundary, `coming-from-contemporary-infrastructure.md` for what the platform replaces, and `debugging-applications.md` The working environment, plainly for the team's day-to-day.
