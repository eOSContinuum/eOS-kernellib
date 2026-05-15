<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# Operations

This document covers running an eOS-kernellib substrate: configuring it via the `.dgd` file, booting and re-booting it, snapshotting and restoring its persistent state, monitoring its output, diagnosing failures, and loading optional host-driver extensions. The architecture document (`doc/architecture.md`) covers the substrate's structural model; this document covers the operator's surface for keeping it running.

**Audience**: someone running the substrate — responsible for choosing config values, watching the running process, taking snapshots, restoring after a crash, and deciding whether to load extensions. Application authoring is covered in `doc/application-authoring.md` and `doc/http-applications.md`.

## The .dgd configuration file

The host driver reads its configuration from a `.dgd` file passed on the command line at boot. The fields below cover the operator-facing decisions; see the upstream DGD reference at <https://github.com/dworkin/dgd> for the full set.

| Field | Role |
|---|---|
| `directory` | The substrate's source root. The driver compiles LPC files relative to this directory and chroots its filesystem-style operations to it |
| `auto_object` | Path of the auto-inherited object; eOS-kernellib uses `/kernel/lib/auto` |
| `driver_object` | Path of the driver object; eOS-kernellib uses `/kernel/sys/driver` |
| `create` | Name of the driver-side create dispatcher; eOS-kernellib uses `_F_create` |
| `include_file` | Path of the standard include file; eOS-kernellib uses `/include/std.h` |
| `include_dirs` | Search path for `#include` directives |
| `telnet_port` | Address-and-port mapping for the telnet listener (admin_console binds here) |
| `binary_port` | Address-and-port mapping for binary listeners (HTTP and other application transports bind here) |
| `swap_file`, `swap_size`, `swap_fragment`, `sector_size` | Swap parameters governing how the substrate pages objects to disk |
| `static_chunk`, `dynamic_chunk` | Memory allocator chunk sizes |
| `dump_file` | Path the substrate writes snapshots to |
| `dump_interval` | Seconds between automatic snapshots; 3600 (one hour) is a reasonable default |
| `hotboot` | Tuple of `({ binary, config, snapshot, snapshot.old })` enabling hot boot via `execv` (see `doc/architecture.md` boot sequence) |
| `typechecking` | Strictness of compile-time type checks; production deployments should set `2` (full) |
| `users`, `editors`, `objects`, `call_outs`, `array_size` | Hard caps on substrate-wide resource counts |
| `modules` | Optional mapping of host-driver extensions to load at boot (see Loading host-driver extensions below) |

A minimal example is included at `example.dgd` in the repository root.

## Booting

The substrate has three boot modes; `doc/architecture.md` covers the dispatch in detail. Briefly:

