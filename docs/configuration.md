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

**Snapshot-pause scaling, measured across two sector sizes.** The dump-time pause scales with in-memory image size, not with the config caps above ("a multi-gigabyte image can take seconds to write; the runtime briefly blocks during the dump", `docs/persistence.md` The statedump cycle). Measured 2026-07-12 and 2026-08-01/02 on an Apple M5 Max (macOS 26.5, arm64, local NVMe) with `scripts/measure-baseline.py`: at the stock `sector_size` of 8192, the client-observed pause -- the window a connected console waits after `snapshot` -- ran 0.003 s to 0.037 s from a 2.4 MB base image through a 506 MB image, with one outlying 0.125 s point at a 103 MB image; these are single-run samples, not averaged, and not monotonic across steps (filesystem caching and swap-file growth dominate at these sizes). Raising `sector_size` to 32768 (the `MEASURE_SECTOR_SIZE` knob on the same script; see `docs/configuration.md` above and `scripts/README.md`) reaches farther: 0.021 s at a 244 MB image, 0.541 s at a 513 MB image, and 1.233 s at a 1083 MB (1.08 GB) image -- the upstream expectation above, that a multi-gigabyte image can take seconds to write, now has a measured point on this hardware just past a gigabyte. The two runs' ~512 MB-scale points disagree across sector sizes (0.037 s at sector_size 8192 vs 0.541 s at sector_size 32768, both single runs, unexplained) -- an open observation, not a theory. Cold boot to console-ready measured 0.08-0.18 s across these runs; restore-boot readiness held at 0.06 s both against the earlier 237 MB image and against the 1.08 GB image (state pages in on demand after readiness); the bundled http-app answered sequential one-connection-per-request `GET /health` at 1,425-1,637 requests per second across the growth runs. A companion `--tls` run of the same script over the native TLS 1.3 stack -- the reference HTTPS application, a self-signed certificate activated through the `tls-cert` reload verb -- measured the same one-connection-per-request `GET /health` shape at about 470 requests per second with a median TLS handshake of roughly 1.5 ms, on that run about a third of the cleartext rate: the expected cost of terminating TLS in interpreted LPC, with a cheap handshake. One machine, one workload shape, several consistent single runs: a rig and a growing set of data points, not a guarantee. Two capacity facts the rig surfaced: the stock build caps `swap_size` at 65535 sectors, so swap capacity scales through `sector_size`; and an image that outgrows the swap device dies with a fatal `out of sectors` error.

**Concurrency, measured twice.** Same rig (`--concurrent`, `--headline` in `scripts/measure-baseline.py`), same machine. An earlier run (2026-07-19) with parallel closed-loop clients driving `GET /health` at 2, 8, and 32 connections found aggregate throughput saturating around 2,400 requests per second alike (2,346 / 2,386 / 2,391), while median per-request latency grew with client count (0.8 / 3.2 / 12.9 ms) -- the signature of one serialization point servicing sub-millisecond requests: added concurrency buys queueing, not parallelism. A follow-up run (2026-08-01) pushed to the stock `users` ceiling -- 32, 64, 128, and 255 connections, the last matching the 255-connection cap -- and held the same pattern: aggregate throughput stayed flat at 2,200-2,340 requests per second (2,311 / 2,340 / 2,200 / 2,268) while median latency climbed with client count (13.5 / 26.0 / 31.1 / 58.1 ms, p95 13.9 / 27.3 / 52.1 / 100.2 ms); every request in the run was served at the full 255-connection cap. The head-of-line worst case, measured directly: with the driver saturated by back-to-back near-budget busy tasks (about 88 ms each on this hardware; a full default 20,000,000-tick budget is roughly 90-120 ms of driver wall), a probing client's median `/health` latency rose from about 2 ms to about 260 ms (maximum 292 ms) and recovered immediately after the burst -- the tick budget bounds any single task's hold on the queue. `docs/execution-model.md` Under sustained load states the semantics behind these numbers. The connection-slot recycling evidence rode along: the health route's `users` line read 1/255 after every concurrent run, including the 255-connection run at the cap, and 2/255 after the head-of-line run.

**A state-touching workload, measured once.** Every throughput figure above drives `GET /health`, a route that answers from the connection object and touches no persistent state. Same rig (`--state-workload` in `scripts/measure-baseline.py`), same machine, 2026-07-29: a clean-slate boot with the composite example deployed (crypto module loaded), a principal provisioned operator-side on the console (`identity mint`, `session mint`), and 200 sequential authenticated `POST /inventory/items` writes -- each request paying bearer-token validation at the handler, the persistent daemon mutation, and the synchronous audit observer in one task -- measured about 970 requests per second (latency median 1.0 ms, p95 1.2 ms). The same boot answered the zero-work comparison, `GET /inventory/health` through the identical transport and routing machinery, at about 2,100 requests per second (median 0.5 ms): on this run the full authenticated, audited write path cost roughly half a millisecond over the empty route. The slot-recycling evidence rode along (a console `status()` probe read users=1/255 before and after each phase). The growth run above now also samples driver resident memory beside each snapshot-pause step: at the stock sector size, 7 MB RSS over the 2.4 MB base snapshot, then 112 MB, 462 MB, 1,402 MB, 3,013 MB, and 5,297 MB as the snapshot file grew to 36 MB, 103 MB, 237 MB, 372 MB, and 506 MB -- resident memory runs several times the on-disk image because the dynamic allocator arena grows in chunks ahead of use (the final 506 MB step's own status line showed a 4.8 GB arena at 11% used). One machine, one workload shape, one measured run each: a rig and a datum, not a guarantee.

**Unmeasured today.** Sustained behavior held over time near the `objects` / `call_outs` / `array_size` ceilings (the growth and concurrency runs above are single-pass, not extended-duration), and the memory cost of a driver rebuilt with wider index types are not measured against this codebase. No run has held the platform under continuous load for days or weeks: dynamic-arena growth past the chunk-ahead behavior above, swap-file fragmentation, call_out table churn, and log or audit growth against the dump cycle have no published multi-day trajectory, so an operator tuning alert thresholds has no baseline to check a slowly climbing signal at day ten against. The sector-size dependence of the pause at the ~512 MB scale (0.037 s at `sector_size` 8192 vs 0.541 s at `sector_size` 32768) is an open observation from single runs, not yet explained. Treat the tables above as compiled-in ceilings and documented defaults, not throughput guarantees.

## Where to next

- **[`docs/operations.md`](operations.md)** covers the operator's task surface: booting, backing up and restoring state, monitoring, diagnosing failures, and loading host-driver extensions.
- **[`docs/evaluating.md`](evaluating.md)** covers the fit decision, including the measured envelope this document's capacity tables carry the detail for.
- **[`docs/persistence.md`](persistence.md)** covers the statedump cycle and persistence model that `dump_interval`, `dump_file`, and the swap parameters serve.
