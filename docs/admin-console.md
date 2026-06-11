# admin_console

The operator's console: a verb-based REPL that binds to the kernel's telnet port and exposes the platform's introspection, code-lifecycle, persistence, permissions, and resource surfaces. This document is the operator's reference; `docs/operations.md` is the deployment surface (configuration, boot modes, statedump cadence, extension loading).

**Audience**: an operator with admin-console access to a running platform; needs to introspect state, hot-fix code, manage permissions, snapshot, or shut down.

The console is implemented in two files:

- `src/kernel/lib/admin_console.c` — the library (~2,300 lines of LPC). Defines every verb, the parser, the history table, the dispatch.
- `src/kernel/obj/admin_console.c` — the clonable. Each operator connection clones this object; the clone holds per-session state (current directory, code-history values, the inherited library's user-tier configuration).

A System-tier subclass at `/usr/System/sys/userd.c` registers the console as the connection handler bound to the kernel's telnet port (named `telnet_port` in the `.dgd` configuration).

## Why a console at all

The platform is an in-memory runtime. State lives in objects; code lives in compiled programs; persistence is a runtime property. None of that is reachable through the host operating system's tools — `ls` on the host filesystem shows the LPC source, not the live object graph; `kill -SIGUSR1` does not snapshot the runtime. Operations on the running platform go through a runtime-aware interface.

admin_console is that interface. Its eight categories of verbs map to eight categories of operational work:

| Category | What it does | Underlying mechanism |
|---|---|---|
| **State inspection** | Query the live image: objects, owners, resources, connections, system health | Host introspection kfuns (`find_object`, `status`, `query_owners`, `query_users`, `get_dir`) |
| **Code lifecycle** | Compile, clone, destruct, instantiate LWOs; recompile in place | `compile_object`, `clone_object`, `destruct_object`, `new_object` |
| **REPL** | Evaluate LPC expressions interactively; replay history | `compile_object` against a temporary path |
| **Filesystem** | Navigate the platform's source tree; read and write source files | `get_dir`, `read_file`, `write_file`, `rename_file`, etc. — all gated by per-tier access checks |
| **Editor** | Edit LPC source via DGD's built-in line editor | DGD's `ed` kfun (the LPC `editor` kfun) |
| **Permissions** | Inspect and modify per-user access bits | Kernel access daemon API (`set_access`, `query_user_access`, `query_file_access`) |
| **Resources** | Inspect and modify per-owner resource limits | Resource daemon API (`rsrc_set_limit`, `rsrc_get`) |
| **Persistence and lifecycle** | Swap out memory; snapshot; shutdown; reboot | Host kfuns `swapout`, `dump_state`, `shutdown` |

Each verb is a thin LPC wrapper over a host kfun or a daemon method. The `code` verb is the most general: it compiles its argument as an LPC expression and runs it, which means an operator with sufficient access can reach anything the platform exposes — the verb set is convenience over `code`, not capability beyond it.

## Connecting

The console listens on the address-and-port set by the `.dgd` configuration's `telnet_port` (default `8023`):

```sh
telnet localhost 8023
```

First-connection behavior depends on whether the kernel has admin credentials persisted:

- **Cold boot, no prior admin**: the kernel prompts to set an admin password. The hash is persisted across statedumps (runtime primitive §3), so subsequent boots find it.
- **Subsequent connections**: name and password prompt; the hash is checked against the persisted credential.

An operator authenticated as `admin` has tier-spanning reach (kernel and System tiers, plus cross-domain visibility into user-tier code). Other authenticated operators have access bounded by their owner's directory tree and the access-daemon grants on top.

## Security posture

The admin_console is the platform's most dangerous interface and the platform's most useful interface. Two facts shape its security model:

1. **The `code` verb evaluates arbitrary LPC.** An operator who can invoke `code` can call any kfun the inheriting object has access to. For `admin`, that means every kfun in the runtime. Treat console access as equivalent to host shell access on the platform's process.
2. **Permissions inside the console enforce the platform's tier model.** Non-admin operators see only what their owner permits. `code` invocations still hit per-call access checks at every kfun boundary; a non-admin operator's `code` invocation cannot bypass the tier model.

In production deployments, expose the telnet port only on a loopback interface or a dedicated maintenance VLAN. The console wire protocol is unencrypted telnet; remote operators should reach the port through an SSH or TLS tunnel.

## Operational tasks

The verb categories below are organized around the operational situation an operator is in — not alphabetically. Each section names the verbs in play, the platform mechanism behind them, and the scenario where the choice of one verb over another matters.

### Inspecting runtime state

**Verbs**: `status`, `code`, `people`, `pwd`, `cd`, `ls`, `history`

**Why**: the platform's value is that the running image is queryable; you do not need a debugger attached to know what the runtime is doing. `status` walks the host's system-wide health vector (memory, swap, objects, users, uptime, call_outs); `code` evaluates an arbitrary LPC expression in the operator's domain and returns the result with history-pointer assignment (`$N`); `people` enumerates live connections.

**How**:

- `status` without arguments calls the host `status()` kfun with no argument and pretty-prints the result (memory used, memory free, swap configuration, object count, free objects, call_out count, free call_outs, uptime, last reboot, daemon registration state).
- `status <obj>` calls `status(obj)` for per-object status: program path, owner, clone count, dataspace size.
- `code <expr>` compiles `<expr>` as the body of a synthesized `mixed exec(object user, mixed... argv) { … }` function in a transient object at `/usr/<owner>/_code`, calls it, prints the result, stores the result in the history table under `$N`, and destructs the transient object. The standard includes (`<float.h>`, `<limits.h>`, `<status.h>`, `<trace.h>`, `<type.h>`) plus the operator's optional `~/include/code.h` are prepended; 26 single-letter `mixed` variables `a` through `z` are pre-declared for ad-hoc use.
- `history [N]` prints the last `N` (default 10) values from the code-history ring buffer.
- `clear` empties the history ring buffer.
- `pwd` prints the operator's current directory; `cd <path>` changes it; `ls [-l] [<glob>]` lists files in the current or named directory.

**What for**:

- **Triage a stuck request**: invoke `status` to inspect resource ceilings, call_out backlog, swap activity. If the platform is healthy, the issue is logical, not capacity.
- **Inspect application state**: `code "/usr/MyApp/sys/daemon"->query()` returns the daemon's current state value without restarting.
- **Trace a value back through history**: when a sequence of `code` invocations has produced a chain of `$N` results, `history` shows the chain; `code $5->something()` walks one of them.
- **Locate a file**: `cd /usr/MyApp/obj && ls` enumerates the cloneable masters in the application's `obj/` subdirectory.

### Hot-fixing code in production

**Verbs**: `compile`, `clone`, `destruct`, `new`

**Why**: hot reload is a runtime primitive — `compile_object(path)` against a path with an existing master replaces the master's program; subsequent calls dispatch to the new version (`docs/runtime-primitives.md` §4). The console exposes this mechanism plus the clone/destruct/instantiate cycle that surrounds it.

**How**:

- `compile <file.c> [<file.c> ...]` calls `compile_object()` on each path (stripped of the `.c` suffix). The verb expands shell-style glob patterns in the current directory; each successful compile stores the master object in the history table.
- `clone <obj> | $N` calls `clone_object()` on the named master (or the historical result `$N`). Stores the clone in history.
- `destruct <obj> | $N` calls `destruct_object()` on the named object. Verifies the object exists; reports the error if not.
- `new <obj> | $N` calls `new_object()` to instantiate a Light-Weight Object (LWO) from a `data/` master. Distinct from `clone`: LWOs are value-shaped (no separate identity), `clone` creates first-class clones.

**What for**:

- **Emergency bug fix in a running service**:
  1. Edit the source file (via `ed` or via the host filesystem).
  2. `compile /usr/MyApp/obj/route_handler.c` — replaces the master. In-flight calls finish on the old code; the next call uses the new code.
  3. Verify with `code "/usr/MyApp/obj/route_handler"->probe()` (or an equivalent test invocation).
  
  No restart; no disconnect.
- **Library upgrade**: recompiling a library (`/usr/MyApp/lib/util.c`) replaces the library master, but existing children of the library do not automatically pick up the new parent. Two options:
  - Destruct each child (`destruct /usr/MyApp/obj/foo`) and recompile (`compile /usr/MyApp/obj/foo.c`). Loses clone state.
  - Use `call_touch` (via `code`) to mark every dependent for lazy upgrade through `_F_touch()`. Preserves state.

  Platform-level recompile-with-dependents (an `upgrade` verb backed by a `progdb`-style dependency daemon) is not currently shipped in this kernel layer; manual coordination is the operator's responsibility.
- **Recover from a wedged daemon**: `destruct /usr/MyApp/sys/router` then `clone /usr/MyApp/sys/router` would re-instantiate from the master — except `sys/` daemons are singletons that compile at boot, not on demand. The recovery is `destruct` followed by `compile` of the daemon source.
- **A/B testing**: keep the canonical handler at `/usr/MyApp/obj/handler.c`; copy it to `handler_b.c`; compile the variant; route a percentage of traffic to the variant; compile the winner back as `handler.c` when results are in.

### Snapshot, restore, and shutdown

**Verbs**: `swapout`, `snapshot`, `shutdown`, `reboot`

**Why**: orthogonal persistence means the snapshot IS the backup. The platform's `dump_state` kfun captures the entire in-memory image to disk; the next boot restores from that image with all object state, all clones, all pending `call_out`s, all access bits, all resource counters intact. Connections do not survive a snapshot restore (open file descriptors are not in the snapshot); on hot boot they do (the file descriptors are inherited across `execv`).

**How**:

- `swapout` calls `swapout()`. Swaps every in-memory object to disk (the swap file), reducing memory pressure. Next access faults the object back in. Useful before a snapshot — a swapped-out image fits more cleanly into the snapshot.
- `snapshot` calls `dump_state(0)` (full image dump). Writes to `dump_file` per the `.dgd` configuration. The previous snapshot moves to `<dump_file>.old`. Cost: I/O for the full image size; runtime briefly blocks.
- `shutdown` calls `shutdown()` (cold shutdown without snapshot). The platform exits; the next boot is a cold boot OR a snapshot-restore from the most recent snapshot (depending on whether the snapshot file is present and valid).
- `reboot` calls `dump_state(1)` (incremental snapshot) followed by `shutdown()`. Effectively: snapshot-and-stop. Next boot restores from the new snapshot.

**What for**:

- **Pre-deployment safety net**: `snapshot` before applying a substantial code change. If the change wedges the platform, restore from `dump_file.old`.
- **Scheduled rotation**: the platform writes automatic snapshots every `dump_interval` seconds (config field). `snapshot` is the operator-on-demand variant; useful before maintenance windows.
- **Recovery from a wedge**: `reboot` returns the platform to the last consistent state. Distinct from a host-level `kill`: `reboot` ensures the next boot starts from a consistent committed snapshot.
- **Hot binary upgrade**: requires the `.dgd` configuration's `hotboot` tuple. Run `code "/usr/System/sys/userd"->hotboot()` (or operator-equivalent) to invoke `execv` against the new binary; connections survive; state restores from the freshly-written snapshot.

Cold shutdown (`shutdown`) leaves no snapshot of its own; the platform restarts from whichever snapshot was last written by `dump_interval` or by an explicit `snapshot`/`reboot`. Plan accordingly: a busy platform with `dump_interval = 3600` and no `snapshot` between rotations loses up to an hour of state on a cold-shutdown-then-recover cycle.

### Managing permissions

**Verbs**: `access`, `grant`, `ungrant`

**Why**: capability separation is a runtime primitive (§2). The kernel access daemon at `/kernel/sys/access_daemon` mediates every cross-domain operation. The console verbs are the operator interface to that daemon.

**How**:

- `access` without arguments prints the current operator's access bits.
- `access <user>` prints another user's access bits.
- `access <directory>` prints who has access to the directory.
- `access global` prints which directories permit global read.
- `grant <user> <directory> [read | write | full]` calls `set_access()` with the directory and mode (default `write` when the mode keyword is omitted).
- `grant <user> access` creates the user's file access itself (adds the owner and creates `/usr/<user>`).
- `grant global <directory>` adds a `/usr/`-subdirectory to the global-read set.
- `ungrant <user> <directory>` removes the grant; `ungrant <user> access` removes the user's file access; `ungrant global <directory>` removes a global-read entry.

**What for**:

- **Onboard a developer**: `grant developer access` creates their owner tree; `grant developer /usr/SharedLib read` — they can now read the shared library but not modify it.
- **Investigate a permission denied**: `access <user>` shows the user's current bits; `access <directory>` shows who has access to the target. Compare to find the missing grant.
- **Lock down a sensitive directory**: `ungrant <user> /usr/Secrets` removes their access; `access /usr/Secrets` confirms no one else has unexpected reach.

### Managing resources

**Verbs**: `quota`, `rsrc`

**Why**: every owner has a resource quota — object count, call_out count, ticks per call, stack depth. The resource daemon (`/kernel/sys/resource_daemon`) tracks usage and enforces the quota at every relevant kfun call. An owner that exhausts their tick budget gets a runtime error (which rolls back the offending atomic context — runtime primitive §1).

**How**:

- `quota` with no arguments prints the operator's own quota and current consumption.
- `quota <user>` prints another user's quota.
- `quota <user> <rsrc>` shows usage of a specific resource (ticks, stack, callouts, objects, fileblocks).
- `quota <user> <rsrc> <limit>` sets the limit. `-1` means unlimited (for the resources that permit unlimited).
- `rsrc` with no arguments shows platform-wide totals across all owners.
- `rsrc <resource>` shows per-owner breakdown for a resource.
- `rsrc <resource> <limit>` sets the platform-wide cap (the value also writable via the `.dgd` configuration; this is the runtime override).

**What for**:

- **Diagnose a runaway loop**: `rsrc ticks` shows which owner is consuming ticks. The offending domain's code is exhausting its budget — typically an infinite loop or an unbounded recursion.
- **Adjust a per-owner budget**: a legitimate workload that hits the ticks ceiling needs `quota <owner> ticks <higher-limit>` — but consider whether the workload should be split into multiple smaller atomic contexts (via `call_out`) before raising the limit.
- **Capacity planning**: `rsrc objects` and `rsrc callouts` show how close the platform is to its hard caps (`objects`, `call_outs` in `.dgd`). Approaching the cap means a config increase is due.

### Editing files

**Verbs**: `ed`

**Why**: operators sometimes need to edit platform source files from within the platform — when the host filesystem is not directly accessible (the kernel's `directory` is chrooted), or when the platform is the host's only interactive entry point. DGD ships a line editor reachable through the host's `editor` kfun; the `ed` verb is the operator wrapper.

**How**: `ed <file>` invokes the host `editor` kfun against the named file. The verb is a thin wrapper. The editor is a classic line-oriented editor (the verb name `ed` reflects this); navigation by line number, search by regular expression, edits by line range. See the host's editor reference for the command set.

**What for**:

- **Quick fix without host access**: a one-line patch to a wedged daemon when the operator does not have shell access to the host machine.
- **Cross-platform edit consistency**: the platform editor handles line endings consistently regardless of the host platform — useful when host editors might write `\r\n` files that DGD does not accept.

For substantial edits, prefer a host-side editor. The `ed` verb is for cases where it's the only option.

### Filesystem navigation and manipulation

**Verbs**: `cd`, `pwd`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`

**Why**: the platform exposes a virtual filesystem rooted at the `.dgd` configuration's `directory` setting. All filesystem-style kfuns (`read_file`, `write_file`, `get_dir`, etc.) are gated by the per-tier access checks. Operating on these files through the console (rather than the host OS) enforces the tier model — `rm` on a file the operator does not own fails the same access check that an LPC `remove_file()` call would fail. The verbs name themselves after Unix equivalents to minimize surprise; the underlying behavior is access-checked, not raw filesystem.

**How**: each verb maps to a host kfun, with the access daemon's per-call check applied at every operation:

| Verb | Kfun(s) | Notes |
|---|---|---|
| `cd <dir>` | `get_dir`, `file_info` | Updates the operator's session directory; verifies the target exists and is a directory |
| `pwd` | (internal) | Prints session directory; never errors |
| `ls [-l] [<glob>]` | `get_dir`, `file_info` | `-l` shows long format with size and timestamp |
| `cp <file> [...] <target>` | `read_file`, `write_file` | Copies one or more files to target; access-checked at each side |
| `mv <file> [...] <target>` | `rename_file` | Renames or moves one or more files; preserves access bits when allowed by tier |
| `rm <file> [...]` | `remove_file` | Removes one or more files; access-checked against the operator; refuses directories |
| `mkdir <directory> [...]` | `make_dir` | Creates directories under the operator's owner tree |
| `rmdir <directory> [...]` | `remove_dir` | Removes empty directories |

**What for**:

- **Survey an unfamiliar domain**: `cd /usr/Stranger && ls` enumerates the domain's structure. Use `ls -l` to see file sizes (LPC source size is a rough proxy for daemon complexity).
- **Stage a deploy**: `cp /usr/MyApp/obj/handler.c /usr/MyApp/obj/handler_old.c` keeps a rollback copy. Edit the original; if the change misbehaves, `mv /usr/MyApp/obj/handler_old.c /usr/MyApp/obj/handler.c` followed by `compile` restores.
- **Reorganize a domain**: `mkdir /usr/MyApp/lib/util && mv /usr/MyApp/lib/strings.c /usr/MyApp/lib/util/`. The driver's `inherit_program` constraint requires `/lib/` in the inherited path — moves that preserve `/lib/` survive; moves that drop it break inheritance.

### Observing connections

**Verbs**: `people`

**Why**: the platform's connection layer (`/kernel/sys/userd`) maintains the live list of active connections. Operators inspect that list for capacity planning, incident response, and security audit.

**How**: `people` queries the host's `query_users()` kfun and prints, per user: connection state, address, current command (when in the editor), idle time. The verb name reflects the runtime's MUD heritage; the underlying mechanism is connection enumeration.

**What for**:

- **Incident response**: who's connected when the platform is misbehaving? Compare against expected operator population.
- **Capacity check**: connection count against the `users` cap in `.dgd`.
- **Audit**: combine with `status` for full snapshot of who-is-on plus what-is-going-on.

## Dispatcher operator surface

**Verbs**: `observers`, `cascade-depth`, `batch-status`, `dispatch-trace`, `register-observer`, `unregister-observer`, `query-approved-registrars`, `approve-registrar`, `unapprove-registrar`

**Why**: the Merry dispatcher (`docs/dispatcher.md`) routes every property-change observation through a substrate that maintains observer registrations, cascade-depth bounds, cycle detection, and batch-status accounting. Operators investigating dispatcher behavior at runtime — debugging a deep cascade, auditing observer registrations after a deploy, toggling verbose trace for a session — need first-class verbs rather than long-form `code MERRY->_query_batch_status(7)` invocations.

**How**: these verbs are not built into the kernel admin console (its built-in set is the kernel-tier categories enumerated above). They live in `src/usr/Merry/lib/admin_console_ext.c` and reach the console via the selective-extension model:

- A KERNEL-tier registry at `/kernel/sys/admin_console_registry` holds a `verb -> (extension_path, method_name)` dispatch table. Merry's nine verbs are hardcoded into the registry's `create()`.
- The console's unknown-verb switch default consults the registry; if it finds an entry and the extension master is loaded, it dispatches there.
- Mutation verbs whose underlying daemon LFUNs are KERNEL-gated (`approve-registrar`, `unapprove-registrar`, `cascade-depth N`, `dispatch-trace on|off`) or capability-gated by caller-domain (`register-observer`, `unregister-observer`) route through the registry's `verb_*` elevation helpers. The registry is KERNEL-tier, so the daemon's gates pass; the registry's own caller-program whitelist constrains which extensions may use the elevation surface.

The kernel admin console itself remains MERRY-unaware. Future operator surfaces for Vault, Schema, or HTTP extend the registry's hardcoded list the same way. (When a cross-subsystem capability model lands, dynamic registration replaces the hardcoded table; the dispatch shape is unchanged.)

**What for**:

- **Observer audit**: `observers <obj_path> <path>` shows every compiled observer registered for a property on the target, grouped by timing. Useful before a Merry-script redeploy to confirm what the substrate believes about the current registration state. See the target-resolution note below for which objects the `<obj_path>` argument can reach.
- **Cascade-depth tuning**: `cascade-depth` reads the current bound; `cascade-depth 64` raises it after a legitimately-deep cascade scenario has been verified safe. Statedump-persistent.
- **Batch forensics**: `batch-status 7` shows whether batch 7 completed, was atomically aborted, cycle-aborted, or vetoed by a pre-observer; the reason field carries the propagated error for aborted batches. Useful when a dispatcher event surfaces in the application log and the batch-id is the only pointer back to the substrate's view.
- **Tracing**: `dispatch-trace on` enables per-`dispatch_set` entry logging to `/usr/Merry/log/dispatch.log` alongside the always-on cycle and cascade events; `dispatch-trace off` returns to the silent default. Useful for one-off troubleshooting; leaving the trace on accumulates log volume.
- **Observer mutation**: `register-observer <obj_path> <path> <timing> <source...>` and `unregister-observer <obj_path> <path> <timing>` bypass application-tier registration paths when an operator needs to install or remove a diagnostic observer at runtime without redeploying a Merry-script-bearing application. The source argument is captured verbatim through end-of-line.
- **Target resolution**: the `<obj_path>` argument resolves through `find_object` under the System auto layer, which deliberately does not resolve clone masters (`*/obj/*` paths) or generated leaf objects -- a clone master is a template, not an addressable runtime object. Reachable targets are therefore singleton programs (`sys/` daemons and the like); clones are not addressable by LPC path, and these verbs do not resolve Index logical names. Since the example applications host their observers on clones (rooms, things), the operator verbs audit and mutate observers on singleton hosts only; clone-hosted observer state is exercised and asserted by the applications' own test phases. `register-observer` / `unregister-observer` additionally require the target to carry the property API (`/lib/util/properties`); the daemon refuses otherwise, since the observer store is the target's property table.
- **Approved-registrar set**: `query-approved-registrars` lists the domains permitted to register observers across object boundaries; `approve-registrar Foo` / `unapprove-registrar Foo` mutates the set. The set seeds with `System` and `admin_console` at boot; adding `Merry` would also let the dispatcher's own /usr/-tier code register on arbitrary hosts (consequence: weaker layering boundary; appropriate for diagnostic scenarios, not for steady-state policy).

A worked example: an application's property write fails with a `cascade-aborted` error after a deploy of new observer scripts. The operator session:

```text
# cascade-depth
cascade-depth: 32
# batch-status 14
batch-status 14: cascade-aborted (merry: cascade depth 32 exceeded at /usr/App/sys/orders:total)
# observers /usr/App/sys/orders total
/usr/App/sys/orders total:
  pre:
    (none)
  main:
    /usr/Merry/merry/a1b2c3d4e5f6
    /usr/Merry/merry/9876543210ab
  post:
    /usr/Merry/merry/cdef01234567
# dispatch-trace on
dispatch-trace on
```

(The console prompt is `# `, and the console does not echo commands; a response can therefore read identically to the command typed, as `dispatch-trace on` does above.)

The combination identifies which timing slots have observers, gives runtime visibility into the next cascade, and bounds further investigation to the three compiled scripts named — all without restarting the platform or modifying application code.

## Bootstrap and authentication

On first cold boot with no persisted admin password:

1. Telnet to the kernel's `telnet_port`.
2. The kernel prompts to set the initial admin password.
3. The password hash persists into the platform's snapshot via the persistence primitive (§3); no separate "save credentials" step.
4. Subsequent connections prompt for the credential.

To reset the admin password from outside the console: the kernel's auth state lives in the access daemon (`/kernel/sys/access_daemon`). Cold-boot from a snapshot taken before the password was set, OR boot with the snapshot removed (cold boot from scratch). Both are operator-level actions outside the console.

## Appendix: alphabetical verb reference

| Verb | Category | Brief |
|---|---|---|
| `access [<user>\|<directory>\|global]` | Permissions | Inspect access bits for a user, a directory, or globally |
| `approve-registrar <domain>` | Dispatcher | Add a caller domain to MERRY's approved-registrars set (extension) |
| `batch-status <batch_id>` | Dispatcher | Query a batch's status + reason (extension) |
| `cascade-depth [N]` | Dispatcher | Read or set the max cascade depth bound (extension) |
| `cd <dir>` | Filesystem | Change session directory |
| `clear` | REPL | Empty the code-history ring buffer |
| `clone <obj>\|$N` | Code lifecycle | Clone a master; stores clone in history |
| `code <expr>` | REPL | Evaluate an LPC expression; stores result in history (`$N`) |
| `compile <file.c> [...]` | Code lifecycle | Compile one or more LPC source files |
| `cp <file> [...] <target>` | Filesystem | Copy files (access-checked at each side) |
| `destruct <obj>\|$N` | Code lifecycle | Destruct an object |
| `dispatch-trace on\|off\|status` | Dispatcher | Toggle or report verbose dispatch-entry tracing (extension) |
| `ed <file>` | Editor | Open the platform's line editor on a file |
| `grant <user> <dir> [read\|write\|full]`, `grant <user> access`, `grant global <dir>` | Permissions | Grant directory access (default `write`), create a user's file access, or add global read |
| `history [N]` | REPL | Show last N values from the code-history ring |
| `ls [-l] [<glob>]` | Filesystem | List directory; `-l` shows size + timestamp |
| `mkdir <directory> [...]` | Filesystem | Create directories |
| `mv <file> [...] <target>` | Filesystem | Rename / move files |
| `new <obj>\|$N` | Code lifecycle | Instantiate an LWO from a `data/` master |
| `observers <obj_path> <path> [timing]` | Dispatcher | List registered observers per timing slot (extension) |
| `people` | Connections | List active connections |
| `pwd` | Filesystem | Print session directory |
| `query-approved-registrars` | Dispatcher | List MERRY's approved-registrars set (extension) |
| `quota [<user> [<rsrc> [<limit>]]]` | Resources | Read or set per-owner resource limits |
| `reboot` | Lifecycle | Incremental snapshot + cold shutdown (recovers from snapshot on next boot) |
| `register-observer <obj_path> <path> <timing> <source...>` | Dispatcher | Install a runtime observer (extension) |
| `rm <file> [...]` | Filesystem | Remove files (refuses directories) |
| `rmdir <directory> [...]` | Filesystem | Remove empty directories |
| `rsrc [<resource> [<limit>]]` | Resources | Read or set platform-wide resource caps |
| `shutdown` | Lifecycle | Cold shutdown without snapshot |
| `snapshot` | Persistence | Write a full statedump to `dump_file` |
| `status [<obj>]` | State inspection | System-wide health vector, or per-object status |
| `swapout` | Persistence | Swap all in-memory objects to the swap file |
| `unapprove-registrar <domain>` | Dispatcher | Remove a caller domain from MERRY's approved-registrars set (extension) |
| `ungrant <user> <dir>`, `ungrant <user> access`, `ungrant global <dir>` | Permissions | Remove a directory grant, a user's file access, or a global-read entry |
| `unregister-observer <obj_path> <path> <timing>` | Dispatcher | Clear all observers at (obj, path, timing) (extension) |

## Where to next

- `docs/operations.md` — the deployment surface: `.dgd` configuration fields, boot modes, statedump cadence, logging, resource caps, host-driver extension loading.
- `docs/architecture.md` — the platform's tier model, daemons, and inheritance chain that the console verbs operate against.
- `docs/runtime-primitives.md` — the runtime primitives the console exposes (atomicity §1, capability separation §2, persistence §3, hot reload §4, state introspection §8).
- `src/kernel/lib/admin_console.c` — the authoritative LPC source for every verb's exact dispatch.