- **Cold boot**: started with no snapshot present. The driver compiles `/kernel/sys/driver`, runs the kernel auto's initd cascade through every `/usr/[A-Z]*/initd.c`, and reaches the running state. This is the path for first-time bring-up and after intentional state wipe.
- **Snapshot restore**: started with a snapshot file present at `dump_file`. The driver reloads the snapshotted object graph and dataspaces, then calls the registered `restored(int hotboot)` driver hook. Initd cascades do not run; the substrate resumes the state captured at the snapshot.
- **Hot boot**: `shutdown(1)` followed by `execv` (when the `.dgd` file's `hotboot` tuple is set). Open file descriptors and connections are inherited by the replacement process; the snapshot is written and reloaded but external connections survive the transition. Used for upgrading the host binary or `.dgd` config without dropping live work.

## admin_console

`admin_console` is the operator REPL. It listens on `telnet_port` and offers a verb-based command surface for inspecting and manipulating the running substrate. Source lives at `src/kernel/lib/admin_console.c` and `src/kernel/obj/admin_console.c`.

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
| Substrate lifecycle | `shutdown`, `reboot` |

`code` is the LPC eval verb: it compiles its argument as an LPC expression in the operator's domain, evaluates it, and prints the result. See `doc/admin-console.md` for per-verb mechanics, operational scenarios, and the underlying kfun dispatch for each verb.

## State persistence

The substrate has three kfuns that operate on persistent state:

- `swapout()` — swap all in-memory objects to the swap file. Frees memory at the cost of paging on next access.
- `dump_state(int incr)` — write a snapshot to `dump_file`. With argument `0` (or no argument), writes a full snapshot. With non-zero argument, writes an incremental snapshot.
- `shutdown(int hotboot)` — shut down the substrate. The kfun does not snapshot before exiting; whether the next boot has a snapshot to restore from depends on whether one was taken (by automatic `dump_interval`, by the `snapshot` verb, or by an explicit `dump_state` call). With non-zero argument and a `hotboot` tuple in the `.dgd` config, performs a hot boot via `execv` instead of exiting; if the `hotboot` tuple is absent, the kfun raises "Hotbooting is disabled".

The admin_console wraps these: `swapout` calls `swapout()`, `snapshot` calls `dump_state(0)`, `shutdown` calls `shutdown()` (cold shutdown without snapshot), `reboot` calls `dump_state(1)` then `shutdown()` (incremental snapshot then cold shutdown). For a clean shutdown that leaves a current restore point, run `snapshot` then `shutdown`, or use `reboot`.

The driver also takes automatic snapshots at the interval set by `dump_interval`. The previous snapshot is moved to `<dump_file>.old` before the new one is written; if the running snapshot is corrupt, deleting it leaves `.old` in place for restore on next boot.

A snapshot captures the persistent object graph: every object's variables, every clone's dataspace, every pending `call_out`. It does not capture open connections. On snapshot restore, connections must be re-established by clients; on hot boot, connections survive via the inherited file descriptors. After a snapshot restore, the driver invokes the registered `restored(int hotboot)` hook (see `src/kernel/sys/driver.c`) which emits a "State restored" message and gives the substrate a place to re-attach managers and re-arm any external resources the snapshot did not preserve.

## Logging and diagnostics

The driver provides a `message(string)` function that timestamps and emits diagnostic output:

```c
ctime(time())[4..18] + " ** " + str
```

`message` is called by the driver during initialization, snapshot restore, and on interrupt. It is callable by kernel-tier and System-tier code; the underlying `send_message` kfun routes the output to the connection that triggered the current call (typically the operator at admin_console). Application-tier code does not invoke `message` directly.

For runtime errors, the driver dispatches through three hooks before falling back to a default formatter:

| Hook | When | Routed to |
|---|---|---|
| `runtime_error(str, caught, trace)` | Uncaught LPC error during normal execution | `errord->runtime_error()` |
| `atomic_error(str, atom, trace)` | Uncaught error inside an `atomic` function | `errord->atomic_error()` |
| `compile_error(file, line, str)` | LPC compilation failure | `errord->compile_error()` |

`errord` is registered via `driver->set_error_manager()`. eOS-kernellib ships an errord at `src/usr/System/sys/errord.c` that formats the trace into a readable form and sends it via `send_message` to the relevant operator. If no errord is registered or its handler raises, the driver falls back to a built-in formatter that walks the trace and emits via the same default channel; errors never silently disappear.

## Resource limits

Substrate-wide caps live in the `.dgd` file: `users`, `editors`, `objects`, `call_outs`, `array_size`. These are hard ceilings; the substrate refuses operations that would exceed them.

Per-owner limits are managed by the resource daemon at `/kernel/sys/resource_daemon` (registered as `resource_daemon` in the driver). Each owner has a quota covering object count, call_out count, ticks consumed per call, and stack depth. The admin_console `quota` and `rsrc` verbs read and write these. Per-owner ticks are charged on the owner's account when their code runs; an owner that exhausts ticks gets a runtime error and rollback rather than a hung substrate.

## Loading host-driver extensions

The substrate loads no extensions by default. Optional extensions are loaded via the `.dgd` file's `modules` mapping:

```text
modules = ([ "/path/to/some-extension.1.5" : "module config" ]);
```

Each entry maps an extension's shared-object path to a configuration string the extension parses on load. The driver dlopens each module at boot, registers any kfuns it provides, and from that point on the extension's kfuns are callable from LPC alongside built-in kfuns; an LPC file calling some kfun cannot tell from the call shape whether the kfun is a host built-in or an extension.

Loading an extension is a durable architectural commitment, not an opt-in convenience. A snapshot taken with an extension active will require that same extension to restore: `statedumps created with a specific kfun extension in effect will require the the same kfun extension on restore` (from the [2010 Hydra mailing-list note](https://mail.dworkin.nl/pipermail/dgd/2010-August/006717.html)). Removing the extension and restoring the snapshot loses state. Plan extension choices accordingly.

The ecosystem provides extension bundles. The canonical one is [dworkin/lpc-ext]; an example extension from that bundle is an AOT-compiling JIT module that decompiles LPC bytecode to LLVM IR, invokes clang to produce per-program shared objects, disk-caches by program hash, and dispatches to native code at call time. The example illustrates the load pattern: a separate build step (`jitcomp`), the shared object's path in the `modules` mapping, the toolchain dependency (clang/LLVM). It is not a deployment recommendation; the substrate has open empirical questions about how an extension-loaded JIT interacts with the substrate's atomicity and hot-reload guarantees.

### Open empirical questions

The two substrate primitives with unverified extension behavior are atomicity (`doc/substrate-primitives.md` §1 Open) and hot reload (§4 Open). Both have the same shape: the substrate guarantee holds without extensions; whether it survives an extension-loaded codepath is unverified. Until verified, an operator enabling such an extension in production should treat these as known unknowns:

- **Atomicity under extension-loaded JIT.** Does the platform's atomic-commit rollback fire when an extension-compiled native function errors mid-call? The atomicity primitive (`doc/substrate-primitives.md` §1) hinges on the runtime restoring in-memory state on error; if extension-compiled code skips the rollback path (for example by writing directly to dataspace memory without going through the atomic-transaction layer), the guarantee holds only without the extension loaded.
- **Hot reload under extension-loaded compiled-code caches.** Does `compile_object(path, source)` interact correctly with an extension's per-program code cache? The hot-reload primitive (§4) requires that the next call after recompilation runs the new logic; if the extension's cache is keyed on something stale, recompiled code can be shadowed by previously-compiled native code.

Both questions are open. Empirical verification requires running the substrate under each extension of interest and exercising the atomicity and hot-reload paths with the extension active. Once results land, this section will resolve to either "verified preserves" or "verified breaks" with a citation to the test result.

## Common failure modes

| Symptom | Likely cause | Diagnosis |
|---|---|---|
| Cold boot fails with compile error in initd cascade | Missing or broken `/usr/<Domain>/initd.c` | Check the message path; the driver names the file and line |
| Snapshot restore fails | Snapshot corrupt or `.dgd` config changed incompatibly (different `auto_object`, different `driver_object`, missing `modules` extension) | Restore from `<dump_file>.old`; if same failure, cold-boot from clean state |
| `dump_state` errors out | Disk full, permissions on `dump_file` directory, or snapshot exceeds available memory | Check disk space and permissions; consider `swapout()` first |
| Application kfun call returns "unknown function" or "extension not loaded" | A required `modules` entry is missing | Check `.dgd modules` mapping; load the missing extension or remove the application code that depends on it |
| Per-owner ticks exhausted | Owner code is consuming ticks faster than `rsrc` allows | Use admin_console `quota` to inspect; either raise the owner's quota or fix the looping code |
| `shutdown(1)` raises "Hotbooting is disabled" | The `.dgd` config has no `hotboot` tuple | Add the tuple to the `.dgd` config (see The .dgd configuration file above) and reboot; or use cold reboot via `shutdown()` instead |
| Hot boot fails after `execv` (substrate exits without restore) | `hotboot` tuple's paths point at a different binary or config than the running one, or `execv` fails | Check the tuple's paths against the running process; fall back to cold reboot via `shutdown()` |

## Where to next

- **`doc/architecture.md`** — substrate tier model, daemons, boot sequence in detail.
- **`doc/substrate-primitives.md`** — the substrate's eight runtime primitives, including the atomicity (§1) and hot-reload (§4) guarantees referenced above.
- **`doc/application-authoring.md`** — writing a tier-E application on top of this substrate.
- **DGD upstream reference** at <https://github.com/dworkin/dgd> — full kfun reference, `.dgd` field reference, host-binary build instructions.

[dworkin/lpc-ext]: https://github.com/dworkin/lpc-ext
