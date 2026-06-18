# Operations

This document covers running an eOS-kernellib instance: configuring it via the `.dgd` file, booting and re-booting it, snapshotting and restoring its persistent state, monitoring its output, diagnosing failures, and loading optional host-driver extensions. The architecture document (`docs/architecture.md`) covers the platform's structural model; this document covers the operator's surface for keeping it running.

**Audience**: someone running the platform — responsible for choosing config values, watching the running process, taking snapshots, restoring after a crash, and deciding whether to load extensions. Application authoring is covered in `docs/application-authoring.md` and `docs/http-applications.md`.

## The .dgd configuration file

The host driver reads its configuration from a `.dgd` file passed on the command line at boot. The fields below cover the operator-facing decisions; see the upstream DGD reference at <https://github.com/dworkin/dgd> for the full set.

| Field | Role |
|---|---|
| `directory` | The platform's source root. The driver compiles LPC files relative to this directory and chroots its filesystem-style operations to it |
| `auto_object` | Path of the auto-inherited object; eOS-kernellib uses `/kernel/lib/auto` |
| `driver_object` | Path of the driver object; eOS-kernellib uses `/kernel/sys/driver` |
| `create` | Name of the driver-side create dispatcher; eOS-kernellib uses `_F_create` |
| `include_file` | Path of the standard include file; eOS-kernellib uses `/include/std.h` |
| `include_dirs` | Search path for `#include` directives |
| `telnet_port` | Address-and-port mapping for the telnet listener (admin_console binds here) |
| `binary_port` | Address-and-port mapping for binary listeners (HTTP and other application transports bind here) |
| `swap_file`, `swap_size`, `swap_fragment`, `sector_size` | Swap parameters governing how the platform pages objects to disk |
| `static_chunk`, `dynamic_chunk` | Memory allocator chunk sizes |
| `dump_file` | Path the platform writes snapshots to |
| `dump_interval` | Seconds between automatic snapshots; 3600 (one hour) is a reasonable default |
| `hotboot` | Tuple of `({ binary, config, snapshot, snapshot.old })` enabling hot boot via `execv` (see `docs/architecture.md` boot sequence) |
| `typechecking` | Strictness of compile-time type checks; production deployments should set `2` (full) |
| `users`, `editors`, `objects`, `call_outs`, `array_size` | Hard caps on platform-wide resource counts |
| `modules` | Optional mapping of host-driver extensions to load at boot (see Loading host-driver extensions below) |

A minimal example is included at `example.dgd` in the repository root.

## Booting

The platform has three boot modes; `docs/architecture.md` covers the dispatch in detail. Briefly:

