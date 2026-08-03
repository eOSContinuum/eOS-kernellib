# Configuration and capacity

This document is the configuration-and-capacity reference: what each `.dgd` field means, what the platform's hard ceilings are, the production-shape starting point, and what has been measured against those ceilings. It does not cover procedures -- booting, backing up, monitoring, and the rest of the operator's task surface live in `docs/operations.md`.

**Audience**: someone choosing config values, sizing a workload's storage shape, or raising a ceiling. Operating a running deployment -- booting, backing up, monitoring, diagnosing failures -- is covered in `docs/operations.md`.

## The .dgd configuration file

The host driver reads its configuration from a `.dgd` file passed on the command line at boot. The fields below cover the operator-facing decisions. See the upstream DGD reference at <https://github.com/dworkin/dgd> for the full set.

| Field | Role |
|---|---|
| `directory` | The platform's source root. The driver compiles LPC files relative to this directory and chroots its filesystem-style operations to it |
| `auto_object` | Path of the auto-inherited object. eOS-kernellib uses `/kernel/lib/auto` |
| `driver_object` | Path of the driver object. eOS-kernellib uses `/kernel/sys/driver` |
| `create` | Name of the driver-side create dispatcher. eOS-kernellib uses `_F_create` |
| `include_file` | Path of the standard include file. eOS-kernellib uses `/include/std.h` |
| `include_dirs` | Search path for `#include` directives |
| `telnet_port` | Address-and-port mapping for the telnet listener (admin_console binds here) |
| `binary_port` | Address-and-port mapping for binary listeners (HTTP and other application transports bind here) |
| `swap_file`, `swap_size`, `swap_fragment`, `sector_size` | Swap parameters governing how the platform pages objects to disk |
| `static_chunk`, `dynamic_chunk` | Memory allocator chunk sizes |
| `dump_file` | Path the platform writes snapshots to |
| `dump_interval` | Seconds between automatic snapshots. 3600 (one hour) is a reasonable default |
| `hotboot` | Tuple of `({ binary, config, snapshot, snapshot.old })` enabling hot boot via `execv` (see `docs/architecture.md` boot sequence) |
| `typechecking` | Strictness of compile-time type checks. Production deployments should set `2` (full) |
| `users`, `editors`, `objects`, `call_outs`, `array_size` | Hard caps on platform-wide resource counts |
| `modules` | Optional mapping of host-driver extensions to load at boot (see `docs/operations.md` Loading host-driver extensions) |

A minimal example is included at `example.dgd` in the repository root.

## Resource limits

Platform-wide caps live in the `.dgd` file: `users`, `editors`, `objects`, `call_outs`, `array_size`. These are hard ceilings. The platform refuses operations that would exceed them.

Per-owner limits are managed by the resource daemon at `/kernel/sys/resource_daemon` (registered as `resource_daemon` in the driver). Each owner has a quota covering object count, call_out count, ticks consumed per call, and stack depth. The admin_console `quota` and `rsrc` verbs read and write these. Per-owner ticks are charged on the owner's account when their code runs. An owner that exhausts ticks gets a runtime error and rollback rather than a hung platform.

The property-change dispatcher (`docs/dispatcher.md`) exposes a runtime-configurable cascade-depth bound via `MERRY->set_max_cascade_depth(int n)` (KERNEL-gated) and `MERRY->query_max_cascade_depth()` (public read-only). Default is `32`. The bound counts depth, not breadth: a flat batched write with many keys does not increment the counter. An observer-triggered chain of further writes does. Hitting the bound throws `merry: cascade depth N exceeded ...` and records `cascade-aborted` in the dispatcher's batch-status log.

The admin verb `cascade-depth [N]` (see `docs/admin-console.md` Dispatcher operator surface) is the operator-facing read/write surface. The no-arg form reports the current value, the integer-arg form sets it via the registry's KERNEL-elevation helper. The dispatcher exposes nine operator verbs from the admin console in total. `cascade-depth` and `dispatch-trace` are covered above, and the remaining seven cover runtime inspection and mutation of dispatcher state (observers, batch-status, observer-registration, approved-registrar set). `docs/admin-console.md` enumerates the full set and the worked-example operator session.

