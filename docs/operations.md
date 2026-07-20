# Operations

This document covers running an eOS-kernellib instance: configuring it via the `.dgd` file, booting and re-booting it, snapshotting, backing up, and restoring its persistent state, its availability and data-loss model, monitoring its output, diagnosing failures, sizing it within the platform's limits, and loading optional host-driver extensions. The architecture document (`docs/architecture.md`) covers the platform's structural model. This document covers the operator's surface for keeping it running.

**Audience**: someone running the platform, responsible for choosing config values, watching the running process, taking snapshots, restoring after a crash, and deciding whether to load extensions. Application authoring is covered in `docs/application-authoring.md` and `docs/http-applications.md`.

## The operator's task index

The jobs this doc (and its task-shaped companions) cover, by name:

| I need to... | Where |
|---|---|
| Choose config values, raise a ceiling | The .dgd configuration file; Limits and capacity |
| Size a workload's storage shape | Limits and capacity, Sizing a workload |
| Boot cold, restore a snapshot, hot-boot | Booting |
| Take a backup now; restore one (same host or off-host) | Backing up and restoring state |
| Schedule unattended off-host backups | Backing up and restoring state, Scheduled off-host backup, end to end |
| Run under systemd, launchd, or another supervisor | Running under a supervisor |
| Monitor headlessly and write alert rules | Monitoring signals |
| Expose a machine-readable health endpoint | `docs/common-tasks.md` Expose a health check for monitoring |
| Serve HTTPS on the labeled port | Network boundary and transport security; `docs/common-tasks.md` Serve HTTPS on the labeled port |
| Load a host-driver extension | Loading host-driver extensions |
| Diagnose a dead or misbehaving process | Common failure modes; Monitoring signals |
| Ship a release; roll one back | `docs/changing-a-running-system.md` Shipping a release, Rolling back a release |
| Provision, rotate, or offboard an operator credential | `docs/security-posture.md` Credential lifecycle |

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
| `modules` | Optional mapping of host-driver extensions to load at boot (see Loading host-driver extensions below) |

A minimal example is included at `example.dgd` in the repository root.

## Booting

The platform has three boot modes. `docs/architecture.md` covers the dispatch in detail. Briefly:

- **Cold boot**: started with no snapshot argument. The driver compiles `/kernel/sys/driver`, which compiles the System initd. The System initd's `create()` iterates and loads every `/usr/[A-Z]*/initd.c`, and the platform reaches the running state. This is the path for first-time bring-up and after intentional state wipe.
- **Snapshot restore**: started with the snapshot named on the command line (`dgd config_file dump_file [dump_file.old]`, Backing up and restoring state below). The driver reloads the snapshotted object graph and dataspaces, then calls the registered `restored(int hotboot)` driver hook. Initd cascades do not run. The platform resumes the state captured at the snapshot. The driver never restores a snapshot it was not given as an argument, however current the file sitting at `dump_file`.
- **Hot boot**: `shutdown(1)` followed by `execv` (when the `.dgd` file's `hotboot` tuple is set). Open file descriptors and connections are inherited by the replacement process. The snapshot is written and reloaded, but external connections survive the transition. Used for upgrading the host binary or `.dgd` config without dropping live work.

## admin_console

`admin_console` is the operator REPL. It listens on `telnet_port` and offers a verb-based command surface for inspecting and manipulating the running platform. Source lives at `src/kernel/lib/admin_console.c` and `src/kernel/obj/admin_console.c`.

The verb categories below cover the shipped surface. Each verb prints its own help when invoked without arguments.

| Category | Verbs |
|---|---|
| REPL and history | `code`, `history`, `clear` |
| Object lifecycle | `compile`, `clone`, `destruct`, `new` |
| Filesystem-style navigation | `cd`, `pwd`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir` |
| Editor | `ed` |
| Permissions | `access`, `grant`, `ungrant` |
| Resources | `quota`, `rsrc` |
| Status and people | `status`, `people` |
| State management | `swapout`, `snapshot` |
| Platform lifecycle | `shutdown`, `reboot` |

`code` is the LPC eval verb: it compiles its argument as an LPC expression in the operator's domain, evaluates it, and prints the result. See `docs/admin-console.md` for per-verb mechanics, operational scenarios, and the underlying kfun dispatch for each verb.

## State persistence

The platform has three kfuns that operate on persistent state:

- `swapout()`: swap all in-memory objects to the swap file. Frees memory at the cost of paging on next access.
- `dump_state(int incr)`: write a snapshot to `dump_file`. With argument `0` (or no argument), writes a full snapshot. With non-zero argument, writes an incremental snapshot.
- `shutdown(int hotboot)`: shut down the platform. The kfun does not snapshot before exiting. Whether the next boot has a snapshot to restore from depends on whether one was taken (by automatic `dump_interval`, by the `snapshot` verb, or by an explicit `dump_state` call). With non-zero argument and a `hotboot` tuple in the `.dgd` config, performs a hot boot via `execv` instead of exiting. If the `hotboot` tuple is absent, the kfun raises "Hotbooting is disabled".

The admin_console wraps these: `swapout` calls `swapout()`, `snapshot` calls `dump_state(0)`, `shutdown` calls `shutdown()` (cold shutdown without snapshot), `reboot` calls `dump_state(1)` then `shutdown()` (incremental snapshot then cold shutdown). For a clean shutdown that leaves a current restore point, run `snapshot` then `shutdown`, or use `reboot`.

The driver also takes automatic snapshots at the interval set by `dump_interval`. The previous snapshot is moved to `<dump_file>.old` before the new one is written. If the running snapshot is corrupt, deleting it leaves `.old` in place for restore on next boot.

A snapshot captures the persistent object graph: every object's variables, every clone's dataspace, every pending `call_out`. It does not capture open connections. On snapshot restore, connections must be re-established by clients. On hot boot, connections survive via the inherited file descriptors. After a snapshot restore, the driver invokes the registered `restored(int hotboot)` hook (see `src/kernel/sys/driver.c`) which emits a "State restored" message and gives the platform a place to re-attach managers and re-arm any external resources the snapshot did not preserve.

## Backing up and restoring state

A complete backup covers more than the dump file:

| Item | Why | Notes |
|---|---|---|
| `dump_file` and `<dump_file>.old` | The statedump pair | Rotation moves the previous file to `.old` before the new one is written (State persistence above). Copy both together, not `dump_file` alone |
| `src/kernel/data/` | Admin credentials and access bits | File-backed independently of the snapshot cycle: the admin password is written on every change, and access grants on every kernel-console `grant` / `ungrant` ("written to host files the moment they changed ... deliberately file-backed, so they survive even without a snapshot", `docs/first-hour.md`); grants made outside those verbs ride the image until the next flush |
| Vault data directories (the Vault daemon's on-disk store, `/usr/Vault/data/vault/<Domain>/...`) | Schema-exported per-domain state | The Vault daemon's own XML storage root, kept separately from the object graph's in-memory copy (`docs/vault-applications.md`) |
| Loaded extension binaries and the `.dgd` `modules` mapping | Restore precondition, not a file to copy | "statedumps created with a specific kfun extension in effect will require the the same kfun extension on restore" (Loading host-driver extensions below, quoting the 2010 Hydra note). Without the same extensions available, a snapshot will not restore at all |

**Safe copy.** Copy after a dump completes, not against a swap file mid-write: `dump_state` briefly blocks the platform while it runs (`docs/persistence.md` The statedump cycle), and the rotation is a rename rather than an in-place edit, but the window is still real. Take a deliberate full dump first (the `snapshot` verb) so the backup generation is self-sufficient, then copy `dump_file` and `<dump_file>.old` together. Keeping the pair costs one extra file and removes any question of which generation is on disk.

**Restore.** Two forms, both invoked as `dgd config_file [restore files]`:

- **Full restore**: `dgd config_file dump_file`. Works when `dump_file` holds a full snapshot: the `snapshot` verb (`dump_state(FALSE)`, `src/kernel/lib/admin_console.c` `cmd_snapshot`) and the console dump-and-exit path (`dump_state(FALSE)`, `src/usr/System/sys/persist_helper.c:54`) both leave one.
- **Two-file (incremental) restore**: `dgd config_file dump_file dump_file.old`. Required when `dump_file` holds an incremental snapshot written by `dump_state(1)`/`dump_state(TRUE)`. The argument order (the current dump file first, its full base second) is verified from the DGD driver's own usage line, `Usage: dgd config_file [[partial_snapshot] snapshot]`, and from `Config::restore(fd, fd2)`: the header read from the first file is checked for the partial flag, and the second file is opened only to back it. This is the same order already documented for the `.dgd` file's `hotboot` tuple (`{ binary, config, snapshot, snapshot.old }`, The .dgd configuration file above). An unset second argument on a partial primary fails at boot with "Missing secondary snapshot".

This two-file form is a different recovery than the corrupt-snapshot fallback in Common failure modes below, which discards the newer `dump_file` outright and restores from `<dump_file>.old` alone as a self-contained snapshot. The two-file form instead restores using both files together, applying the incremental on top of its base.

Which stop path leaves which case:

| Stop path | Call | Restore needs |
|---|---|---|
| Kill signal (SIGTERM) | `prepare_reboot()` then `dump_state(1)` (`src/kernel/sys/driver.c:757-766`) | `dump_file` + `dump_file.old` (in practice, see below) |
| `reboot` verb | `dump_state(TRUE)` then `shutdown()` (`cmd_reboot`, `src/kernel/lib/admin_console.c`) | `dump_file` + `dump_file.old` (in practice, see below) |
| `snapshot` verb | `dump_state(FALSE)` (`cmd_snapshot`) | `dump_file` alone |
| Console dump-and-exit path | `dump_state(FALSE)` then `shutdown()` (`src/usr/System/sys/persist_helper.c:54`) | `dump_file` alone |

A supervisor sending SIGTERM (the ordinary "stop the service" path outside admin_console) and the `reboot` verb both write an incremental. Strictly, a `dump_state(1)` dump is written as a partial only when swapped-out objects are pending at dump time. A small freshly-booted image can produce a self-contained file (which is why the tutorial's single-file restore succeeds after its first `reboot`), but on a long-running image the partial case is the norm. Routine operator practice should keep `<dump_file>.old` alongside `dump_file` rather than treat it as disposable. A restore attempted with only `dump_file` after either path is the likely cause of a "Missing secondary snapshot" failure at boot.

**The off-host restore drill** (performed once, 2026-07-12): a snapshot written by the macOS/arm64 driver restored under the Linux/aarch64 driver built from the same source -- `State restored.` on the first boot line, and the deployed example's post-restore test phases ran to completion on the foreign host (its full sentinel count, including the persistence-verification phase). The procedure that worked, in full:

1. Copy the backup set to the target host: the `src` tree (which carries `src/kernel/data/` and the Vault XML directories inside it), the dump pair, and the `.dgd` config.
2. Edit one config field: `directory` to the tree's absolute path on the new host. The state-file fields resolve relative to `directory`, so a layout-preserving copy needs no other edit; the restore arguments resolve against the invocation directory.
3. Start the driver naming the snapshot: `dgd config dump_file [dump_file.old]`.

Portability is stated exactly as tested: one macOS/arm64-to-Linux/aarch64 restore with driver binaries built from the same source succeeded. Other host and architecture pairs are unverified; the driver's own guard for an unusable file is the `Bad or incompatible restore file header` refusal, so an incompatible pair fails at boot rather than corrupting.

**Backup-set coherence.** Take the dump pair and the tree at the same cut. The snapshot carries the compiled programs and all object state; the tree is what future compiles and cold boots build from, and it also carries the file-backed siblings (kernel data, Vault XML). A backup that pairs an older snapshot with a newer tree restores the older image state and will recompile against the newer sources on the next upgrade -- and Vault XML newer than the image diverges the other way: the next explicit Vault import (a respawn by name from the owning domain) re-imports state the image predates -- nothing re-reads the XML at restore itself. Neither is corruption; both are divergence you chose by mixing cuts.

**Scheduled off-host backup, end to end.** The unattended composition of the pieces above, from the host's scheduler (cron, launchd, a systemd timer):

1. **Trigger a deliberate full dump headlessly.** `scripts/drive-verbs.py` (the regression harness's telnet client, `scripts/README.md`) logs into the console and drives verbs from a file: a one-line ephemeral verbset -- `cmd: snapshot`; the verb succeeds silently, and step 2's mtime move is its acknowledgment -- makes the backup generation self-sufficient (the `snapshot` verb writes a full snapshot, the stop-path table above), rather than gambling on what the last `dump_interval` cycle left. The trigger authenticates as an operator (file-level `user:`/`password:` directives); remember the monitoring-credential caveat under Monitoring signals -- a console credential is never read-only in blast radius.
2. **Gate the copy on dump quiescence.** Proceed when `dump_file`'s modification time is newer than the trigger and stable: two mtime reads a few seconds apart that agree (A stalled snapshot above explains why mtime is the signal).
3. **Copy the backup set at one cut**: `dump_file` and `<dump_file>.old` together, plus the `src` tree, which carries `src/kernel/data/` and the Vault XML inside it -- the full table above, at the same cut (Backup-set coherence above).
4. **Close the race.** The automatic `dump_interval` cycle can rotate the pair mid-copy. After the copy, re-read `dump_file`'s mtime; if it moved during the copy, redo the copy. With `dump_interval` sized in hours the retry is rare.

**Post-restore checklist.** `State restored.` as the first boot line after the version banner; the application's own verification (its sentinel driver or probes); clients reconnect (connections never survive a statedump restore); `Missing secondary snapshot` means an incremental primary was named without its base, and `Bad or incompatible restore file header` means the file and binary do not match.

## Availability and data-loss model

The platform is a single process on a single machine. There is no replica to fail over to and no distributed consensus to reason about (`docs/persistence.md`: "this platform is deliberately single-coherence-domain"). Availability and data loss are properties of one process's dump-and-restore cycle, not of a cluster.

**Recovery point.** The RPO is `dump_interval` (The .dgd configuration file above), a sizing decision the operator makes, not a platform-supplied guarantee. Work committed after the last completed dump and lost on an unclean stop is bounded by that interval. The prior snapshot is untouched by a failed or interrupted dump attempt and remains a valid restore point (Backing up and restoring state above).

**Crash semantics.** A crash, such as a process killed before any dump runs, host power loss, or a `dump_state` failure mid-write (Common failure modes below), loses everything committed since the last completed dump. The platform does not partially apply an interrupted dump.

**Downtime taxonomy.**

| Mode | Trigger | Connections | State |
|---|---|---|---|
| Hot boot | `shutdown(1)` + `execv`, with a `hotboot` tuple configured (Booting above) | Survive: inherited file descriptors | Survives: dump plus immediate reload |
| Statedump restore | Cold start naming the snapshot on the command line (full, or the two-file incremental form) | Drop: clients reconnect | Survives, from the dump file(s) |
| Cold boot | Cold start with no restore argument | Drop | Rebuilt from source: only what the initd cascade recreates, nothing carried over |

**Portability.** A snapshot restores only against a driver started with the same `auto_object` and `driver_object`, and with the same `modules` extensions loaded (Common failure modes below, the same conditions `docs/persistence.md` states for hot boot). It is a resume point for a specific configuration, not a portable backup format across incompatible driver configurations.

## Logging and diagnostics

The driver provides a `message(string)` function that timestamps and emits diagnostic output:

```c
ctime(time())[4..18] + " ** " + str
```

`message` is called by the driver during initialization, snapshot restore, and on interrupt. It is callable by kernel-tier and System-tier code. The underlying `send_message` kfun routes the output to the connection that triggered the current call (typically the operator at admin_console). Application-tier code does not invoke `message` directly.

The platform's general diagnostic facility is `logd`, a System-tier daemon at `/usr/System/sys/logd`. It owns a single persistent sink (`/usr/System/log/system.log`), the emission threshold, and the operator surface. The three diagnostic calls platform and application code already carry, `debugLog` / `info` / `sysLog` (defined in `/lib/util/lpc.c`), forward to `logd`, each mapped to a fixed severity:

| Call | Level | Intended use |
|---|---|---|
| `debugLog(str)` | DEBUG | developer tracing |
| `info(str)` | INFO | routine progress |
| `sysLog(str)` | NOTICE | general system events |

Levels are ordered ascending by severity (DEBUG < INFO < NOTICE < ERROR). ERROR is reserved for `errord`'s reports (see runtime errors below). `logd` drops any message below its threshold, and the threshold defaults to INFO, so `debugLog` output is suppressed until an operator lowers it. The forwarders reach the daemon by `call_other` rather than inheritance, so code in any tier can log without the `/kernel/lib` inheritance restriction that constrains the capability library (`docs/capability.md`). A `find_object` guard turns a log call into a no-op during the boot window before `logd` is loaded, rather than an error.

`logd` never writes synchronously. DGD forbids `write_file` inside an `atomic` function, and the diagnostic calls fire from atomic contexts (Schema import, the property-change dispatcher). So each call buffers its line in memory and schedules a single coalesced `call_out(0)`. The flush appends the buffered batch to `system.log` in a fresh, non-atomic execution where the write is legal, and echoes NOTICE-and-above lines to the operator console (via `message`) for a live view. A line logged inside a committed atomic is written after the atomic commits. A line logged inside an atomic that rolls back is discarded with the buffer. Its work did not happen, so its progress log is moot. The deferral also makes logging non-throwing, which is load-bearing: the driver notifies `errord` of even caught errors, so a synchronous in-atomic write failure would feed back through `errord` into logging and storm.

Two admin_console verbs, registered through `admin_console_registry`, are the operator surface:

- `log [N]`: tail the last N lines of `system.log` (default 40, bounded to the final 8 KB so a large unrotated log does not load wholesale). It is read-only and rides the console's existing privilege.
- `log-level [LEVEL]`: with no argument, report the current threshold. With `debug` / `info` / `notice` / `error`, set it. The set path mutates daemon state, so it is capability-gated: the verb routes through the registry's KERNEL-elevation helper, which checks the `admin_console.caller` capability via `capabilityd` (`docs/capability.md`) before applying the change. `logd` is the first System-tier consumer of the capability library.

`logd` appends and never prunes. Rotation and retention are ordinary log-management tooling's responsibility: the daemon builds in no rotation policy, the same posture the property-change dispatcher's audit log takes (below).

For runtime errors, the driver dispatches through three hooks before falling back to a default formatter:

| Hook | When | Routed to |
|---|---|---|
| `runtime_error(str, caught, trace)` | Uncaught LPC error during normal execution | `errord->runtime_error()` |
| `atomic_error(str, atom, trace)` | Uncaught error inside an `atomic` function | `errord->atomic_error()` |
| `compile_error(file, line, str)` | LPC compilation failure | `errord->compile_error()` |

`errord` is registered via `driver->set_error_manager()`. Its `runtime_error` hook returns the error string, and the driver adopts the returned value, so an error manager may rewrite an error's message before it propagates further; the shipped errord returns it unchanged (`docs/debugging-applications.md` Reading an error trace). eOS-kernellib ships an errord at `src/usr/System/sys/errord.c` that formats the trace into a readable form and sends it via `send_message` to the relevant operator. In addition to the console, `errord` drains each formatted report into `logd`'s sink at ERROR level (its `persist` path), so error diagnostics are durable rather than console-transient. This is how the platform's error reporting survives the atomic barrier: an `atomic_error` is dispatched only after its failed atomic has already rolled back, so the diagnostics would otherwise vanish with the rollback. The driver carries the trace across the barrier in thread-local storage, `errord` formats it post-rollback, and the `logd` tee persists it to `system.log`. If no errord is registered or its handler raises, the driver falls back to a built-in formatter that walks the trace and emits via the same default channel. Errors never silently disappear.

The property-change dispatcher writes a per-failure audit line to `/usr/Merry/log/dispatch.log` on observer-cycle detection, cascade-depth overflow, and observer-source compile failures. Volume is low under normal operation (writes only on detected failures). The log is rotated by ordinary log-management tooling: the dispatcher does not build in a rotation policy. See `docs/dispatcher.md` Audit log.

Optional verbose-trace lines are general diagnostics rather than audit, so they route to `logd` at DEBUG level (not to `dispatch.log`) when the `dispatch_trace` flag is on. Toggle via the admin verb `dispatch-trace on|off|status` (see `docs/admin-console.md` Dispatcher operator surface) or via `MERRY->set_dispatch_trace(int flag)` (KERNEL-gated, public read via `MERRY->query_dispatch_trace()`). Default is off. Trace lines elide their I/O entirely when the flag is unset. Two gates apply when on: the flag enables emission, and `logd`'s threshold must admit DEBUG lines. Under the default INFO threshold the lines are dropped, so pair `dispatch-trace on` with `log-level debug` (the verb prints a hint when the current threshold would suppress trace). Read the result with the `log` verb. Routing through `logd`'s deferred flush also means trace lines survive atomic-mode dispatch, where a direct file write would be refused. When on, the current scope emits one trace line per `dispatch_set` entry (object name + path). Additional trace sites are future-work. Leaving trace on during steady-state operation increases log volume. The flag is intended for operator-driven troubleshooting sessions.

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
| `swap_size` × `sector_size` | 65535 × 1024 bytes | N/A | About 64 MiB of pageable object storage, sized to the example's tiny working set rather than a production footprint |

The stock driver build's index widths (matching the driver's own header comment: "default: 64K objects, 64K swap sectors, 255 users, max string length 64K") set the ceilings above. A driver rebuilt with wider `uindex`/`eindex` types raises them, at the cost of a larger per-object memory footprint. eOS-kernellib runs against a stock build, so the table above is the practical ceiling until that changes.

Ceilings that are not `.dgd` fields:

| Ceiling | Value | Source |
|---|---|---|
| Host driver's core kfun set | Capped at 256, by the 1-byte kfun numbering | `docs/architecture.md` Host-driver extensions |
| Per-execution tick budget | 20,000,000 ticks, default | Set at boot in `src/kernel/sys/driver.c`. Raised or lowered per owner via `quota <owner> ticks <limit>` (Resource limits above) |
| LPC `int` width | 32-bit signed | `docs/lpc-essentials.md` Types and values |

**Sizing a workload.** Which storage shape holds N records, and which ceiling binds first. One constant the tables above do not carry: **a single mapping caps at 32,767 key-value pairs** on a stock build -- the `array_size` knob governs mappings as well as arrays, and exceeding it raises `"Mapping too large"` at construction or `"Mapping too large to grow"` on assignment past the cap (verified against the driver source; the same knob is already at its driver ceiling, so there is no config headroom). The planned two-level mapping is the roadmap's answer for one logical mapping beyond that bound (`docs/runtime-platform-roadmap.md` Wave 3).

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

**Snapshot-pause scaling, measured once.** The dump-time pause scales with in-memory image size, not with the config caps above ("a multi-gigabyte image can take seconds to write; the runtime briefly blocks during the dump", `docs/persistence.md` The statedump cycle). Measured 2026-07-12 on an Apple M5 Max (macOS 26.5, arm64, local NVMe) with `scripts/measure-baseline.py`: the client-observed pause -- the window a connected console waits after `snapshot` -- stayed at or under 0.12 s from a 2 MB base image through a 237 MB image, and was not monotonic across steps (filesystem caching and swap-file growth dominate at these sizes). The same runs measured cold boot to console-ready at roughly 0.1 s, a restore boot against the 237 MB snapshot reaching console-ready in under 0.1 s (state pages in on demand after readiness), and the bundled http-app answering about 1,600 sequential one-connection-per-request `GET /health` requests per second. A companion `--tls` run of the same script over the native TLS 1.3 stack -- the reference HTTPS application, a self-signed certificate activated through the `tls-cert` reload verb -- measured the same one-connection-per-request `GET /health` shape at about 470 requests per second with a median TLS handshake of roughly 1.5 ms, on that run about a third of the cleartext rate: the expected cost of terminating TLS in interpreted LPC, with a cheap handshake. One machine, one workload shape, two consistent runs: a rig and a datum, not a guarantee. Two capacity facts the rig surfaced: the stock build caps `swap_size` at 65535 sectors, so swap capacity scales through `sector_size`; and an image that outgrows the swap device dies with a fatal `out of sectors` error.

**Concurrency, measured once.** Same rig (`--concurrent`, `--headline` in `scripts/measure-baseline.py`), same machine, 2026-07-19: with parallel closed-loop clients driving `GET /health`, aggregate throughput saturates around 2,400 requests per second at 2, 8, and 32 connections alike (2,346 / 2,386 / 2,391), while median per-request latency grows with client count (0.8 / 3.2 / 12.9 ms) -- the signature of one serialization point servicing sub-millisecond requests: added concurrency buys queueing, not parallelism. The head-of-line worst case, measured directly: with the driver saturated by back-to-back near-budget busy tasks (about 88 ms each on this hardware; a full default 20,000,000-tick budget is roughly 90-120 ms of driver wall), a probing client's median `/health` latency rose from about 2 ms to about 260 ms (maximum 292 ms) and recovered immediately after the burst -- the tick budget bounds any single task's hold on the queue. `docs/execution-model.md` Under sustained load states the semantics behind these numbers. The connection-slot recycling evidence rode along: the health route's `users` line read 1/255 after each concurrent run and 2/255 after the head-of-line run.

**Unmeasured today.** Dump-pause behavior beyond a quarter-gigabyte image, sustained throughput near the `objects` / `call_outs` / `array_size` ceilings (the concurrency figures above stop at 32 connections, far from any table cap), and the memory cost of a driver rebuilt with wider index types are not measured against this codebase. Treat the tables above as compiled-in ceilings and documented defaults, not throughput guarantees.

## Running under a supervisor

The platform is one process. A process supervisor (systemd, a container runtime, runit) owns its lifecycle: start it, restart it on exit, stop it on demand. Two facts shape that configuration.

**A graceful stop takes a final snapshot.** The driver catches `SIGTERM`, the default stop signal for `systemctl stop`, `docker stop`, and a bare `kill`. On receipt it runs `prepare_reboot()`, writes an incremental snapshot with `dump_state(1)`, and shuts down cold (`src/kernel/sys/driver.c:757-766`, reached through the `SIGTERM` handler in `dworkin/dgd` `src/host/unix/local.cpp`). A supervisor's ordinary stop therefore leaves a current restore point with no operator action. This is the same incremental form the `reboot` verb writes (Backing up and restoring state above), so recovery needs both `dump_file` and `<dump_file>.old`.

Give the stop timeout room for the dump. Dump time scales with the in-memory image size (Limits and capacity above), so a large image needs a stop timeout longer than a supervisor's default. A supervisor that escalates to `SIGKILL` before the dump finishes loses that snapshot.

`SIGINT`, `SIGHUP`, `SIGUSR1`, and `SIGUSR2` are not caught: their default disposition applies, so a stop sent as `SIGINT` terminates the process without the snapshot. Only `SIGTERM` runs the snapshot-and-shutdown path. `SIGINT`, `SIGKILL`, and a host crash bypass it and lose the work committed since the last automatic dump (Availability and data-loss model above).

**Restart is a cold restore, not a hot boot.** The signal path calls `shutdown()`, not `shutdown(1)`, so it does not `execv` and does not preserve connections. On restart the supervisor cold-boots against the snapshot pair (`dgd config_file dump_file dump_file.old`), the platform restores state, and clients reconnect. Connection-preserving hot boot is a separate operator action: the System console's `hotboot` verb calls `shutdown(1)` to `execv` the replacement process against a configured `hotboot` tuple (Booting above). No supervisor signal triggers it. Configure the supervisor to restart with the dump files in place: a restart that cannot read them cold-boots from source and carries no state across (the Cold boot row of the downtime taxonomy above).

**A boot-state-invariant start command.** The restore arguments are the fork in the road: passing them when the files are absent is a fatal `Config error: cannot open restore file`, and omitting them when a snapshot exists silently cold-boots a stale world -- the driver has no auto-detection (Booting above). A fixed `ExecStart` cannot cover both states, so start through a wrapper that passes the snapshot pair only when present:

```sh
#!/bin/sh
# run-dgd.sh -- boot-state-invariant start
cd /srv/eos || exit 1
set --
[ -f state/snapshot ] && set -- state/snapshot
[ -f state/snapshot.old ] && set -- "$@" state/snapshot.old
exec /srv/eos/dgd/bin/dgd server.dgd "$@"
```

A reference unit around it, each setting tied to a fact above:

```ini
[Unit]
Description=eOS-kernellib platform
After=network.target

[Service]
Type=simple
User=eos
WorkingDirectory=/srv/eos
ExecStart=/srv/eos/run-dgd.sh
Restart=on-failure
KillSignal=SIGTERM
# SIGTERM triggers the snapshot-then-exit path; give the dump room.
# Scale with image size (Limits and capacity above).
TimeoutStopSec=300

[Install]
WantedBy=multi-user.target
```

`KillSignal` stays `SIGTERM` because it is the only signal the driver catches for the graceful snapshot; `Restart=on-failure` rather than `always` so a deliberate operator `halt` stays down.

**Native TLS.** The platform terminates TLS 1.3 itself (Network boundary and transport security below): configure a second `binary_port` entry for the `https` label, load the crypto module (Loading host-driver extensions below), and place PEM credentials at the configured paths. The host's ACME client owns issuance and renewal -- e.g. a certbot deploy hook copying fullchain and private key into `<directory>/usr/System/data/tls/` (readable only by the runtime user). A first-ever certificate that lands after boot is activated with `tls-cert reload` on the console; renewals need only the file copy, since credentials are read per connection.

**Reverse proxy (alternative).** Where one host already fronts several services with a single proxy, terminating TLS there remains valid:

```nginx
server {
    listen 443 ssl;
    server_name example.org;
    # ssl_certificate / ssl_certificate_key per your issuance
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
    }
}
```

**Log rotation.** `logd` appends one line per flush and holds no descriptor between flushes, so plain logrotate with `create` suffices -- no signal, no copytruncate. The host path is the `.dgd` `directory` value plus `/usr/System/log/system.log`:

```text
/srv/eos/src/usr/System/log/system.log {
    weekly
    rotate 8
    compress
    missingok
    create 0640 eos eos
}
```

## Monitoring signals

An unattended deployment needs its health read by a monitoring system, not by an operator at the telnet prompt. The signals below already exist. This section maps each to the condition it warns of and a way of reaching it without a human on the console. The interactive-triage view of the same signals is in `docs/admin-console.md` (Debugging a stuck platform).

**The monitoring credential.** Provision a dedicated operator with nothing beyond registration -- `grant monitor access` and no directory grants -- and its surface, verified against a live console, is: `status` and `people` answer; the registry extension verbs (`log`, `observers`, `dispatch-trace`, and the rest) answer `No command`, because a registered user logs into the System console and only the kernel console (the `admin` login) routes them; `upgrade` refuses every source the operator cannot write. Log-based alerting therefore reads the host file directly (the `directory` value plus `/usr/System/log/system.log`, Running under a supervisor above) rather than the `log` verb. One warning keeps this credential honest: `halt` carries no access gate, so even this minimal operator can stop the platform -- a console credential is never read-only in blast radius, and the telnet-tunnel perimeter (`docs/admin-console.md` Console security posture) is what actually protects it.

What `status` looks like when healthy (captured from a live console; the counts map to the table below):

```text
                                          Server:       DGD 1.7.9
