# Admin console

The operator's console: a verb-based REPL that binds to the kernel's telnet port and exposes the platform's introspection, code-lifecycle, persistence, permissions, and resource surfaces. This document is the operator's reference; `docs/operations.md` is the deployment surface (configuration, boot modes, statedump cadence, extension loading).

**Audience**: an operator with admin-console access to a running platform; needs to introspect state, hot-fix code, manage permissions, snapshot, or shut down.

The console is implemented in two files:

- `src/kernel/lib/admin_console.c`: the library (~2,300 lines of LPC). Defines every verb, the parser, the history table, the dispatch.
- `src/kernel/obj/admin_console.c`: the clonable. Each operator connection clones this object. The clone holds per-session state (current directory, code-history values, the inherited library's user-tier configuration). The clonable is also the console's composition point with the wider platform: its switch-default routes registry-extension verbs, and it masks the object-taking verbs (`clone`, `destruct`, `new`, `status`) to resolve Index logical names before delegating to the library. The library itself stays composition-free.

The kernel's telnet port (named `telnet_port` in the `.dgd` configuration) is bound through two paths, detailed under Connecting below: the `admin` login clones the kernel console directly, and a registered user name routes through the System-tier telnet manager at `/usr/System/sys/userd.c` to the console-inheriting subclass at `/usr/System/obj/user.c`.

## Why a console at all

The platform is an in-memory runtime. State lives in objects. Code lives in compiled programs. Persistence is a runtime property. None of that is reachable through the host operating system's tools: `ls` on the host filesystem shows the LPC source, not the live object graph. `kill -SIGUSR1` does not snapshot the runtime. Operations on the running platform go through a runtime-aware interface.

admin_console is that interface. Its eight categories of verbs map to eight categories of operational work:

| Category | What it does | Underlying mechanism |
|---|---|---|
| **State inspection** | Query the live image: objects, owners, resources, connections, system health | Host introspection kfuns (`find_object`, `status`, `query_owners`, `query_users`, `get_dir`) |
| **Code lifecycle** | Compile, clone, destruct, instantiate LWOs; recompile in place | `compile_object`, `clone_object`, `destruct_object`, `new_object` |
| **REPL** | Evaluate LPC expressions interactively; replay history | `compile_object` against a temporary path |
| **Filesystem** | Navigate the platform's source tree; read and write source files | `get_dir`, `read_file`, `write_file`, `rename_file`, etc. (all gated by per-tier access checks) |
| **Editor** | Edit LPC source via DGD's built-in line editor | DGD's `ed` kfun (the LPC `editor` kfun) |
| **Permissions** | Inspect and modify per-user access bits | Kernel access daemon API (`set_access`, `query_user_access`, `query_file_access`) |
| **Resources** | Inspect and modify per-owner resource limits | Resource daemon API (`rsrc_set_limit`, `rsrc_get`) |
| **Persistence and lifecycle** | Swap out memory; snapshot; shutdown; reboot | Host kfuns `swapout`, `dump_state`, `shutdown` |

Each verb is a thin LPC wrapper over a host kfun or a daemon method. The `code` verb is the most general: it compiles its argument as an LPC expression and runs it, which means an operator with sufficient access can reach anything the platform exposes. The verb set is convenience over `code`, not capability beyond it.

## Connecting

The console listens on the address-and-port set by the `.dgd` configuration's `telnet_port` (default `8023`):

```sh
telnet localhost 8023
```

First-connection behavior depends on whether the kernel has admin credentials persisted:

- **Cold boot, no prior admin**: the kernel prompts to set an admin password. The hash is persisted across statedumps (runtime primitive §3), so subsequent boots find it.
- **Subsequent connections**: name and password prompt; the hash is checked against the persisted credential.

An operator authenticated as `admin` has tier-spanning reach (kernel and System tiers, plus cross-domain visibility into user-tier code). Other authenticated operators have access bounded by their owner's directory tree and the access-daemon grants on top. A cold boot registers no operator beyond `admin`: the System telnet manager accepts only names on the kernel access list, so a new operator is provisioned live from the admin console. `grant <user> access` registers the name (creating `/usr/<user>`), directory grants scope what the new operator may touch, and the operator's first login walks the set-a-password flow. `scripts/verbsets/operator-provision.verbset` automates exactly this sequence.

The two login shapes also reach different console objects. The `admin` login (on the primary telnet port) clones the kernel console (`src/kernel/obj/admin_console.c`), whose switch-default routes the registry's extension verbs (`observers`, `log`, `dispatch-trace`, and the rest). A registered user name routes through the System userd to the System login console (`/usr/System/obj/user.c`), which inherits the same console library and additionally carries the System lifecycle verbs (`upgrade`, `issues`, `hotboot`, `halt`) that the kernel console's verb set does not list. It does not route registry-extension verbs. A verb answered with `No command` on one console shape may belong to the other.

## Console security posture

The admin_console is the platform's most dangerous interface and the platform's most useful interface. Two facts shape its security model:

1. **The `code` verb evaluates arbitrary LPC.** An operator who can invoke `code` can call any kfun the inheriting object has access to. For `admin`, that means every kfun in the runtime. Treat console access as equivalent to host shell access on the platform's process.
2. **Permissions inside the console enforce the platform's tier model.** Non-admin operators see only what their owner permits. `code` invocations still hit per-call access checks at every kfun boundary; a non-admin operator's `code` invocation cannot bypass the tier model.

In production deployments, expose the telnet port only on a loopback interface or a dedicated maintenance VLAN. The console wire protocol is unencrypted telnet; remote operators should reach the port through an SSH or TLS tunnel. See `docs/operations.md` (Network boundary and transport security) for the full deployment posture, covering the application HTTP transport alongside the console.

## Operational tasks

The verb categories below are organized around the operational situation an operator is in, not alphabetically. Each section names the verbs in play, the platform mechanism behind them, and the scenario where the choice of one verb over another matters.

### Inspecting runtime state

**Verbs**: `status`, `code`, `people`, `pwd`, `cd`, `ls`, `history`

**Why**: the platform's value is that the running image is queryable; you do not need a debugger attached to know what the runtime is doing. `status` walks the host's system-wide health vector (memory, swap, objects, users, uptime, call_outs); `code` evaluates an arbitrary LPC expression in the operator's domain and returns the result with history-pointer assignment (`$N`); `people` enumerates live connections.

**How**:

- `status` without arguments calls the host `status()` kfun with no argument and pretty-prints the result (memory used, memory free, swap configuration, object count, free objects, call_out count, free call_outs, uptime, last reboot, daemon registration state).
- `status <obj>` calls `status(obj)` for per-object status: object name, compile time, program size, creator, variable count, owner, pending call_out count, master ID, sector count. `<obj>` is an LPC path, a `$N`/`$ident` history reference, or an Index logical name (`status Schema:Core:Entry`); colon-shaped arguments resolve path-first, then through the logical-name registry, and print `No such object or Index name.` when neither route resolves.
- `code <expr>` compiles `<expr>` as the body of a synthesized `mixed exec(object user, mixed... argv) { ... }` function in a transient object at `/usr/<owner>/_code`, calls it, prints the result, stores the result in the history table under `$N`, and destructs the transient object. The standard includes (`<float.h>`, `<limits.h>`, `<status.h>`, `<trace.h>`, `<type.h>`) plus the operator's optional `~/include/code.h` are prepended; 26 single-letter `mixed` variables `a` through `z` are pre-declared for ad-hoc use.
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

**Why**: hot reload is a runtime primitive: `compile_object(path)` against a path with an existing master replaces the master's program. Subsequent calls dispatch to the new version (`docs/runtime-primitives.md` §4). The console exposes this mechanism plus the clone/destruct/instantiate cycle that surrounds it.

**How**:

- `compile <file.c> [<file.c> ...]` calls `compile_object()` on each path (stripped of the `.c` suffix). The verb expands shell-style glob patterns in the current directory; each successful compile stores the master object in the history table.
- `clone <obj> | $N` calls `clone_object()` on the named master (or the historical result `$N`). Stores the clone in history.
- `destruct <obj> | $N` calls `destruct_object()` on the named object. Verifies the object exists; reports the error if not.
- `new <obj> | $N` calls `new_object()` to instantiate a Light-Weight Object (LWO) from a `data/` master. Distinct from `clone`: LWOs are value-shaped (no separate identity), `clone` creates first-class clones.

All four object-taking verbs (`clone`, `destruct`, `new`, `status`) also accept an Index logical name in place of `<obj>`: a colon-shaped argument resolves path-first, then through the logical-name registry, and the canonical object name is handed to the library verb, so the per-verb guards behave exactly as with a path (`clone` on a name that resolves to a clone still refuses with `Not a master object.`). This is the operator-facing address for clones, whose `path#index` form is not stable across boots. An unresolved colon-shaped argument reports `No such object or Index name.`

**What for**:

- **Emergency bug fix in a running service**:
  1. Edit the source file (via `ed` or via the host filesystem).
  2. `compile /usr/MyApp/obj/route_handler.c`: replaces the master. In-flight calls finish on the old code. The next call uses the new code.
  3. Verify with `code "/usr/MyApp/obj/route_handler"->probe()` (or an equivalent test invocation).
  
  No restart; no disconnect.
- **Library upgrade**: recompiling a library (`/usr/MyApp/lib/util.c`) replaces the library master, but existing children of the library do not automatically pick up the new parent. The `upgrade [-a|-p] <file> [<file> ...]` verb is carried by the System login console (`/usr/System/obj/user.c`), the console shape a registered user name logs into (see Connecting). The `admin` login's kernel console does not list it. The verb drives the platform's recompile cascade: the upgrade daemon (`/usr/System/sys/upgraded.c`) walks the object manager's inheritance graph for every direct and transitive dependent, recompiles them (`-a` for all-or-nothing atomic recompile), and with `-p` queues `call_touch` patching and then drives the patch sweep itself through zero-delay callouts, migrating clone state eagerly (`scripts/verbsets/operator-upgrade.verbset` drives the full verb cycle as a registered operator: refusal, staging, upgrade, cascade assertions). Manual alternatives remain:
  - Destruct each child (`destruct /usr/MyApp/obj/foo`) and recompile (`compile /usr/MyApp/obj/foo.c`). Loses clone state.
  - Use `call_touch` (via `code`) to mark every dependent for lazy upgrade through `_F_touch()`. Preserves state.
- **Recover from a wedged daemon**: `destruct /usr/MyApp/sys/router` then `clone /usr/MyApp/sys/router` would re-instantiate from the master, but `sys/` daemons are singletons that compile at boot, not on demand. The recovery is `destruct` followed by `compile` of the daemon source.
- **A/B testing**: keep the canonical handler at `/usr/MyApp/obj/handler.c`; copy it to `handler_b.c`; compile the variant; route a percentage of traffic to the variant; compile the winner back as `handler.c` when results are in.

### Snapshot, restore, and shutdown

**Verbs**: `swapout`, `snapshot`, `shutdown`, `reboot`

**Why**: orthogonal persistence means the snapshot IS the backup. The platform's `dump_state` kfun captures the entire in-memory image to disk; the next boot restores from that image with all object state, all clones, all pending `call_out`s, all access bits, all resource counters intact. Connections do not survive a snapshot restore (open file descriptors are not in the snapshot); on hot boot they do (the file descriptors are inherited across `execv`).

**How**:

- `swapout` calls `swapout()`. Swaps every in-memory object to disk (the swap file), reducing memory pressure. Next access faults the object back in. Useful before a snapshot: a swapped-out image fits more cleanly into the snapshot.
- `snapshot` calls `dump_state(0)` (full image dump). Writes to `dump_file` per the `.dgd` configuration. The previous snapshot moves to `<dump_file>.old`. Cost: I/O for the full image size; runtime briefly blocks.
- `shutdown` calls `shutdown()` (cold shutdown without snapshot). The platform exits; the next boot is a cold boot OR a snapshot-restore from the most recent snapshot (depending on whether the snapshot file is present and valid).
- `reboot` calls `dump_state(1)` (incremental snapshot) followed by `shutdown()`. Effectively: snapshot-and-stop; requires full access. Next boot restores from the new snapshot.

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

- **Onboard a developer**: `grant developer access` creates their owner tree. `grant developer /usr/SharedLib read`: they can now read the shared library but not modify it.
- **Investigate a permission denied**: `access <user>` shows the user's current bits; `access <directory>` shows who has access to the target. Compare to find the missing grant.
- **Lock down a sensitive directory**: `ungrant <user> /usr/Secrets` removes their access; `access /usr/Secrets` confirms no one else has unexpected reach.

### Managing resources

**Verbs**: `quota`, `rsrc`

**Why**: every owner has a resource quota: object count, call_out count, ticks per call, stack depth. The resource daemon (`/kernel/sys/resource_daemon`) tracks usage and enforces the quota at every relevant kfun call. An owner that exhausts their tick budget gets a runtime error (which rolls back the offending atomic context, per runtime primitive §1).

**How**:

- `quota` with no arguments prints the operator's own quota and current consumption.
- `quota <user>` prints another user's quota.
- `quota <user> <rsrc>` shows usage of a specific resource (ticks, stack, callouts, objects, fileblocks).
- `quota <user> <rsrc> <limit>` sets the limit. `-1` means unlimited (for the resources that permit unlimited).
- `rsrc` with no arguments shows platform-wide totals across all owners.
- `rsrc <resource>` shows per-owner breakdown for a resource.
- `rsrc <resource> <limit>` sets the platform-wide cap (the value also writable via the `.dgd` configuration; this is the runtime override).

**What for**:

- **Diagnose a runaway loop**: `rsrc ticks` shows which owner is consuming ticks. The offending domain's code is exhausting its budget, typically an infinite loop or an unbounded recursion.
- **Adjust a per-owner budget**: a legitimate workload that hits the ticks ceiling needs `quota <owner> ticks <higher-limit>`, but consider whether the workload should be split into multiple smaller atomic contexts (via `call_out`) before raising the limit.
- **Capacity planning**: `rsrc objects` and `rsrc callouts` show how close the platform is to its hard caps (`objects`, `call_outs` in `.dgd`). Approaching the cap means a config increase is due.

### Editing files

**Verbs**: `ed`

**Why**: operators sometimes need to edit platform source files from within the platform: when the host filesystem is not directly accessible (the kernel's `directory` is chrooted), or when the platform is the host's only interactive entry point. DGD ships a line editor reachable through the host's `editor` kfun. The `ed` verb is the operator wrapper.

**How**: `ed <file>` invokes the host `editor` kfun against the named file. The verb is a thin wrapper. The editor is a classic line-oriented editor (the verb name `ed` reflects this); navigation by line number, search by regular expression, edits by line range. See the host's editor reference for the command set.

**What for**:

- **Quick fix without host access**: a one-line patch to a wedged daemon when the operator does not have shell access to the host machine.
- **Cross-platform edit consistency**: the platform editor handles line endings consistently regardless of the host platform, useful when host editors might write `\r\n` files that DGD does not accept.

For substantial edits, prefer a host-side editor. The `ed` verb is for cases where it's the only option.

### Filesystem navigation and manipulation

**Verbs**: `cd`, `pwd`, `ls`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`

**Why**: the platform exposes a virtual filesystem rooted at the `.dgd` configuration's `directory` setting. All filesystem-style kfuns (`read_file`, `write_file`, `get_dir`, etc.) are gated by the per-tier access checks. Operating on these files through the console (rather than the host OS) enforces the tier model: `rm` on a file the operator does not own fails the same access check that an LPC `remove_file()` call would fail. The verbs name themselves after Unix equivalents to minimize surprise. The underlying behavior is access-checked, not raw filesystem.

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
- **Reorganize a domain**: `mkdir /usr/MyApp/lib/util && mv /usr/MyApp/lib/strings.c /usr/MyApp/lib/util/`. The driver's `inherit_program` constraint requires `/lib/` in the inherited path: moves that preserve `/lib/` survive. Moves that drop it break inheritance.

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

**Why**: the Merry dispatcher (`docs/dispatcher.md`) routes every property-change observation through a substrate that maintains observer registrations, cascade-depth bounds, cycle detection, and batch-status accounting. Operators investigating dispatcher behavior at runtime (debugging a deep cascade, auditing observer registrations after a deploy, toggling verbose trace for a session) need first-class verbs rather than long-form `code MERRY->_query_batch_status(7)` invocations.

**How**: these verbs are not built into the kernel admin console (its built-in set is the kernel-tier categories enumerated above). They live in `src/usr/Merry/lib/admin_console_ext.c` and reach the console via the selective-extension model:

- A KERNEL-tier registry at `/kernel/sys/admin_console_registry` holds a `verb -> (extension_path, method_name)` dispatch table. Merry's nine verbs are hardcoded into the registry's `create()`.
- The console's unknown-verb switch default consults the registry; if it finds an entry and the extension master is loaded, it dispatches there.
- Mutation verbs whose underlying daemon LFUNs are KERNEL-gated (`approve-registrar`, `unapprove-registrar`, `cascade-depth N`, `dispatch-trace on|off`) or capability-gated by caller-domain (`register-observer`, `unregister-observer`) route through the registry's `verb_*` elevation helpers. The registry is KERNEL-tier, so the daemon's gates pass. Which extensions may use the elevation surface is itself a capability: `admin_console.caller` in the capability library (`docs/capability.md`), seeded with the Merry extension library at the registry's `create()` and checked through the registry's inherited `/kernel/lib/capability` helpers.

The kernel admin console itself remains MERRY-unaware. Future operator surfaces for Vault, Schema, or HTTP extend the registry's hardcoded verb table the same way and seed their own capabilities in the store. The cross-subsystem capability model the registry once only anticipated has landed (`docs/capability.md`): the caller authorization and the approved-registrar set are now capabilities, while the verb-to-extension table stays hardcoded at `create()` by design. Dynamic *verb* registration is a separate future step, and the dispatch shape is unchanged either way.

**What for**:

- **Observer audit**: `observers <obj_path> [<path> [timing]] [-effective]` exposes all three of the daemon's read-only query views (`docs/observers.md` "Query surface"). With no `<path>` it enumerates the target's observed `(path, timing)` slots: the discovery step when the paths are unknown. With a `<path>` it lists the local slot per timing, indexed. The indices are what `unregister-observer`'s optional index argument removes by. With `-effective` (a `<path>` is required) it renders the ancestry-walk view: what a dispatch would fire, each entry labeled with the owning ancestor. Useful before a Merry-script redeploy to confirm what the substrate believes about the current registration state. See the target-resolution note below for which objects the `<obj_path>` argument can reach.
- **Cascade-depth tuning**: `cascade-depth` reads the current bound; `cascade-depth 64` raises it after a legitimately-deep cascade scenario has been verified safe. Statedump-persistent.
- **Batch forensics**: `batch-status 7` shows whether batch 7 completed, was atomically aborted, cycle-aborted, or vetoed by a pre-observer; the reason field carries the propagated error for aborted batches. Useful when a dispatcher event surfaces in the application log and the batch-id is the only pointer back to the substrate's view.
- **Tracing**: `dispatch-trace on` enables per-`dispatch_set` entry logging to the general `logd` stream at DEBUG level (the always-on cycle and cascade audit events stay in `/usr/Merry/log/dispatch.log`). `dispatch-trace off` returns to the silent default. Trace lines are suppressed under `logd`'s default INFO threshold, so pair with `log-level debug` (the verb prints a hint when the current threshold would drop them) and read the result with `log`. Useful for one-off troubleshooting. Leaving the trace on accumulates log volume.
- **Observer mutation**: `register-observer <obj_path> <path> <timing> <source...>` and `unregister-observer <obj_path> <path> <timing> [index]` bypass application-tier registration paths when an operator needs to install or remove a diagnostic observer at runtime without redeploying a Merry-script-bearing application. The source argument is captured verbatim through end-of-line. Without the trailing index, `unregister-observer` clears all observers at the triple; with it, the single slot entry at that position is removed (the daemon's `remove_observer`; indices as shown by `observers <obj_path> <path>`).
- **Target resolution**: the `<obj_path>` argument resolves path-first, then through the Index logical-name registry, the same order the coercion codec uses for object references. The path route goes through `find_object` under the System auto layer, which deliberately does not resolve clone masters (`*/obj/*` paths) or generated leaf objects: a clone master is a template, not an addressable runtime object, and a clone's `path#index` form is not stable across boots. The name route is what makes clones addressable: an object registered via `/lib/util/named::set_object_name` is reachable by its logical name (`observers Chat:Lobby`, `register-observer MerryApp:demo:parent ...`), so the full registration cycle runs against clone hosts from the console. A target that resolves by neither route reports `<verb>: target not found (no loaded object or Index name): <arg>`. `register-observer` / `unregister-observer` additionally require the target to carry the property API (`/lib/util/properties`). The daemon refuses otherwise, since the observer store is the target's property table.
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
    [0] /usr/Merry/data/merry#-1 {Set($this, "audit:total", $new); return TRUE;}
    [1] /usr/Merry/data/merry#-1 {Set($other, "rollup:total", $new); return TRUE;}
  post:
    [0] /usr/Merry/data/merry#-1 {Log("total changed"); return TRUE;}
# dispatch-trace on
dispatch-trace on
note: trace lines emit at DEBUG and the current log-level suppresses them; `log-level debug` to see them
# log-level debug
log-level set to DEBUG
```

(The console prompt is `# `, and the console does not echo commands; a response can therefore read identically to the command typed, as `dispatch-trace on` does above. The note follows because the default INFO threshold would drop the DEBUG-level trace lines; `log-level debug` opens the sink, and the `log` verb tails the result.)

The combination identifies which timing slots have observers, gives runtime visibility into the next cascade, and bounds further investigation to the three observer sources shown, all without restarting the platform or modifying application code. Every stored observer shares the light-weight wrapper name (`/usr/Merry/data/merry#-1`). The bracketed index and the source snippet are what distinguish entries, and the index is what `unregister-observer`'s optional index argument removes by.

## Debugging a stuck platform

The investigative moves from the task sections above, consolidated into one walkthrough. Each entry is a symptom, the verb sequence to run, and how to read what comes back. Take a `snapshot` before any invasive recovery step: the snapshot is the rollback point if the recovery makes things worse.

**The platform feels unresponsive: requests hang or take seconds.** Run `status` and read the health vector against capacity: call_out count near the `call_outs` cap means a backlog of deferred work. Object count near the `objects` cap means allocation failures are imminent. Heavy swap activity means the resident set exceeds memory and every access is paging. If the vector is healthy (counts well under caps, no swap churn), the problem is logical, not capacity: move to the per-owner and per-object probes below.

**One workload is eating the platform.** Run `rsrc ticks` and read the per-owner tick consumption with the max-usage column. The owner consuming far beyond its peers hosts the offending code, typically an unbounded loop or runaway recursion. `quota <owner>` shows the owner's limits and current use. Two exits: fix the looping code (hot-fix via `compile`, no restart), or (when the workload is legitimate) raise the budget with `quota <owner> ticks <limit>`, after considering whether the work should instead be split across timeslices via `call_out`. A tick-exhausted call errors and rolls back its atomic context, so the platform itself survives the runaway. The budget is the protection.

**A daemon stopped responding.** Probe its state directly: `code "/usr/App/sys/router"->query_state()` (any cheap LFUN the daemon exposes). An error reveals whether the master is wedged, destructed, or erroring internally: the trace names the failing function and line. Recovery for a singleton daemon is `destruct /usr/App/sys/router` followed by `compile /usr/App/sys/router.c` (daemons compile at boot, not on demand, so the recompile re-instantiates it). The daemon's in-memory state does not survive this: check what the domain's initd re-establishes before destructing.

**A property write fails with a cascade or batch error.** This is dispatcher territory: `cascade-depth` for the current bound, `batch-status <id>` for the failing batch's status and propagated reason, `observers <obj_path> <path>` for what is registered where, `dispatch-trace on` for per-dispatch visibility into the next occurrence. The worked example in the Dispatcher operator surface section above walks the full sequence.

**Something an operator did, or a stray connection, is interfering.** `people` lists live connections with addresses and idle time. Compare against the expected operator population. Combine with `status` for the who-is-on plus what-is-going-on snapshot. The console's own sessions appear here too: `people` marks who is in the editor, and editor sessions come from a small fixed pool (the `editors` field in the `.dgd` configuration).

**The platform is degrading slowly across days.** Capacity creep: `rsrc objects` and `rsrc callouts` against the `.dgd` caps show whether the platform is drifting toward a hard ceiling; `status` swap numbers show whether the image has outgrown memory. A `swapout` relieves memory pressure immediately (objects page back in on access); a config raise needs a reboot and belongs to `docs/operations.md`.

**None of the above explains it.** `snapshot`, then experiment freely: `code` reaches everything the platform exposes, and the snapshot plus `<dump_file>.old` are the way back. For failures at boot rather than at runtime (compile errors in the initd cascade, restore failures, missing extensions), see the Common failure modes table in `docs/operations.md`.

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
| `observers <obj_path> [<path> [timing]] [-effective]` | Dispatcher | List observers: local slot (indexed), observed-path enumeration, or effective walk view (extension) |
| `people` | Connections | List active connections |
| `pwd` | Filesystem | Print session directory |
| `query-approved-registrars` | Dispatcher | List MERRY's approved-registrars set (extension) |
| `quota [<user> [<rsrc> [<limit>]]]` | Resources | Read or set per-owner resource limits |
| `reboot` | Lifecycle | Incremental snapshot + cold shutdown (recovers from snapshot on next boot). Requires full access. |
| `register-observer <obj_path> <path> <timing> <source...>` | Dispatcher | Install a runtime observer (extension) |
| `rm <file> [...]` | Filesystem | Remove files (refuses directories) |
| `rmdir <directory> [...]` | Filesystem | Remove empty directories |
| `rsrc [<resource> [<limit>]]` | Resources | Read or set platform-wide resource caps |
| `shutdown` | Lifecycle | Cold shutdown without snapshot. No access gate: any authenticated operator may run it (`reboot`, by contrast, requires full access). |
| `snapshot` | Persistence | Write a full statedump to `dump_file` |
| `status [<obj>]` | State inspection | System-wide health vector, or per-object status |
| `swapout` | Persistence | Swap all in-memory objects to the swap file |
| `unapprove-registrar <domain>` | Dispatcher | Remove a caller domain from MERRY's approved-registrars set (extension) |
| `ungrant <user> <dir>`, `ungrant <user> access`, `ungrant global <dir>` | Permissions | Remove a directory grant, a user's file access, or a global-read entry |
| `unregister-observer <obj_path> <path> <timing> [index]` | Dispatcher | Clear all observers at (obj, path, timing), or remove one by index (extension) |

## System login console verbs

The appendix above is the kernel console's verb set, reached by the `admin` login. The System login console, reached by a registered user name (Connecting above), inherits those verbs and adds the lifecycle verbs below, which the kernel console does not carry (`src/usr/System/obj/user.c`). It also renames one: the kernel console's `shutdown` is reached as `halt` here.

| Verb | Brief |
|---|---|
| `upgrade [-a\|-p] <file> [...]` | Recompile a source file and every dependent through the upgrade cascade. `-a` recompiles the whole dependency tree as one all-or-nothing atomic operation. `-p` additionally queues `call_touch` patching so live clones migrate state (see Hot-fixing code in production above and `docs/code-lifecycle.md` Library upgrade). |
| `issues <file> [...]` | List the outstanding compiled program versions (objectd calls them issues) for each file. More than one issue means older versions are still bound to dependents that have not upgraded, so the verb reads back whether an upgrade cascade has fully propagated. |
| `hotboot` | Write an incremental snapshot and re-exec the host binary in place (`dump_state(1)` then `shutdown(1)`), inheriting open connections. This is the connection-preserving host-binary swap (`docs/operations.md` Running under a supervisor). It requires full access and fails with `Permission denied` otherwise. |
| `halt` | Cold shutdown without a snapshot, the System console's name for the kernel console's `shutdown`. It leaves a restore point only if a `snapshot` or `reboot` ran first. Unlike `reboot` and `hotboot` it carries no full-access gate: any authenticated operator can halt the platform -- weigh that when provisioning operators. |

`upgrade` recompiles only the sources the operator's owner may write, so an operator can upgrade only what it can edit.

## Where to next

- [`docs/operations.md`](operations.md): the deployment surface, covering `.dgd` configuration fields, boot modes, statedump cadence, logging, resource caps, host-driver extension loading.
- [`docs/architecture.md`](architecture.md): the platform's tier model, daemons, and inheritance chain that the console verbs operate against.
- [`docs/runtime-primitives.md`](runtime-primitives.md): the runtime primitives the console exposes (atomicity §1, capability separation §2, persistence §3, hot reload §4, state introspection §8).
- [`src/kernel/lib/admin_console.c`](../src/kernel/lib/admin_console.c): the authoritative LPC source for every verb's exact dispatch.