## Limits and capacity

`example.dgd`'s numbers are demo-scale, not sizing guidance. Read against the driver's own compiled bounds (`dworkin/dgd` `src/config.cpp`'s config-field range table and `src/config.h`'s index-type defaults):

| `.dgd` field | `example.dgd` | Driver's compiled range (stock build) | Reading |
|---|---|---|---|
| `array_size` | 32767 | 1-32767 (`USHRT_MAX / 2`) | Already at the driver's ceiling: raising it needs a driver built with a wider array-size type, not a config edit |
| `users` | 255 | 0-255 (`EINDEX_MAX`, a one-byte count) | Already at the stock build's ceiling, for the same reason |
| `editors` | 10 | 0-255 (`EINDEX_MAX`) | Demo-scale: real deployments have headroom to 255 with no rebuild |
| `objects` | 10000 | 2-65535 (`UINDEX_MAX`) | Demo-scale: headroom to 65535 with no rebuild |
| `call_outs` | 10000 | 0-65534 (`CINDEX_MAX - 1`) | Demo-scale: headroom to 65534 with no rebuild |
| `swap_size` × `sector_size` | 65535 × 1024 bytes | `swap_size` 1024-65535 (the sector-index cap); `sector_size` 512-65535 | About 64 MiB of pageable object storage, sized to the example's tiny working set rather than a production footprint |

The stock driver build's index widths (matching the driver's own header comment: "default: 64K objects, 64K swap sectors, 255 users, max string length 64K") set the ceilings above. A driver rebuilt with wider `uindex`/`eindex` types raises them, at the cost of a larger per-object memory footprint. eOS-kernellib runs against a stock build, so the table above is the practical ceiling until that changes.

The two ranges in the `swap_size` × `sector_size` row compound into the platform's absolute state ceiling: 65535 sectors times the 65535-byte maximum `sector_size` is just under 4.0 GiB of total persistent object storage on a stock build -- the most state the platform can hold at any configuration, however much RAM the host has (RAM sizes the resident set; the swap device bounds total state). Beyond that, the fix is a driver rebuilt with a wider sector index, not a config edit; the operational sequence for a deployment approaching this ceiling -- what to spend first, and the export-based terminal move -- is `docs/operations.md` When the image approaches the state ceiling.

Ceilings that are not `.dgd` fields:

| Ceiling | Value | Source |
|---|---|---|
| Host driver's core kfun set | Capped at 256, by the 1-byte kfun numbering | `docs/architecture.md` Host-driver extensions |
| Per-execution tick budget | 20,000,000 ticks, default | Set at boot in `src/kernel/sys/driver.c`. Raised or lowered per owner via `quota <owner> ticks <limit>` (Resource limits above) |
| LPC `int` width | 32-bit signed | `docs/lpc-essentials.md` Types and values |
| Application time horizon | Epoch seconds in a 32-bit signed int roll over 2038-01-19 03:14:07 UTC | `time()` (kfun), `Time`/`GMTime` (`docs/kernel-libraries.md` Time) |

**The 2038 time horizon.** `time()` and the `Time`/`GMTime` library both hold epoch seconds in an LPC `int`, so on a stock build they carry the signed-32-bit ceiling above: past 2038-01-19 03:14:07 UTC the value wraps negative, the same boundary as any other 32-bit signed Unix timestamp. This is an application-layer exposure, not necessarily a driver-internal one -- the driver's own `call_out` scheduling keeps its due-time bookkeeping in an unsigned 32-bit field with a later (2106) wraparound, so a `call_out` armed before 2038 does not inherit the signed overflow through that internal path. What does inherit it is any value an application reads from `time()`, stores as a timestamp, or carries into a `call_out` argument for its own deadline arithmetic (the lateness-detection pattern in `docs/persistence.md` Time) -- a stored deadline past the rollover compares incorrectly once `time()` itself has gone negative. A driver rebuilt with a wider `LPCint` (the `LARGENUM` build option) removes the ceiling; on a stock build, an application that must carry deadlines past 2038 needs to encode them outside the native epoch-seconds `int` (a wider stored representation, or an offset from a fixed epoch than 1970).