------------ Swap device -------------
sectors:        513 /     65535 (  1%)    Start time:   Jul 12 15:40:27 2026
sector size:   1K
swap average:  0.75, 0.15                 Uptime:       00:00:01

--------------- Memory ---------------    ------------ Callouts ------------
static:     1502332 /   1621584 ( 93%)    short:         1            (100%)
dynamic:     429476 /    780288 ( 55%) +  other:         0            (  0%) +
            1931808 /   2401872 ( 80%)                   1 /    10000 (  0%)

Objects:        215 /     10000 (  2%)    Users:         1 /      255 (  0%)
```

**Reading the block, field by field.** Every line is a slot of the no-argument `status()` vector (the `ST_*` indices a health probe reads directly):

- `sectors: used / total (%)` -- swap-device occupancy against the `.dgd` `swap_size` cap, with `sector size` echoing `sector_size`. This is the one fatal ceiling: an image that outgrows the swap device dies with a fatal `out of sectors` error (Limits and capacity above), so this percentage belongs in the alert set, with an earlier threshold than the degrading signals (table below).
- `swap average: a, b` -- objects swapped out per second, averaged over the last minute and the last five minutes (the driver counts swapouts per window; the console divides by 60 and 300). Near zero when the resident set fits memory; sustained non-zero values are the "sustained churn" the alert table names -- every access is paging.
- `static:` / `dynamic:` -- bytes in use versus bytes allocated from the host, for the two allocators: static holds the boot-time infrastructure tables (sized to the `.dgd` caps) and long-lived driver buffers, dynamic holds compiled programs and object data; the third row is their sum. The runtime grows both by chunks (`static_chunk` / `dynamic_chunk`), so the percentage reads utilization of the current allocation, not distance to a configured ceiling -- for capacity planning watch the absolute totals and the process RSS, not the percentage.
- `short:` / `other:` -- queued callouts, split by the driver's scheduling horizon: whole-second callouts due soon (within roughly the next two minutes) versus longer-dated and millisecond-delay ones; then their sum against the `call_outs` table cap -- the alert-table row.
- `Objects:` -- objects in use (masters plus clones) against the `objects` cap.
- `Users:` -- live connections against the `users` cap.
- `Server`, `Start time`, `Uptime` -- the driver banner (a `master`-built driver still prints the base release string, `docs/getting-started.md`), boot wall-clock, and time since -- the unexpected-reset signal.

**What an alertable line looks like.** A runtime error persists into `system.log` as a multi-line block: one timestamped `ERROR` header line carrying the message, then the indented trace frames beneath it (observed by tailing the log after a forced fault). Match alerting rules on the header (` ERROR `), not on frame lines; one fault produces one header and many frames.

**Capacity headroom, from `status()`.** The no-argument `status()` health vector (the `status` verb, `docs/admin-console.md`) carries the counts to watch against the `.dgd` caps (Limits and capacity above):

| Signal | Alert condition | Reading |
|---|---|---|
| call_out count vs the `call_outs` cap | Approaching the cap | A backlog of deferred work: new `call_out`s begin to fail |
| object count vs the `objects` cap | Approaching the cap | Allocation headroom is running out: clones and new objects begin to fail |
| swap sectors vs the `swap_size` cap | Rising occupancy, alerted earlier than the rows above | The one ceiling that is fatal rather than degrading: at the cap the platform dies with `out of sectors` (Limits and capacity above). The durable fix is a `sector_size` raise and a reboot from snapshot |
| users count vs the `users` cap | Approaching the cap | At the cap, new connections complete their TCP connect and are never answered, with nothing logged -- the silent form of full. A climbing count under flat traffic is a connection leak (Common failure modes below) |
| swap activity | Sustained churn | The resident set exceeds memory and every access pages. A `swapout` relieves pressure; the durable fix is a config raise and reboot |
| uptime, last reboot | Reset unexpectedly | The platform restarted: check it against the supervisor's restart log and the snapshot cadence |

Per-owner tick consumption is the other capacity signal. `rsrc ticks` (the resource daemon, Resource limits above) reports each owner's tick usage against its budget. An owner far above its peers is running away. A tick-exhausted call rolls back rather than hanging the platform.

**Alertable log lines.** `logd` writes `system.log` and tees `errord`'s reports there at ERROR level (Logging and diagnostics above):

- ERROR lines in `system.log`: every uncaught runtime, atomic, and compile error routes through `errord` to this sink. A rising ERROR rate is the platform-level fault signal.
- `cascade-aborted` and cycle-detection lines in `/usr/Merry/log/dispatch.log`: the property-change dispatcher records observer-cycle detection and cascade-depth overflow here (Logging and diagnostics above). These mark misbehaving application observer wiring.

**A stalled snapshot.** The most reliable unattended signal that automatic persistence has stopped is the `dump_file` modification time. A successful dump rewrites it (automatic every `dump_interval`, plus any explicit one), and the rotation moves the prior file to `<dump_file>.old` first (State persistence above). A `dump_file` mtime older than `dump_interval` plus the dump duration means automatic snapshots are no longer completing, most often disk-full or a permissions problem on the dump directory (Common failure modes below). Because the rotation writes `<dump_file>.old` before the new file, a failed write leaves the prior snapshot intact as a restore point. An operator-invoked `snapshot` that fails also reports to the console and, through `errord`, to `system.log`.

**Headless polling.** Reaching these signals without an operator borrows the mechanism the regression harness already relies on: a client drives the `admin_console` verbs over the telnet port and checks the replies. `scripts/drive-verbs.py` (documented in `scripts/README.md` and run by `drive-verbs-smoke.sh`) connects, authenticates, runs verbs such as `status`, and matches expected output. A monitoring probe follows the same shape: it reads the health vector on an interval and alerts on the thresholds above. An application can instead expose a runtime-derived health check on its own HTTP transport; the shipped `examples/http-app` demonstrates exactly that with `GET /status`, returning the capacity-headroom counts as `key=used/cap` lines a probe parses without a console login (`docs/common-tasks.md` Expose a health check for monitoring). That check rides `binary_port`, cleartext or TLS (Network boundary and transport security below).

## Network boundary and transport security

The platform listens on two kinds of port (The .dgd configuration file above): `telnet_port` for the operator console, and `binary_port` for application transports (HTTP and others). Their exposure and transport security are separate decisions.

**The operator console is unencrypted.** `admin_console` speaks plain telnet: the wire carries the operator's password and every command in clear text. Bind `telnet_port` to a loopback interface or a dedicated maintenance network, never a public one, and reach it through an SSH tunnel or a host-terminated TLS tunnel. Console access is equivalent to host shell access on the platform's process (`docs/admin-console.md` Console security posture), so the tunnel endpoint and its credentials carry that weight.

**Application HTTP terminates TLS natively.** Native TLS 1.3 termination is the platform transport (`docs/runtime-platform-roadmap.md` Transport posture records the activation): the HTTPS bootstrap (`src/usr/System/sys/https_server.c`) serves the labeled `https` binary port, cloning the application's TLS server mount (`/usr/WWW/obj/tls_server`) per connection -- `examples/https-app/` is the reference application. Activation needs three things: the lpc-ext crypto module (the TLS stack's body is gated on the host driver being built with `KF_SECURE_RANDOM`; Loading host-driver extensions below), a second `binary_port` entry (the port-label registry declares `https` for index 1), and PEM credentials at the configured paths (`/usr/System/data/tls/cert.pem` and `key.pem` by default). Anything missing is a logged stand-down, not an error. The `tls-cert` console verb reports status, revalidates the files, re-points the paths, and completes a deferred registration without a restart (`docs/admin-console.md`). Certificate acquisition and renewal stay with the host's ACME client writing those paths; credentials are read per connection, so a renewed file is picked up on the next handshake with no verb. The platform sees the real client address -- there is no proxy hop whose origin information would need reconstructing from forwarded headers.

**The private key does not persist.** The HTTPS bootstrap holds no key material in object state, and the TLS session drops the certificate key at handshake completion, so neither an idle image nor one with live established connections carries the key into the snapshot or swap files. This is a tested property, not a design intention: `scripts/https-smoke.sh` takes a console-driven statedump twice -- idle, and with an established TLS connection held open -- and scans it for the key as DER, PEM base64, and the raw private scalar. The state-file sensitivity table below is unchanged by TLS activation: those files hold the entire application state, just never the TLS private key.

**A reverse proxy remains an option, not the doctrine.** Where one host fronts several services with a single proxy, terminating TLS at the proxy and forwarding cleartext HTTP to `binary_port` on the loopback remains a valid deployment (the alternative block under Running under a supervisor above); behind a proxy the platform sees the proxy's address as the client, the usual forwarded-header tradeoff. The choice also has an assurance dimension: the native stack is a from-scratch TLS 1.3 implementation in interpreted LPC with no external audit (`docs/security-posture.md` Non-goals and known limits), so where that posture is insufficient the proxy path terminates TLS with a mainstream implementation.

**Exposure to abuse.** The platform ships no defense against a deliberate client: one peer can hold connection slots until the `users` cap silences every port (the console's included), and nothing in the image rate-limits, sheds, or evicts it (`docs/security-posture.md` Non-goals and known limits). Host-level controls own this surface: per-IP connection limits at the firewall (conntrack or equivalent), a fronting proxy's limits where one is deployed, and alerting on the `users` count as the early warning (Monitoring signals above). Plan the locked-out case before it happens: at the cap the driver refuses new connections only, so a console session already open keeps working -- hold one standing from the maintenance network, and its `reboot` verb still executes a controlled snapshot-and-exit from inside. Without one, the host is the fallback: SIGTERM writes the final incremental snapshot and exits (Running under a supervisor above), and the supervisor's restart clears the connection table.

## State file locations and permissions

The platform's persistent state lives in host files whose contents range from the object graph to admin credentials, so their filesystem permissions are part of the security posture.

| Path | Contents | Sensitivity |
|---|---|---|
| `dump_file` and `<dump_file>.old` | The persistent object graph: every object's variables, every clone's dataspace, pending `call_out`s | The entire application state |
| `swap_file` | Objects paged out of memory | Live object data, the same sensitivity as the snapshot |
| `src/kernel/data/` (`admin.pwd`, `access.data`) | The admin password hash and per-user access grants | Platform credentials |
| `/usr/System/log/system.log`, `/usr/Merry/log/dispatch.log` | Diagnostic and audit logs | Whatever application detail the logs record |
| Vault data directories (`/usr/Vault/data/vault/...`) | Schema-exported per-domain state | Application state |

Run the platform as a dedicated unprivileged user and keep each of these readable and writable only by that user: a restrictive `umask` on the process, files not group- or world-readable, containing directories not traversable by other users. The runtime user needs write access to the `dump_file` and `swap_file` directories. A permissions problem on the `dump_file` directory is a common cause of a failed dump (Common failure modes below). Back up the `dump_file` pair and `src/kernel/data/` to off-host storage for disaster recovery (Backing up and restoring state above). That backup carries the same credentials and state, so protect it the same way.

## Loading host-driver extensions

The platform loads no extensions by default. Optional extensions are loaded via the `.dgd` file's `modules` mapping:

```text
modules = ([ "/path/to/some-extension.1.5" : "module config" ]);
```

Each entry maps an extension's shared-object path to a configuration string the extension parses on load. The driver dlopens each module at boot, registers any kfuns it provides, and from that point on the extension's kfuns are callable from LPC alongside built-in kfuns. An LPC file calling some kfun cannot tell from the call shape whether the kfun is a host built-in or an extension.

Loading an extension is a durable architectural commitment, not an opt-in convenience. A snapshot taken with an extension active will require that same extension to restore: `statedumps created with a specific kfun extension in effect will require the the same kfun extension on restore` (from the [2010 Hydra mailing-list note](https://mail.dworkin.nl/pipermail/dgd/2010-August/006717.html)). Removing the extension and restoring the snapshot loses state. Plan extension choices accordingly.

The ecosystem provides extension bundles. The canonical one is [dworkin/lpc-ext]. An example extension from that bundle is an AOT-compiling JIT module that decompiles LPC bytecode to LLVM IR, invokes clang to produce per-program shared objects, disk-caches by program hash, and dispatches to native code at call time. The example illustrates the load pattern: a separate build step (`jitcomp`), the shared object's path in the `modules` mapping, the toolchain dependency (clang/LLVM). It is not a deployment recommendation. The platform has open empirical questions about how an extension-loaded JIT interacts with the platform's atomicity and hot-reload guarantees.

### Open empirical questions

The two runtime primitives with unverified extension behavior are atomicity (`docs/runtime-primitives.md` §1 Open) and hot reload (§4 Open). Both have the same shape: the platform guarantee holds without extensions. Whether it survives an extension-loaded codepath is unverified. Until verified, an operator enabling such an extension in production should treat these as known unknowns:

- **Atomicity under extension-loaded JIT.** Does the platform's atomic-commit rollback fire when an extension-compiled native function errors mid-call? The atomicity primitive (`docs/runtime-primitives.md` §1) hinges on the runtime restoring in-memory state on error. If extension-compiled code skips the rollback path (for example by writing directly to dataspace memory without going through the atomic-transaction layer), the guarantee holds only without the extension loaded.
- **Hot reload under extension-loaded compiled-code caches.** Does `compile_object(path, source)` interact correctly with an extension's per-program code cache? The hot-reload primitive (§4) requires that the next call after recompilation runs the new logic. If the extension's cache is keyed on something stale, recompiled code can be shadowed by previously-compiled native code.

Both questions are open. Empirical verification requires running the platform under each extension of interest and exercising the atomicity and hot-reload paths with the extension active. Once results land, this section will resolve to either "verified preserves" or "verified breaks" with a citation to the test result.

## Common failure modes

| Symptom | Likely cause | Diagnosis |
|---|---|---|
| Cold boot fails with compile error in initd cascade | Missing or broken `/usr/<Domain>/initd.c` | Check the message path. The driver names the file and line |
| Snapshot restore fails | Snapshot corrupt or `.dgd` config changed incompatibly (different `auto_object`, different `driver_object`, missing `modules` extension) | Restore from `<dump_file>.old`. If same failure, cold-boot from clean state |
| `dump_state` errors out | Disk full, permissions on `dump_file` directory, or snapshot exceeds available memory | Check disk space and permissions. Consider `swapout()` first |
| Platform dies with a fatal `out of sectors` error | The in-memory image outgrew the swap device: `swap_size` (capped at 65535 sectors in the stock build) times `sector_size` (Limits and capacity above) | Raise `sector_size` in the `.dgd` config and reboot from the latest snapshot. Alert on the `status` swap-sector percentage before it gets here (Monitoring signals above) |
| New connections open but nothing answers; existing service goes quiet | The `users` table is full: genuine concurrent load at the cap, or a leak outpacing reclaim -- a server that never releases completed requests holds each slot until the flow layer's 60-second inactivity timeout | At the cap both count surfaces (the `status` verb, the health route's `users=` line) need a connection the driver will no longer grant, so confirm from a console session already open -- or from outside: TCP connects that nothing ever answers, on every port at once, are this row. Recovery: a standing session's `reboot`, or SIGTERM from the host (the graceful stop writes the snapshot) and a supervisor restart; either clears the table but not the cause. Then find the leak (each HTTP example releases via `doneRequest()` when its response completes) or the abuser (Exposure to abuse above) |
| `system.log` grows without bound, repeating the same block | Error-manager feedback storm: an error caught inside an `atomic` function re-entering the error manager and looping a file write | Read the repeating block's trace for the atomic call site. `logd`'s deferred writes exist to prevent the class (Logging and diagnostics above); `scripts/base-boot-guard.sh` is its regression guard |
| Application kfun call returns "unknown function" or "extension not loaded" | A required `modules` entry is missing | Check `.dgd modules` mapping. Load the missing extension or remove the application code that depends on it |
| Per-owner ticks exhausted | Owner code is consuming ticks faster than `rsrc` allows | Use admin_console `quota` to inspect. Either raise the owner's quota or fix the looping code |
| `shutdown(1)` raises "Hotbooting is disabled" | The `.dgd` config has no `hotboot` tuple | Add the tuple to the `.dgd` config (see The .dgd configuration file above) and reboot, or use cold reboot via `shutdown()` instead |
| Hot boot fails after `execv` (platform exits without restore) | `hotboot` tuple's paths point at a different binary or config than the running one, or `execv` fails | Check the tuple's paths against the running process. Fall back to cold reboot via `shutdown()` |

## Where to next

- **[`docs/architecture.md`](architecture.md)** covers the platform tier model, daemons, and boot sequence in detail.
- **[`docs/runtime-primitives.md`](runtime-primitives.md)** covers the platform's eight runtime primitives, including the atomicity (§1) and hot-reload (§4) guarantees referenced above.
- **[`docs/application-authoring.md`](application-authoring.md)** covers writing a tier-E application on top of this platform.
- **DGD upstream reference** at <https://github.com/dworkin/dgd>: full kfun reference, `.dgd` field reference, host-binary build instructions.

[dworkin/lpc-ext]: https://github.com/dworkin/lpc-ext