- **Cold boot**: started with no snapshot present. The driver compiles `/kernel/sys/driver`, runs the kernel auto's initd cascade through every `/usr/[A-Z]*/initd.c`, and reaches the running state. This is the path for first-time bring-up and after intentional state wipe.
- **Snapshot restore**: started with a snapshot file present at `dump_file`. The driver reloads the snapshotted object graph and dataspaces, then calls the registered `restored(int hotboot)` driver hook. Initd cascades do not run; the platform resumes the state captured at the snapshot.
- **Hot boot**: `shutdown(1)` followed by `execv` (when the `.dgd` file's `hotboot` tuple is set). Open file descriptors and connections are inherited by the replacement process; the snapshot is written and reloaded but external connections survive the transition. Used for upgrading the host binary or `.dgd` config without dropping live work.

## admin_console

`admin_console` is the operator REPL. It listens on `telnet_port` and offers a verb-based command surface for inspecting and manipulating the running platform. Source lives at `src/kernel/lib/admin_console.c` and `src/kernel/obj/admin_console.c`.

The verb categories below cover the shipped surface; each verb prints its own help when invoked without arguments.

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

- `swapout()` — swap all in-memory objects to the swap file. Frees memory at the cost of paging on next access.
- `dump_state(int incr)` — write a snapshot to `dump_file`. With argument `0` (or no argument), writes a full snapshot. With non-zero argument, writes an incremental snapshot.
- `shutdown(int hotboot)` — shut down the platform. The kfun does not snapshot before exiting; whether the next boot has a snapshot to restore from depends on whether one was taken (by automatic `dump_interval`, by the `snapshot` verb, or by an explicit `dump_state` call). With non-zero argument and a `hotboot` tuple in the `.dgd` config, performs a hot boot via `execv` instead of exiting; if the `hotboot` tuple is absent, the kfun raises "Hotbooting is disabled".

The admin_console wraps these: `swapout` calls `swapout()`, `snapshot` calls `dump_state(0)`, `shutdown` calls `shutdown()` (cold shutdown without snapshot), `reboot` calls `dump_state(1)` then `shutdown()` (incremental snapshot then cold shutdown). For a clean shutdown that leaves a current restore point, run `snapshot` then `shutdown`, or use `reboot`.

The driver also takes automatic snapshots at the interval set by `dump_interval`. The previous snapshot is moved to `<dump_file>.old` before the new one is written; if the running snapshot is corrupt, deleting it leaves `.old` in place for restore on next boot.

A snapshot captures the persistent object graph: every object's variables, every clone's dataspace, every pending `call_out`. It does not capture open connections. On snapshot restore, connections must be re-established by clients; on hot boot, connections survive via the inherited file descriptors. After a snapshot restore, the driver invokes the registered `restored(int hotboot)` hook (see `src/kernel/sys/driver.c`) which emits a "State restored" message and gives the platform a place to re-attach managers and re-arm any external resources the snapshot did not preserve.

## Logging and diagnostics

The driver provides a `message(string)` function that timestamps and emits diagnostic output:

```c
ctime(time())[4..18] + " ** " + str
```

`message` is called by the driver during initialization, snapshot restore, and on interrupt. It is callable by kernel-tier and System-tier code; the underlying `send_message` kfun routes the output to the connection that triggered the current call (typically the operator at admin_console). Application-tier code does not invoke `message` directly.

The platform's general diagnostic facility is `logd`, a System-tier daemon at `/usr/System/sys/logd`. It owns a single persistent sink — `/usr/System/log/system.log` — the emission threshold, and the operator surface. The three diagnostic calls platform and application code already carry, `debugLog` / `info` / `sysLog` (defined in `/lib/util/lpc.c`), forward to `logd`, each mapped to a fixed severity:

| Call | Level | Intended use |
|---|---|---|
| `debugLog(str)` | DEBUG | developer tracing |
| `info(str)` | INFO | routine progress |
| `sysLog(str)` | NOTICE | general system events |

Levels are ordered ascending by severity (DEBUG < INFO < NOTICE < ERROR); ERROR is reserved for `errord`'s reports (see runtime errors below). `logd` drops any message below its threshold, and the threshold defaults to INFO — so `debugLog` output is suppressed until an operator lowers it. The forwarders reach the daemon by `call_other` rather than inheritance, so code in any tier can log without the `/kernel/lib` inheritance restriction that constrains the capability library (`docs/capability.md`); a `find_object` guard turns a log call into a no-op during the boot window before `logd` is loaded, rather than an error.

`logd` never writes synchronously. DGD forbids `write_file` inside an `atomic` function, and the diagnostic calls fire from atomic contexts (Schema import, the property-change dispatcher). So each call buffers its line in memory and schedules a single coalesced `call_out(0)`; the flush appends the buffered batch to `system.log` in a fresh, non-atomic execution where the write is legal, and echoes NOTICE-and-above lines to the operator console (via `message`) for a live view. A line logged inside a committed atomic is written after the atomic commits; a line logged inside an atomic that rolls back is discarded with the buffer — its work did not happen, so its progress log is moot. The deferral also makes logging non-throwing, which is load-bearing: the driver notifies `errord` of even caught errors, so a synchronous in-atomic write failure would feed back through `errord` into logging and storm.

Two admin_console verbs, registered through `admin_console_registry`, are the operator surface:

- `log [N]` — tail the last N lines of `system.log` (default 40, bounded to the final 8 KB so a large unrotated log does not load wholesale). Read-only; rides the console's existing privilege.
- `log-level [LEVEL]` — with no argument, report the current threshold; with `debug` / `info` / `notice` / `error`, set it. The set path mutates daemon state, so it is capability-gated: the verb routes through the registry's KERNEL-elevation helper, which checks the `admin_console.caller` capability via `capabilityd` (`docs/capability.md`) before applying the change. `logd` is the first System-tier consumer of the capability library.

`logd` appends and never prunes; rotation and retention are ordinary log-management tooling's responsibility — the daemon builds in no rotation policy, the same posture the property-change dispatcher's audit log takes (below).

For runtime errors, the driver dispatches through three hooks before falling back to a default formatter:

| Hook | When | Routed to |
|---|---|---|
| `runtime_error(str, caught, trace)` | Uncaught LPC error during normal execution | `errord->runtime_error()` |
| `atomic_error(str, atom, trace)` | Uncaught error inside an `atomic` function | `errord->atomic_error()` |
| `compile_error(file, line, str)` | LPC compilation failure | `errord->compile_error()` |

`errord` is registered via `driver->set_error_manager()`. eOS-kernellib ships an errord at `src/usr/System/sys/errord.c` that formats the trace into a readable form and sends it via `send_message` to the relevant operator. In addition to the console, `errord` drains each formatted report into `logd`'s sink at ERROR level (its `persist` path), so error diagnostics are durable rather than console-transient. This is how the platform's error reporting survives the atomic barrier: an `atomic_error` is dispatched only after its failed atomic has already rolled back, so the diagnostics would otherwise vanish with the rollback — the driver carries the trace across the barrier in thread-local storage, `errord` formats it post-rollback, and the `logd` tee persists it to `system.log`. If no errord is registered or its handler raises, the driver falls back to a built-in formatter that walks the trace and emits via the same default channel; errors never silently disappear.

The property-change dispatcher writes a per-failure audit line to `/usr/Merry/log/dispatch.log` on observer-cycle detection, cascade-depth overflow, and observer-source compile failures. Volume is low under normal operation (writes only on detected failures); the log is rotated by ordinary log-management tooling — the dispatcher does not build in a rotation policy. See `docs/dispatcher.md` Audit log.

The same log file also receives optional verbose-trace lines when the `dispatch_trace` flag is on. Toggle via the admin verb `dispatch-trace on|off|status` (see `docs/admin-console.md` Dispatcher operator surface) or via `MERRY->set_dispatch_trace(int flag)` (KERNEL-gated; public read via `MERRY->query_dispatch_trace()`). Default is off; trace lines elide their I/O entirely when the flag is unset. When on, the current scope writes one trace line per `dispatch_set` entry (object name + path); additional trace sites are future-work. Leaving trace on during steady-state operation increases log volume; intended for operator-driven troubleshooting sessions.

## Resource limits

Platform-wide caps live in the `.dgd` file: `users`, `editors`, `objects`, `call_outs`, `array_size`. These are hard ceilings; the platform refuses operations that would exceed them.

Per-owner limits are managed by the resource daemon at `/kernel/sys/resource_daemon` (registered as `resource_daemon` in the driver). Each owner has a quota covering object count, call_out count, ticks consumed per call, and stack depth. The admin_console `quota` and `rsrc` verbs read and write these. Per-owner ticks are charged on the owner's account when their code runs; an owner that exhausts ticks gets a runtime error and rollback rather than a hung platform.

The property-change dispatcher (`docs/dispatcher.md`) exposes a runtime-configurable cascade-depth bound via `MERRY->set_max_cascade_depth(int n)` (KERNEL-gated) and `MERRY->query_max_cascade_depth()` (public read-only). Default is `32`. The bound counts depth, not breadth — a flat batched write with many keys does not increment the counter; an observer-triggered chain of further writes does. Hitting the bound throws `merry: cascade depth N exceeded ...` and records `cascade-aborted` in the dispatcher's batch-status log.

The admin verb `cascade-depth [N]` (see `docs/admin-console.md` Dispatcher operator surface) is the operator-facing read/write surface; the no-arg form reports the current value, the integer-arg form sets it via the registry's KERNEL-elevation helper. The dispatcher additionally exposes nine operator verbs from the admin console for runtime inspection and mutation of dispatcher state (observers, batch-status, observer-registration, approved-registrar set); `docs/admin-console.md` enumerates the full set and the worked-example operator session.

## Loading host-driver extensions

The platform loads no extensions by default. Optional extensions are loaded via the `.dgd` file's `modules` mapping:

```text
modules = ([ "/path/to/some-extension.1.5" : "module config" ]);
```

Each entry maps an extension's shared-object path to a configuration string the extension parses on load. The driver dlopens each module at boot, registers any kfuns it provides, and from that point on the extension's kfuns are callable from LPC alongside built-in kfuns; an LPC file calling some kfun cannot tell from the call shape whether the kfun is a host built-in or an extension.

Loading an extension is a durable architectural commitment, not an opt-in convenience. A snapshot taken with an extension active will require that same extension to restore: `statedumps created with a specific kfun extension in effect will require the the same kfun extension on restore` (from the [2010 Hydra mailing-list note](https://mail.dworkin.nl/pipermail/dgd/2010-August/006717.html)). Removing the extension and restoring the snapshot loses state. Plan extension choices accordingly.

The ecosystem provides extension bundles. The canonical one is [dworkin/lpc-ext]; an example extension from that bundle is an AOT-compiling JIT module that decompiles LPC bytecode to LLVM IR, invokes clang to produce per-program shared objects, disk-caches by program hash, and dispatches to native code at call time. The example illustrates the load pattern: a separate build step (`jitcomp`), the shared object's path in the `modules` mapping, the toolchain dependency (clang/LLVM). It is not a deployment recommendation; the platform has open empirical questions about how an extension-loaded JIT interacts with the platform's atomicity and hot-reload guarantees.

### Open empirical questions

The two runtime primitives with unverified extension behavior are atomicity (`docs/runtime-primitives.md` §1 Open) and hot reload (§4 Open). Both have the same shape: the platform guarantee holds without extensions; whether it survives an extension-loaded codepath is unverified. Until verified, an operator enabling such an extension in production should treat these as known unknowns:

- **Atomicity under extension-loaded JIT.** Does the platform's atomic-commit rollback fire when an extension-compiled native function errors mid-call? The atomicity primitive (`docs/runtime-primitives.md` §1) hinges on the runtime restoring in-memory state on error; if extension-compiled code skips the rollback path (for example by writing directly to dataspace memory without going through the atomic-transaction layer), the guarantee holds only without the extension loaded.
- **Hot reload under extension-loaded compiled-code caches.** Does `compile_object(path, source)` interact correctly with an extension's per-program code cache? The hot-reload primitive (§4) requires that the next call after recompilation runs the new logic; if the extension's cache is keyed on something stale, recompiled code can be shadowed by previously-compiled native code.

Both questions are open. Empirical verification requires running the platform under each extension of interest and exercising the atomicity and hot-reload paths with the extension active. Once results land, this section will resolve to either "verified preserves" or "verified breaks" with a citation to the test result.

## Common failure modes

| Symptom | Likely cause | Diagnosis |
|---|---|---|
| Cold boot fails with compile error in initd cascade | Missing or broken `/usr/<Domain>/initd.c` | Check the message path; the driver names the file and line |
| Snapshot restore fails | Snapshot corrupt or `.dgd` config changed incompatibly (different `auto_object`, different `driver_object`, missing `modules` extension) | Restore from `<dump_file>.old`; if same failure, cold-boot from clean state |
| `dump_state` errors out | Disk full, permissions on `dump_file` directory, or snapshot exceeds available memory | Check disk space and permissions; consider `swapout()` first |
| Application kfun call returns "unknown function" or "extension not loaded" | A required `modules` entry is missing | Check `.dgd modules` mapping; load the missing extension or remove the application code that depends on it |
| Per-owner ticks exhausted | Owner code is consuming ticks faster than `rsrc` allows | Use admin_console `quota` to inspect; either raise the owner's quota or fix the looping code |
| `shutdown(1)` raises "Hotbooting is disabled" | The `.dgd` config has no `hotboot` tuple | Add the tuple to the `.dgd` config (see The .dgd configuration file above) and reboot; or use cold reboot via `shutdown()` instead |
| Hot boot fails after `execv` (platform exits without restore) | `hotboot` tuple's paths point at a different binary or config than the running one, or `execv` fails | Check the tuple's paths against the running process; fall back to cold reboot via `shutdown()` |

## Where to next

- **`docs/architecture.md`** — platform tier model, daemons, boot sequence in detail.
- **`docs/runtime-primitives.md`** — the platform's eight runtime primitives, including the atomicity (§1) and hot-reload (§4) guarantees referenced above.
- **`docs/application-authoring.md`** — writing a tier-E application on top of this platform.
- **DGD upstream reference** at <https://github.com/dworkin/dgd> — full kfun reference, `.dgd` field reference, host-binary build instructions.

[dworkin/lpc-ext]: https://github.com/dworkin/lpc-ext