**Sizing a workload.** Which storage shape holds N records, and which ceiling binds first. One constant the tables above do not carry: **a single mapping caps at 32,767 key-value pairs** on a stock build -- the `array_size` knob governs mappings as well as arrays, and exceeding it raises `"Mapping too large"` at construction or `"Mapping too large to grow"` on assignment past the cap (verified against the driver source; the same knob is already at its driver ceiling, so there is no config headroom). The planned two-level mapping is the roadmap's answer for one logical mapping beyond that bound (`docs/runtime-platform-roadmap.md` Wave 3). Connection capacity is its own sizing axis: what a `users` slot actually counts (concurrent streams, not registered users) and how a fronting proxy changes the arithmetic is `docs/operations.md` Connection-slot economics.

| Shape | First-binding ceiling | 10^5 records, stock build | 10^6 records |
|---|---|---|---|
| One clone per record | The `objects` table (65535, shared with every platform and application object) | No | No |
| One `mapping` | 32,767 key-value pairs per mapping | No | No |
| `/lib/Array` (integer-indexed) | Structurally 32767^2 elements; in practice the holder's single dataspace -- the swap device must hold it and the snapshot writes it every cycle | Yes | Yes, with the swap sized for the dataspace |
| `/lib/KVstore` (string keys) | The `objects` table again, through node clones: roughly N/(fan-out/2) leaves plus a thin interior layer; per-node arrays bound fan-out by `array_size` | Yes (fan-out 100: ~1,000-2,000 nodes) | Yes (fan-out 100: ~10,000-20,000 nodes; fan-out 1,000: ~1,000-2,000) |

The residency profiles differ more than the counts: a mapping or an `Array` lives in one dataspace that pages in and out as a unit and contributes its full size to every snapshot, while a `KVstore` pages at node granularity (an access faults in the touched nodes, not the whole set) at the price of `objects`-table slots. `docs/kernel-libraries.md` Choosing a collection carries the author-facing decision rule; the wider-index driver rebuild that would raise the object ceiling is observed to segfault naively (`docs/building.md`), so treat 65535 objects as the practical bound.

**A production-shape starting point.** The sizing-relevant fields of a non-demo `.dgd`, each set against the tables above -- splice into a copy of `example.dgd` (these lines alone are not a bootable config), and treat it as a starting point, not a guarantee:

```text
users           = 255;      /* stock ceiling (one-byte count) */
editors         = 255;      /* headroom to the same cap; demo ships 10 */
array_size      = 32767;    /* driver ceiling; also caps each mapping at 32767 pairs */
objects         = 65535;    /* demo ships 10000; stock ceiling */
call_outs       = 65534;    /* demo ships 10000; stock ceiling */
swap_size       = 65535;    /* the sector-count cap: capacity scales through sector_size */
sector_size     = 16384;    /* 65535 x 16 KiB = ~1 GiB pageable object storage */
dump_file       = "../state/snapshot";   /* rotation writes <dump_file>.old beside it */
dump_interval   = 3600;     /* the data-loss window on snapshot restore */
```

Raising `sector_size` takes effect at the next boot (the swap file is per-boot scratch, rebuilt empty each start), and a restore boot accepts the new value. `users` and `array_size` are already at stock ceilings; the rest have the headroom shown in the field table above.

**Snapshot-pause scaling, measured across two sector sizes.** The dump-time pause scales with in-memory image size, not with the config caps above ("a multi-gigabyte image can take seconds to write; the runtime briefly blocks during the dump", `docs/persistence.md` The statedump cycle). Measured 2026-07-12 and 2026-08-01/02 on an Apple M5 Max (macOS 26.5, arm64, local NVMe) with `scripts/measure-baseline.py`; every figure below is a single-run sample, not averaged.

| `sector_size` | Image size | Client-observed pause | Resident memory (RSS) |
|---|---|---|---|
| 8192 (stock) | 2.4 MB (base) | 0.003-0.037 s across the run | 7 MB |
| 8192 | 36 MB | within that range | 112 MB |
| 8192 | 103 MB | 0.125 s (outlier) | 462 MB |
| 8192 | 237 MB | within that range | 1,402 MB |
| 8192 | 372 MB | within that range | 3,013 MB |
| 8192 | 506 MB | within that range | 5,297 MB |
| 32768 (`MEASURE_SECTOR_SIZE`) | 244 MB | 0.021 s | not measured |
| 32768 | 513 MB | 0.541 s | not measured |
| 32768 | 1,083 MB (1.08 GB) | 1.233 s | not measured |

The stock-`sector_size` run's per-step pause is not broken out beyond the 0.003-0.037 s range and the 103 MB outlier, and is not monotonic across steps (filesystem caching and swap-file growth dominate at these sizes); the RSS column is broken out per step and runs several times the on-disk image, because the dynamic allocator arena grows in chunks ahead of use (the 506 MB step's own status line showed a 4.8 GB arena at 11% used). Raising `sector_size` to 32768 reaches farther -- the 1.233 s point at 1.08 GB is the upstream expectation above ("a multi-gigabyte image can take seconds to write") landing on this hardware just past a gigabyte. The two runs' ~512 MB-scale points disagree across sector sizes (0.037 s at `sector_size` 8192 vs 0.541 s at `sector_size` 32768, both single runs, unexplained) -- an open observation, not a theory.

| Metric | Value |
|---|---|
| Cold boot to console-ready | 0.08-0.18 s |
| Restore-boot readiness (237 MB through 1.08 GB snapshots) | 0.06 s (state pages in on demand after readiness) |
| Cleartext `GET /health`, one-connection-per-request | 1,425-1,637 req/s |
| TLS 1.3 `GET /health`, one-connection-per-request (self-signed cert via `tls-cert reload`) | about 470 req/s, median handshake roughly 1.5 ms -- about a third of the cleartext rate, the expected cost of terminating TLS in interpreted LPC with a cheap handshake |

Two capacity facts the rig surfaced: the stock build caps `swap_size` at 65535 sectors, so swap capacity scales only through `sector_size`; and an image that outgrows the swap device dies with a fatal `out of sectors` error. One machine, one workload shape, several consistent single runs: a rig and a growing set of data points, not a guarantee.

**Concurrency, measured twice.** Same rig (`--concurrent`, `--headline` in `scripts/measure-baseline.py`), same machine.

| Run | Clients | Aggregate throughput (req/s) | Median latency | p95 latency |
|---|---|---|---|---|
| 2026-07-19 | 2 | 2,346 | 0.8 ms | not measured |
| 2026-07-19 | 8 | 2,386 | 3.2 ms | not measured |
| 2026-07-19 | 32 | 2,391 | 12.9 ms | not measured |
| 2026-08-01 | 32 | 2,311 | 13.5 ms | 13.9 ms |
| 2026-08-01 | 64 | 2,340 | 26.0 ms | 27.3 ms |
| 2026-08-01 | 128 | 2,200 | 31.1 ms | 52.1 ms |
| 2026-08-01 | 255 (the connection-slot cap) | 2,268 | 58.1 ms | 100.2 ms |

Aggregate throughput saturates around the same level across every measured client count -- the signature of one serialization point servicing sub-millisecond requests: added concurrency buys queueing, not parallelism. Every request in the 255-connection run was served at the full 255-connection cap. The head-of-line worst case, measured directly: with the driver saturated by back-to-back near-budget busy tasks (about 88 ms each on this hardware; a full default 20,000,000-tick budget is roughly 90-120 ms of driver wall), a probing client's median `/health` latency rose from about 2 ms to about 260 ms (maximum 292 ms) and recovered immediately after the burst -- the tick budget bounds any single task's hold on the queue. `docs/execution-model.md` Under sustained load states the semantics behind these numbers. The connection-slot recycling evidence rode along: the health route's `users` line read 1/255 after every concurrent run, including the 255-connection run at the cap, and 2/255 after the head-of-line run.

**A state-touching workload, measured once.** Every throughput figure above drives `GET /health`, a route that answers from the connection object and touches no persistent state. Same rig (`--state-workload` in `scripts/measure-baseline.py`), same machine, 2026-07-29: a clean-slate boot with the composite example deployed (crypto module loaded), a principal provisioned operator-side on the console (`identity mint`, `session mint`).

| Route | Requests/sec | Median latency | p95 latency |
|---|---|---|---|
| `POST /inventory/items` (200 sequential authenticated writes: bearer-token validation, a persistent daemon mutation, and the synchronous audit observer per request) | about 970 | 1.0 ms | 1.2 ms |
| `GET /inventory/health` (zero-work comparison, same transport and routing machinery) | about 2,100 | 0.5 ms | not measured |

On this run the full authenticated, audited write path cost roughly half a millisecond over the empty route. The slot-recycling evidence rode along (a console `status()` probe read users=1/255 before and after each phase). One machine, one workload shape, one measured run each: a rig and a datum, not a guarantee.

**Sizing a workload against the envelope.** A worked translation from a hypothetical workload to the ceilings above, chaining record count through to snapshot cost and connection economics. A **50,000-record authenticated service**, one clone per record: 50,000 sits well inside the `objects` table's 65,535-object headroom (Limits and capacity above), so this shape is a `Yes` in the Sizing-a-workload row for "One clone per record". If each record's clone carries a modest property set, the resulting image lands in the tens-of-MB range the stock-`sector_size` table above already covers directly -- read its pause and RSS off the row nearest that size rather than extrapolating past the measured 506 MB point. At `dump_interval` 3600 (one hour, the field table's suggested default), the RPO for this shape is one hour of writes (`docs/operations.md` Availability and data-loss model), and the recurring pause each cycle is the sub-0.04 s figure the stock-`sector_size` row shows at this scale -- negligible against a one-hour cycle. For 40 concurrent users driving requests against this service, the concurrency table above says the platform serves them from the same connection-slot pool the 255-cap governs, with throughput still in the 2,200-2,400 req/s range at that client count and median latency in the tens of milliseconds, not the sub-millisecond figures `GET /health` shows alone -- the state-touching-workload table above is the closer analogue for a service that reads or writes on every request. A **state-heavy store approaching the ceiling** -- one `/lib/KVstore` holding a million string-keyed records at fan-out 1,000 -- costs roughly 1,000-2,000 `objects`-table slots for its node clones (the Sizing-a-workload row above), leaving the bulk of the 65,535-object ceiling for the rest of the domain; its snapshot cost is driven by total dataspace size, not node count, so the same stock-`sector_size` table is the reference once the store's on-disk footprint is known, and a store approaching the 4 GiB stock-build state ceiling needs the `sector_size` 32768 column instead, whose only measured point past a gigabyte is 1.233 s at 1.08 GB.

Two interactions this worked pass surfaces early: resident memory runs several times the on-disk image size (plan host RAM off the RSS column, not the snapshot-file size), and the pause scales with both image size and `sector_size` (the two runs disagree at the ~512 MB scale, above) -- re-measure with `scripts/measure-baseline.py` at the shape's actual scale rather than interpolating between the rows here.

**Unmeasured today.** Sustained behavior held over time near the `objects` / `call_outs` / `array_size` ceilings (the growth and concurrency runs above are single-pass, not extended-duration), and the memory cost of a driver rebuilt with wider index types are not measured against this codebase. No run has held the platform under continuous load for days or weeks: dynamic-arena growth past the chunk-ahead behavior above, swap-file fragmentation, call_out table churn, and log or audit growth against the dump cycle have no published multi-day trajectory, so an operator tuning alert thresholds has no baseline to check a slowly climbing signal at day ten against. The sector-size dependence of the pause at the ~512 MB scale (0.037 s at `sector_size` 8192 vs 0.541 s at `sector_size` 32768) is an open observation from single runs, not yet explained. Treat the tables above as compiled-in ceilings and documented defaults, not throughput guarantees.

## Where to next

- **[`docs/operations.md`](operations.md)** covers the operator's task surface: booting, backing up and restoring state, monitoring, diagnosing failures, and loading host-driver extensions.
- **[`docs/evaluating.md`](evaluating.md)** covers the fit decision, including the measured envelope this document's capacity tables carry the detail for.
- **[`docs/persistence.md`](persistence.md)** covers the statedump cycle and persistence model that `dump_interval`, `dump_file`, and the swap parameters serve.
