# Operations

This document covers running an eOS-kernellib instance: booting and re-booting it, snapshotting, backing up, and restoring its persistent state, its availability and data-loss model, monitoring its output, diagnosing failures, and loading optional host-driver extensions. `.dgd` configuration fields and the platform's capacity ceilings are covered in `docs/configuration.md`. The architecture document (`docs/architecture.md`) covers the platform's structural model. This document covers the operator's surface for keeping it running.

**Audience**: someone running the platform, responsible for watching the running process, taking snapshots, restoring after a crash, and deciding whether to load extensions. Choosing config values and sizing a workload is covered in `docs/configuration.md`. Application authoring is covered in `docs/application-authoring.md` and `docs/http-applications.md`.

## The operator's task index

The jobs this doc (and its task-shaped companions) cover, by name:

| I need to... | Where |
|---|---|
| **Diagnose a dead or misbehaving process, right now** | **Common failure modes; Monitoring signals** |
| Stand up a fresh production deployment, in order | Day 0: standing up a production deployment |
| Choose config values, raise a ceiling | `docs/configuration.md` The .dgd configuration file; Limits and capacity |
| Size a workload's storage shape | `docs/configuration.md` Limits and capacity, Sizing a workload |
| Boot cold, restore a snapshot, hot-boot | Booting |
| Take a backup now; restore one (same host or off-host) | Backing up and restoring state |
| Schedule unattended off-host backups | Backing up and restoring state, Scheduled off-host backup, end to end |
| Run under systemd, launchd, or another supervisor | Running under a supervisor |
| Monitor headlessly and write alert rules | Monitoring signals |
| Expose a machine-readable health endpoint | `docs/common-tasks.md` Expose a health check for monitoring |
| Serve HTTPS on the labeled port | Network boundary and transport security; `docs/common-tasks.md` Serve HTTPS on the labeled port |
| Load a host-driver extension | Loading host-driver extensions |
| Plan for the image approaching the state ceiling | When the image approaches the state ceiling |
| Ship a release; roll one back | `docs/changing-a-running-system.md` Shipping a release, Rolling back a release |
| Provision, rotate, or offboard an operator credential | `docs/security-posture.md` Credential lifecycle |

## Day 0: standing up a production deployment

The constituent pieces each have their own section; what a first deployment needs is their order, because three orderings are load-bearing: **the admin credential is claimed by the first console connection**, so the telnet port stays loopback-only until the claim lands (an unclaimed admin behind a reachable port is a race anyone on that network can win); **transport security activates before any real client is pointed at the platform**; and **extensions are cold-boot facts** (a module added at a restore boot reaches the kfun table but not the platform daemons, whose cold-boot stand-down is image state -- Loading host-driver extensions below), so the module set is decided before the boot that goes live. The sequence:

1. **Build the pieces on the host.** The driver from the pinned commit (`docs/getting-started.md` Install DGD) and every extension the deployment needs beside it -- the crypto module if identity, sessions, or native TLS are in play. Create an unprivileged service user; the checkout and `state/` belong to it alone (State file locations and permissions below).
2. **Write the production configuration.** Start from the production-shape starting point (`docs/configuration.md` Limits and capacity): size the caps, set `dump_interval` against the availability model's recovery-point and recurring-pause trade (Availability and data-loss model below), keep `telnet_port` bound to loopback, and name the `modules`.
3. **First boot, and claim admin immediately.** Boot, connect over loopback, and walk the first-claim password flow before anything else touches the host (`docs/security-posture.md` Credential lifecycle). The hash lands file-backed under `src/kernel/data/`, independent of the image.
4. **Provision the operator surface.** The monitoring credential (`grant monitor access`, first-login password -- Monitoring signals below), each human operator's registered login, and the application's secret file where one is needed (`docs/common-tasks.md` Provision an application secret out of source; the file lives inside the domain tree, so stage it whenever step 6's copy puts that tree on the host).
5. **Activate transport security.** Certificates at the configured paths, `tls-cert reload`, and the labeled `https` port answering -- before serving anything real (Network boundary and transport security below). A reverse proxy in front is the alternative where one host fronts several services.
6. **Deploy the application domains and cold-boot.** Deploy-by-copy requires a cold boot -- the initd iteration runs only there (`docs/http-applications.md` Reference application) -- and this same boot fixes the image's extension set for every restore that follows. The domains' own sentinel drivers or health routes verify the deploy. (The application team's side of this step -- the repository the copy comes from -- is `docs/application-repository.md`.)
7. **Verify the way the monitor will.** Drive the health route and read the counts against the alert thresholds (Monitoring signals below; `docs/common-tasks.md` Expose a health check for monitoring).
8. **Hand off to the supervisor and schedule the drills.** The supervisor owns restarts from here (Running under a supervisor below); the backup sequence runs on its schedule with the restore rehearsal that makes it a recovery plan (Backing up and restoring state below). Day 2 -- shipping and rolling back releases -- is `docs/changing-a-running-system.md`.

## Booting

The platform has three boot modes. `docs/architecture.md` covers the dispatch in detail. Briefly:

- **Cold boot**: started with no snapshot argument. The driver compiles `/kernel/sys/driver`, which compiles the System initd. The System initd's `create()` iterates and loads every `/usr/[A-Z]*/initd.c`, and the platform reaches the running state. This is the path for first-time bring-up and after intentional state wipe.
- **Snapshot restore**: started with the snapshot named on the command line (`dgd config_file dump_file [dump_file.old]`, Backing up and restoring state below). The driver reloads the snapshotted object graph and dataspaces, then calls the registered `restored(int hotboot)` driver hook. Initd cascades do not run. The platform resumes the state captured at the snapshot. The driver never restores a snapshot it was not given as an argument, however current the file sitting at `dump_file`.
- **Hot boot**: `shutdown(1)` followed by `execv` (when the `.dgd` file's `hotboot` tuple is set). Open file descriptors and connections are inherited by the replacement process. The snapshot is written and reloaded, but external connections survive the transition. Used for upgrading the host binary or `.dgd` config without dropping live work.

### Config changes across a restore

The capacity remediations in this document say "raise the cap and reboot from snapshot". This table is what a restore boot actually accepts, each row verified against a live restore (edit the field, boot `dgd <config> <dump_file> [<dump_file>.old]`, read `State restored.` and the live values `status` surfaces):

| Field changed | A restore boot's behavior |
|---|---|
| `objects` raise, `call_outs` raise | Accepted; the raised cap is live (the `status` denominators show it) |
| `users` | Lowering is accepted and live. Raising past 255 is impossible at any boot: the config parser refuses (`Config error ... int value out of range`) -- 255 is the driver's ceiling, so the users cap has no raise remediation |
| `array_size` | Already at the stock ceiling (32767); a raise refuses at config parse, restore and cold boot alike |
| `editors` raise | Accepted |
| `swap_size` raise | Accepted; the raised sector ceiling is live |
| `sector_size` change | Accepted; the per-boot swap file is rebuilt at the new size and the restored image's sector count re-derives against it |
| `swap_fragment`, `static_chunk` / `dynamic_chunk`, `dump_interval` | Accepted |
| `telnet_port` / `binary_port` change | Accepted; the restored image binds the new ports (the old ones are released) |
| `dump_file` path change | Accepted; the restore reads the snapshot named on the command line, and subsequent snapshots write to the new path |
| `typechecking` change | **Refused**: `Bad or incompatible snapshot header`, then `Config error: initialization failed`. The setting is baked into the snapshot header and compared on restore; changing it takes a cold boot |
| `modules` addition | Boots and restores with no diagnostic, with the two-level behavior stated at Loading host-driver extensions: the module's kfuns work in the restored image, while platform daemons that probed for the module at cold boot keep their recorded stand-down |

One method caveat for anyone scripting restore cycles: a platform stopped by a kill signal writes an incremental snapshot on the way down (the kernel driver dumps before shutdown), rotating the previous full snapshot to `<dump_file>.old` -- so a kill-stopped platform restores with the two-file form, and a single-file restore of the fresh primary fails with `Missing secondary snapshot` (Backing up and restoring state below).

### Replace the host binary, end to end

Rung 6 of `docs/changing-a-running-system.md` names the mechanism (the hotboot tuple); this is the operator sequence, executed live:

1. **Pre-flight the tuple.** The `hotboot` tuple is boot-time configuration -- its paths resolve against `directory`, and it cannot change without a cold reboot -- so the swap is a file replacement at the tuple's configured binary path. Compare that path against what is actually running: `ps -o args= -p <pid>`.
2. **Rehearse the candidate binary off-host first** (`docs/changing-a-running-system.md` Shipping a release): does the current snapshot restore under it?
3. **Write a full snapshot and set the recovery point aside.** Run `snapshot` on the console, then copy the pair somewhere the hotboot will not rotate: recovery from a failed hotboot cold-boots from a PRE-hotboot snapshot. The pair a hotboot writes on its way down serialized the open connections, and a cold start of it dies with `Fatal error: cannot restore user` -- it is a valid restore target only for the `execv` half that inherits those descriptors.
4. **Stage the rehearsed binary at the tuple's binary path.** Replace the file, never write the running binary's inode in place: `mv` the old aside (or `rm` it), then copy the new build to the tuple path.
5. **Run `hotboot`.** The verb lives on the System console -- a registered operator login with full access -- not on the kernel admin console (`docs/admin-console.md` System login console verbs). The server log shows `** System hotbooting...` then the banner and `** State restored.`.
6. **Verify by host-side process inspection, not the banner.** The banner prints the same driver version either way. The discriminators: the pid is unchanged (`execv` replaces the program in the same process), `ps -o args= -p <pid>` now shows the tuple's four paths verbatim as the argv, the file at the tuple path carries the new build's inode and mtime, and pre-hotboot console connections are still open with image state carried (`status` shows the original start time with Last reboot updated).
7. **Know the failure mode.** If the tuple's binary path is missing or not executable when `hotboot` runs, the server log says `Hotboot failed` and the process EXITS -- the snapshot and shutdown were already committed before the `execv` attempt -- dropping every connection with no console diagnostic. The supervisor restarts against step 3's set-aside pair (Running under a supervisor); this is the recovery the pre-flight in step 1 exists to make unnecessary.

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

**Which restore to run, from what survives on disk.** The incident-moment version of the sections below: find the row that matches what is actually on the host, run its command, and check the first boot line against it.

| What survives | Command | Expected first boot line | If it fails |
|---|---|---|---|
| `dump_file` and `<dump_file>.old`, both readable | `dgd config_file dump_file dump_file.old` | `State restored.` | `Bad or incompatible restore file header` means the file itself is corrupt or foreign to this driver build (the off-host restore drill's guard, below); `Bad or incompatible snapshot header` means the file is readable but its header disagrees with this boot (a different driver build, or a changed `typechecking` level -- Config changes across a restore above). Either way, a corrupt `dump_file` falls to the next row |
| Only `dump_file`, and it is a full snapshot (written by the `snapshot` verb or the console dump-and-exit path) | `dgd config_file dump_file` | `State restored.` | If `dump_file` is actually an incremental with no `.old` present, this fails with `Missing secondary snapshot` -- name the base explicitly (previous row) or fall back to the next one |
| Only `<dump_file>.old` (`dump_file` gone or corrupt) | `dgd config_file dump_file.old` | `State restored.` (the previous generation; work between the two dumps is lost) | Same failure again means `.old` is unusable too -- cold-boot from clean state (next rows) |
| Neither dump file usable, but the `src` tree and Vault XML survive | `dgd config_file` (cold boot, no restore arguments) then respawn per domain from the Vault store | `Initialization complete.` (cold-boot banner, not `State restored.`), then each domain's own respawn confirmation | Rebuilds only schema-exported state (What the Vault XML backup can recover below); a dangling reference fails one domain's `configure` step without stopping the others -- check `system.log` for `Warning:: Schema node` / `VAULT: Configuration failed` |
| Nothing survives (no tree, no dumps, no Vault XML) | Redeploy the `src` tree from source control, then cold boot | `Initialization complete.` | Every domain starts from its `initd.c` defaults; this is a rebuild, not a recovery -- the row above is the reason backups get copied off-host (Off-host sets carry credentials below) |

A complete backup covers more than the dump file:

| Item | Why | Notes |
|---|---|---|
| `dump_file` and `<dump_file>.old` | The statedump pair | Rotation moves the previous file to `.old` before the new one is written (State persistence above). Copy both together, not `dump_file` alone |
| `src/kernel/data/` | Admin credentials and access bits | File-backed independently of the snapshot cycle: the admin password is written on every change, and access grants on every kernel-console `grant` / `ungrant` ("written to host files the moment they changed ... deliberately file-backed, so they survive even without a snapshot", `docs/first-hour.md`); grants made outside those verbs ride the image until the next flush |
| Vault data directories (the Vault daemon's on-disk store, `/usr/Vault/data/vault/<Domain>/...`) | Schema-exported per-domain state | The Vault daemon's own XML storage root, kept separately from the object graph's in-memory copy (`docs/vault-applications.md`) |
| Loaded extension binaries and the `.dgd` `modules` mapping | Restore precondition, not a file to copy | "statedumps created with a specific kfun extension in effect will require the the same kfun extension on restore" (Loading host-driver extensions below, quoting the 2010 Hydra note). Without the same extensions available, a snapshot will not restore at all. The inverse direction is subtler, and split by level (both halves observed live): a snapshot taken without an extension restores cleanly under a driver that loads it, and the module's raw kfuns DO work in the restored image -- but the platform daemons that probed for the module at their cold-boot creation carry their stand-down as image state, so the capabilities built on the module (identity minting, sessions, native TLS) still answer "crypto module not loaded" until a cold boot re-runs the probes. Operationally an extension therefore remains a cold-boot decision: the daemon surface, not the kfun table, is what a deployment consumes |

**Safe copy.** Copy after a dump completes, not against a swap file mid-write: `dump_state` briefly blocks the platform while it runs (`docs/persistence.md` The statedump cycle), and the rotation is a rename rather than an in-place edit, but the window is still real. Take a deliberate full dump first (the `snapshot` verb) so the backup generation is self-sufficient, then copy `dump_file` and `<dump_file>.old` together. Keeping the pair costs one extra file and removes any question of which generation is on disk.

**Restore.** Two forms, both invoked as `dgd config_file [restore files]`:

- **Full restore**: `dgd config_file dump_file`. Works when `dump_file` holds a full snapshot: the `snapshot` verb (`dump_state(FALSE)`, `src/kernel/lib/admin_console.c` `cmd_snapshot`), the programmatic dump-only surface (`persist_helper->trigger_dump()`, `docs/persistence.md` The programmatic surface), and the dump-and-exit path (`dump_state(FALSE)`, `src/usr/System/sys/persist_helper.c:83`) all leave one.
- **Two-file (incremental) restore**: `dgd config_file dump_file dump_file.old`. Required when `dump_file` holds an incremental snapshot written by `dump_state(1)`/`dump_state(TRUE)`. The argument order (the current dump file first, its full base second) is verified from the DGD driver's own usage line, `Usage: dgd config_file [[partial_snapshot] snapshot]`, and from `Config::restore(fd, fd2)`: the header read from the first file is checked for the partial flag, and the second file is opened only to back it. This is the same order already documented for the `.dgd` file's `hotboot` tuple (`{ binary, config, snapshot, snapshot.old }`, `docs/configuration.md` The .dgd configuration file). An unset second argument on a partial primary fails at boot with "Missing secondary snapshot".

This two-file form is a different recovery than the corrupt-snapshot fallback in Common failure modes below, which discards the newer `dump_file` outright and restores from `<dump_file>.old` alone as a self-contained snapshot. The two-file form instead restores using both files together, applying the incremental on top of its base.

Which stop path leaves which case:

| Stop path | Call | Restore needs |
|---|---|---|
| Kill signal (SIGTERM) | `prepare_reboot()` then `dump_state(1)` (`src/kernel/sys/driver.c:757-766`) | `dump_file` + `dump_file.old` (in practice, see below) |
| `reboot` verb | `dump_state(TRUE)` then `shutdown()` (`cmd_reboot`, `src/kernel/lib/admin_console.c`) | `dump_file` + `dump_file.old` (in practice, see below) |
| `snapshot` verb | `dump_state(FALSE)` (`cmd_snapshot`) | `dump_file` alone |
| Console dump-and-exit path | `dump_state(FALSE)` then `shutdown()` (`src/usr/System/sys/persist_helper.c:83`) | `dump_file` alone |
| Programmatic dump-only (not a stop: the runtime keeps serving) | `dump_state(FALSE)` (`persist_helper->trigger_dump()`, capability-gated) | `dump_file` alone |

A supervisor sending SIGTERM (the ordinary "stop the service" path outside admin_console) and the `reboot` verb both write an incremental. Strictly, a `dump_state(1)` dump is written as a partial only when swapped-out objects are pending at dump time. A small freshly-booted image can produce a self-contained file (which is why the tutorial's single-file restore succeeds after its first `reboot`), but on a long-running image the partial case is the norm. Routine operator practice should keep `<dump_file>.old` alongside `dump_file` rather than treat it as disposable. A restore attempted with only `dump_file` after either path is the likely cause of a "Missing secondary snapshot" failure at boot.

**The off-host restore drill** (performed once, 2026-07-12): a snapshot written by the macOS/arm64 driver restored under the Linux/aarch64 driver built from the same source -- `State restored.` on the first boot line, and the deployed example's post-restore test phases ran to completion on the foreign host (its full sentinel count, including the persistence-verification phase). The procedure that worked, in full:

1. Copy the backup set to the target host: the `src` tree (which carries `src/kernel/data/` and the Vault XML directories inside it), the dump pair, and the `.dgd` config.
2. Edit one config field: `directory` to the tree's absolute path on the new host. The state-file fields resolve relative to `directory`, so a layout-preserving copy needs no other edit for the restore itself; the restore arguments resolve against the invocation directory. A rehearsal boot on a shared or reachable host additionally remaps the ports first (Rehearsal-boot hygiene below).
3. Start the driver naming the snapshot: `dgd config dump_file [dump_file.old]`.

Portability is stated exactly as tested: one macOS/arm64-to-Linux/aarch64 restore with driver binaries built from the same source succeeded. Other host and architecture pairs are unverified; the driver's own guard for an unusable file is the `Bad or incompatible restore file header` refusal, so an incompatible pair fails at boot rather than corrupting. This same drill is the reusable procedure behind two recurring practices: the scheduled restorability check (step 5 below) and the pre-release and host-binary-upgrade rehearsal (`docs/changing-a-running-system.md` Shipping a release).

**Backup-set coherence.** Take the dump pair and the tree at the same cut. The snapshot carries the compiled programs and all object state; the tree is what future compiles and cold boots build from, and it also carries the file-backed siblings (kernel data, Vault XML). A backup that pairs an older snapshot with a newer tree restores the older image state and will recompile against the newer sources on the next upgrade -- and Vault XML newer than the image diverges the other way: the next explicit Vault import (a respawn by name from the owning domain) re-imports state the image predates -- nothing re-reads the XML at restore itself. Neither is corruption; both are divergence you chose by mixing cuts.

**What the Vault XML backup can recover.** The backup table has you copy the Vault data directories, but nothing re-reads that XML at restore -- so its recovery tier needs stating. The ladder, best case first:

- **Both dump files usable** -- restore the pair; the XML is not consulted. This is the normal path.
- **Only `<dump_file>.old`** -- restore the previous generation; you lose the work between the two dumps, nothing more.
- **The dump pair is gone or corrupt and the Vault XML survives** -- cold boot from the tree, then respawn per domain. This rebuilds only the schema-exported state each domain persisted through the Vault: the non-Vault object graph and any pending `call_out`s are not in the XML and do not come back.

Respawn is application code, not an operator verb: each domain respawns its own stored objects from inside its own context (`spawn_create_one` / `spawn_configure_one` on a vault node), and no bulk-respawn tooling ships (`docs/vault-applications.md` The owning-domain respawn). Two constraints matter at recovery scale. Order the respawns so a referenced object loads before its referrer -- a dangling `lpc_obj` reference fails the whole `configure` step for the referring object, which then carries only its `create()`-time state, not just the missing field (`docs/vault-applications.md`). And check the result: the only trace of a skipped or failed import is two `system.log` lines, `Warning:: Schema node` and `VAULT: Configuration failed` -- grep for both after the sweep. The disaster rung is not the ladder's only customer: a *planned* cold boot forced by a kernel-tier change with no live path -- the kernel-auto matrix row -- runs the same drill deliberately, and prices it at design time (`docs/changing-a-running-system.md` Changing the kernel layer).

**Scheduled off-host backup, end to end.** The unattended composition of the pieces above, from the host's scheduler (cron, launchd, a systemd timer):

1. **Trigger a deliberate full dump headlessly.** `scripts/drive-verbs.py` (the regression harness's telnet client, `scripts/README.md`) logs into the console and drives verbs from a file: a one-line ephemeral verbset -- `cmd: snapshot`; the verb succeeds silently, and step 2's mtime move is its acknowledgment -- makes the backup generation self-sufficient (the `snapshot` verb writes a full snapshot, the stop-path table above), rather than gambling on what the last `dump_interval` cycle left. The trigger authenticates as an operator (file-level `user:`/`password:` directives); remember the monitoring-credential caveat under Monitoring signals -- a console credential is never read-only in blast radius.
2. **Gate the copy on dump quiescence.** Proceed when `dump_file`'s modification time is newer than the trigger and stable: two mtime reads a few seconds apart that agree (A stalled snapshot above explains why mtime is the signal).
3. **Copy the backup set at one cut**: `dump_file` and `<dump_file>.old` together, plus the `src` tree, which carries `src/kernel/data/` and the Vault XML inside it -- the full table above, at the same cut (Backup-set coherence above).
4. **Close the race.** The automatic `dump_interval` cycle can rotate the pair mid-copy. After the copy, re-read `dump_file`'s mtime; if it moved during the copy, redo the copy. With `dump_interval` sized in hours the retry is rare.
5. **Prove a generation restores.** On a schedule -- per generation, or weekly -- boot a throwaway driver on the backup host against the copied set, exactly the off-host restore drill above: assert `State restored.` on the first boot line plus the application's own sentinel probes, then discard the boot. Alert on failure. A backup no generation of which has ever restored is a hope, not a recovery plan, and the corrupt-snapshot failure mode below makes the stakes concrete: the `.old` fallback only helps if some generation is known-restorable. The same rehearsal boot doubles as the pre-release and host-binary-upgrade rehearsal (`docs/changing-a-running-system.md` Shipping a release).

**Retention.** Each generation is self-sufficient -- the dump pair plus the tree at the same cut -- so retention cost is linear in generations, and the restore ladder above never reaches past one generation on its own (`<dump_file>.old` backs only its own pair). A defensible starting shape: a week of daily generations, the newest of them proven restorable by step 5, thinned to monthly beyond; tune the count against the same recovery-point economics that size `dump_interval` (Availability and data-loss model below).

**Off-host sets carry credentials -- encrypt at rest.** The copied tree includes `src/kernel/data/` (the admin password hash and the access lists), and the dump pair carries the entire application state (the sensitivity table under State file locations and permissions below). Off-host generations therefore get encryption at rest and owner-only permissions as a floor. The property matters, not the tool: disk encryption on the backup volume or per-generation file encryption both satisfy it.

**Rehearsal-boot hygiene.** Step 5's throwaway boot brings production up on the backup host: the restored image answers to production's operator passwords, the copied `src/kernel/data/admin.pwd` governs the admin login, and the stock config's bare `binary_port = 8080` form listens on every interface (observed live; the mapping form binds an address -- `telnet_port` already uses it in the stock config). Give the drill its own config: `binary_port` rewritten to the localhost-mapping form (`([ "localhost" : <port> ])`), port numbers on both listeners that cannot collide with anything else on the backup host, and discard the boot's state files with the drill. The rehearsal proves restorability; it does not stand up a second production.

**Post-restore checklist.** `State restored.` as the first boot line after the version banner; the application's own verification (its sentinel driver or probes); clients reconnect (connections never survive a statedump restore); `Missing secondary snapshot` means an incremental primary was named without its base, and `Bad or incompatible restore file header` means the file and binary do not match. Expect one transient on an aged snapshot: its accumulated overdue `call_out` backlog fires immediately on restore, a burst of activity, log volume, and callout count that is catch-up, not a fault (`docs/persistence.md` Persistence boundaries, the Time bullet, carries the application-side lateness idioms).

## Availability and data-loss model

The platform is a single process on a single machine. There is no replica to fail over to and no distributed consensus to reason about (`docs/persistence.md`: "this platform is deliberately single-coherence-domain"). Availability and data loss are properties of one process's dump-and-restore cycle, not of a cluster.

**Recovery point.** The RPO is `dump_interval` (`docs/configuration.md` The .dgd configuration file), a sizing decision the operator makes, not a platform-supplied guarantee. Work committed after the last completed dump and lost on an unclean stop is bounded by that interval. The prior snapshot is untouched by a failed or interrupted dump attempt and remains a valid restore point (Backing up and restoring state above).

**Crash semantics.** A crash, such as a process killed before any dump runs, host power loss, or a `dump_state` failure mid-write (Common failure modes below), loses everything committed since the last completed dump. The platform does not partially apply an interrupted dump.

**Recurring pause.** The availability cost that recurs by design: every `dump_interval` cycle the whole runtime briefly blocks while the image writes ("the runtime briefly blocks during the dump", `docs/persistence.md` The statedump cycle) -- the one head-of-line stall the tick budget does not bound, because it is the runtime writing, not a task running (`docs/execution-model.md` The price: head-of-line latency names it as the exception). Measured to a 1.08 GB image at a raised `sector_size` the client-observed pause reached 1.233 s (0.037 s at the stock `sector_size` for a comparably sized image); the pause scales with image size and, on this evidence, with `sector_size` as well (`docs/configuration.md` Snapshot-pause scaling, measured across two sector sizes); beyond that envelope it is unmeasured (`docs/configuration.md` Unmeasured today) -- a growing image re-measures with `scripts/measure-baseline.py` rather than extrapolating. Sizing `dump_interval` therefore trades on both axes at once: shorter narrows the recovery point above and pays the pause more often.

**Downtime taxonomy.**

| Mode | Trigger | Connections | State |
|---|---|---|---|
| Hot boot | `shutdown(1)` + `execv`, with a `hotboot` tuple configured (Booting above) | Survive: inherited file descriptors | Survives: dump plus immediate reload |
| Statedump restore | Cold start naming the snapshot on the command line (full, or the two-file incremental form) | Drop: clients reconnect | Survives, from the dump file(s) |
| Cold boot | Cold start with no restore argument | Drop | Rebuilt from source: only what the initd cascade recreates, nothing carried over |

**Recovery time.** The recovery point above bounds what is lost; recovery time -- how long until service returns -- has two parts with different shapes. The down-window is supervisor detection plus restore boot: the restore boot itself measured at 0.06 s to console-ready, holding at that figure from a 237 MB snapshot up through a 1.08 GB snapshot (`docs/configuration.md` Snapshot-pause scaling, measured across two sector sizes), because readiness precedes the data -- state pages in on demand after it. Time to steady state is the longer tail: clients reconnect (connections never survive a restore, the taxonomy above), demand paging warms as state is first touched, and an aged snapshot's overdue `call_out` backlog fires immediately as a catch-up burst (Post-restore checklist above). An SLA or incident playbook budgets the down-window from the supervisor's detection interval plus the measured restore boot, and expects the warmup tail, not the boot, to dominate what users observe. The same envelope caveat applies: measured to a 1.08 GB image, unmeasured beyond (`docs/configuration.md` Unmeasured today).

**Portability.** A snapshot restores only against a driver started with the same `auto_object` and `driver_object`, and with the same `modules` extensions loaded (Common failure modes below, the same conditions `docs/persistence.md` states for hot boot). It is a resume point for a specific configuration, not a portable backup format across incompatible driver configurations.

**Availability arithmetic, worked.** The pieces above, composed into three numbers for one concrete deployment shape: a 506 MB image, stock `sector_size`, `dump_interval` 3600 (one hour). The numbers are this rig's, measured on the hardware named in `docs/configuration.md` Snapshot-pause scaling -- not a guarantee for any other machine or workload.

- **Planned pause time per day.** `dump_interval` 3600 runs 24 dumps a day; the stock-`sector_size` client-observed pause at this image size is the top of the measured 0.003-0.037 s range (`docs/configuration.md` Snapshot-pause scaling, measured across two sector sizes). 24 x 0.037 s is about 0.9 s of runtime-blocked time per day.
- **Worst-case unclean-stop loss.** The RPO is `dump_interval` itself ("The RPO is `dump_interval` ... a sizing decision the operator makes, not a platform-supplied guarantee", Recovery point above): up to one hour of committed-but-undumped writes, including writes already acknowledged to clients.
- **Expected down-window for a supervisor restart.** The restore boot measured at 0.06 s to console-ready, holding at that figure from a 237 MB snapshot up through a 1.08 GB snapshot (Recovery time above, `docs/configuration.md` Snapshot-pause scaling); a systemd `Restart=on-failure` unit (Running under a supervisor below) reacts to the process's own exit without a health-check poll, so detection is near-instant for a crash and the down-window is dominated by that 0.06 s restore boot. A supervisor that instead polls a health check on an interval adds that interval on top -- not measured here, and worth pricing against the specific supervisor configuration in use.

Time to steady state -- clients reconnecting, demand paging warming, an overdue `call_out` backlog catching up -- is the longer tail Recovery time names above, and is not included in the down-window figure: it depends on the deployment's own reconnect and warmup behavior, not a rig measurement.

## Logging and diagnostics

The driver provides a `message(string)` function that timestamps and emits diagnostic output:

```c
ctime(time())[4..18] + " ** " + str
```

`message` is called by the driver during initialization, snapshot restore, and on interrupt. It is callable by kernel-tier and System-tier code. The underlying `send_message` kfun routes the output to the connection that triggered the current call (typically the operator at admin_console). Application-tier code does not invoke `message` directly.

`message` emits exactly the string it is given -- no newline is appended (`src/kernel/sys/driver.c` `message`), and whether a line ends is the caller's choice. The boot banner's messages carry their own `\n`; the deferred-startup `NOTICE` burst's do not, so in a captured driver log (a supervisor's capture, `dgd ... > boot.log`, CI) the whole burst lands as one physical line. Grep captured boot logs for substrings (`Warning::`, `import_state FAILED`); a line-oriented check (`grep -c NOTICE`, line counts) undercounts.

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

## Running under a supervisor

The platform is one process. A process supervisor (systemd, a container runtime, runit) owns its lifecycle: start it, restart it on exit, stop it on demand. Two facts shape that configuration.

**A graceful stop takes a final snapshot.** The driver catches `SIGTERM`, the default stop signal for `systemctl stop`, `docker stop`, and a bare `kill`. On receipt it runs `prepare_reboot()`, writes an incremental snapshot with `dump_state(1)`, and shuts down cold (`src/kernel/sys/driver.c:757-766`, reached through the `SIGTERM` handler in `dworkin/dgd` `src/host/unix/local.cpp`). A supervisor's ordinary stop therefore leaves a current restore point with no operator action. This is the same incremental form the `reboot` verb writes (Backing up and restoring state above), so recovery needs both `dump_file` and `<dump_file>.old`.

Give the stop timeout room for the dump. Dump time scales with the in-memory image size (`docs/configuration.md` Limits and capacity), so a large image needs a stop timeout longer than a supervisor's default. A supervisor that escalates to `SIGKILL` before the dump finishes loses that snapshot.

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
# Scale with image size (docs/configuration.md Limits and capacity).
TimeoutStopSec=300

[Install]
WantedBy=multi-user.target
```

`KillSignal` stays `SIGTERM` because it is the only signal the driver catches for the graceful snapshot; `Restart=on-failure` rather than `always` so a deliberate operator `halt` stays down.

**Running in a container.** The repository's own `Dockerfile` is dev/CI-only (its header says nothing persists across runs unless `state/` is mounted, and its `ENTRYPOINT` runs the regression harness, not a production boot) -- it is the wrong base for this shape. Rather than build an image and then override its working directory and entrypoint out from under it, mount the same `/srv/eos` host layout the systemd unit above already assumes (the `dgd` binary, `run-dgd.sh`, `server.dgd`, `state/`, `src/` all live there per that section) into a stock minimal image that supplies nothing but a userspace for the mounted binary to run in. A worked service, syntax-validated with `docker compose config`:

```yaml
services:
  eos:
    image: debian:bookworm-slim
    user: "1000:1000"
    working_dir: /srv/eos
    entrypoint: ["/srv/eos/run-dgd.sh"]
    volumes:
      - /srv/eos:/srv/eos
    ports:
      - "127.0.0.1:8023:8023"    # telnet_port: host-loopback only, never published
      - "8080:8080"              # binary_port
    stop_signal: SIGTERM
    stop_grace_period: 300s
    restart: on-failure
```

Each field traces to a fact stated above or in `docs/configuration.md`:

- `image: debian:bookworm-slim` matches the base the repository's own `Dockerfile` builds `dgd` against, so the mounted binary's glibc expectations match the container's -- a host-built `dgd` bind-mounted into an image on a different base (a different glibc, or a musl-based image like `alpine`) is not guaranteed to run; pin the container's base to whatever distro actually built the binary being mounted, not necessarily this one.
- `volumes: - /srv/eos:/srv/eos` is the whole point of this shape: the image itself carries no `dgd` binary and no LPC source tree, so `working_dir: /srv/eos` and `entrypoint: ["/srv/eos/run-dgd.sh"]` below resolve only because this one bind mount supplies every path both of them name -- the binary at `/srv/eos/dgd/bin/dgd`, the wrapper script itself, `server.dgd`, and the `state/` and `src/` directories the wrapper and the restore need (the snapshot pair `dump_file` / `<dump_file>.old`, `swap_file`, and the object tree the snapshot references). None of it is baked into the image, so a container recreate does not discard it.
- `entrypoint: ["/srv/eos/run-dgd.sh"]` reuses the boot-state-invariant wrapper from this section verbatim -- the same fork-in-the-road (restore arguments fatal when the files are absent, silently stale when omitted with a snapshot present) applies inside a container, and the mount above is what makes this path resolve.
- `stop_signal: SIGTERM` and `stop_grace_period: 300s` are the container-runtime names for the same two facts the systemd unit's `KillSignal` and `TimeoutStopSec` encode above: `SIGTERM` is the only signal the driver catches for the snapshot-then-exit path, and the grace period must outlast the dump, which scales with image size (`docs/configuration.md` Limits and capacity) -- size it the same way `TimeoutStopSec=300` was sized.
- `restart: on-failure` matches the systemd unit's choice and its reason: a deliberate operator `halt` should stay down, not bounce.
- `user: "1000:1000"` runs the process as an unprivileged UID, the container analogue of the unit's `User=eos`; it must own (or have read/write access to) the mounted `/srv/eos` tree on the host side, the same requirement the systemd unit's `User=eos` carries.
- the `telnet_port` publish binds to `127.0.0.1` on the host, keeping the operator console reachable only through the host's own loopback or an SSH tunnel into it (Network boundary and transport security below) -- never publish it on `0.0.0.0` or a container network others can reach; `binary_port` is the only port meant for outside traffic.

**Native TLS.** The platform terminates TLS 1.3 itself (Network boundary and transport security below): configure a second `binary_port` entry for the `https` label, load the crypto module (Loading host-driver extensions below), and place PEM credentials at the configured paths. The host's ACME client owns issuance and renewal -- e.g. a certbot deploy hook copying fullchain and private key into `<directory>/usr/System/data/tls/` (readable only by the runtime user). A first-ever certificate that lands after boot is activated with `tls-cert reload` on the console; renewals need only the file copy, since credentials are read per connection. The key may be RSA or RSA-PSS in PKCS#8 form (`BEGIN PRIVATE KEY`), ECDSA over P-256, P-384, or P-521 in PKCS#8 or SEC1 form (`BEGIN EC PRIVATE KEY`), or Ed25519/Ed448 in PKCS#8 form -- Let's Encrypt's default ECDSA issuance works as-is. Traditional PKCS#1 RSA PEM (`BEGIN RSA PRIVATE KEY`) is not accepted; convert with `openssl pkcs8 -topk8 -nocrypt`.

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

- `sectors: used / total (%)` -- swap-device occupancy against the `.dgd` `swap_size` cap, with `sector size` echoing `sector_size`. This is the one fatal ceiling: an image that outgrows the swap device dies with a fatal `out of sectors` error (`docs/configuration.md` Limits and capacity), so this percentage belongs in the alert set, with an earlier threshold than the degrading signals (table below).
- `swap average: a, b` -- objects swapped out per second, averaged over the last minute and the last five minutes (the driver counts swapouts per window; the console divides by 60 and 300). Near zero when the resident set fits memory; sustained non-zero values are the "sustained churn" the alert table names -- every access is paging.
- `static:` / `dynamic:` -- bytes in use versus bytes allocated from the host, for the two allocators: static holds the boot-time infrastructure tables (sized to the `.dgd` caps) and long-lived driver buffers, dynamic holds compiled programs and object data; the third row is their sum. The runtime grows both by chunks (`static_chunk` / `dynamic_chunk`), so the percentage reads utilization of the current allocation, not distance to a configured ceiling -- for capacity planning watch the absolute totals and the process RSS, not the percentage.
- `short:` / `other:` -- queued callouts, split by the driver's scheduling horizon: whole-second callouts due soon (within roughly the next two minutes) versus longer-dated and millisecond-delay ones; then their sum against the `call_outs` table cap -- the alert-table row.
- `Objects:` -- objects in use (masters plus clones) against the `objects` cap.
- `Users:` -- live connections against the `users` cap.
- `Server`, `Start time`, `Uptime` -- the driver banner (a `master`-built driver still prints the base release string, `docs/getting-started.md`), boot wall-clock, and time since -- the unexpected-reset signal.

**What an alertable line looks like.** A runtime error persists into `system.log` as a multi-line block: one timestamped `ERROR` header line carrying the message, then the indented trace frames beneath it (observed by tailing the log after a forced fault). Match alerting rules on the header (` ERROR `), not on frame lines; one fault produces one header and many frames.

**Capacity headroom, from `status()`.** The no-argument `status()` health vector (the `status` verb, `docs/admin-console.md`) carries the counts to watch against the `.dgd` caps (`docs/configuration.md` Limits and capacity):

| Signal | Alert condition | Starting threshold | Reading |
|---|---|---|---|
| call_out count vs the `call_outs` cap | Approaching the cap | Warn at 70% of `call_outs`, page at 85% | A backlog of deferred work: new `call_out`s begin to fail |
| object count vs the `objects` cap | Approaching the cap | Warn at 70% of `objects`, page at 85% | Allocation headroom is running out: clones and new objects begin to fail |
| swap sectors vs the `swap_size` cap | Rising occupancy, alerted earlier than the rows above | Warn at 50% of `swap_size`, page at 70% | The one ceiling that is fatal rather than degrading: at the cap the platform dies with `out of sectors` (`docs/configuration.md` Limits and capacity). The durable fix is a `sector_size` raise and a reboot from snapshot; the full ladder is When the image approaches the state ceiling, below |
| users count vs the `users` cap | Approaching the cap | Warn at 70% of `users`, page at 85% | At the cap, new connections complete their TCP connect and are never answered, with nothing logged -- the silent form of full. A climbing count under flat traffic is a connection leak (Common failure modes below) |
| swap activity | Sustained churn | Warn when the five-minute average is nonzero on two consecutive polls; page when it is still nonzero fifteen minutes later | The resident set exceeds memory and every access pages. A `swapout` relieves pressure; the durable fix is a config raise and reboot (Config changes across a restore, above, is what a restore boot accepts) |
| uptime, last reboot | Reset unexpectedly | Page on any decrease | The platform restarted: check it against the supervisor's restart log and the snapshot cadence |
| health-route response time | Sustained elevation over the deployment's measured baseline | Probe the health route on the existing polling interval; warn when the median holds at several times your measured baseline across two consecutive polls, page when it is still elevated fifteen minutes later | The signature degradation the count rows cannot see: queueing. The platform's own measurement served a ~260 ms median under saturation while `users` -- the one capacity count that run reported -- sat at 2/255, nowhere near alertable (`docs/configuration.md` Limits and capacity). Attribution is the paragraph below |
| `dump_file` modification time | Stalled | Page when the mtime is older than `dump_interval` plus the measured dump duration | Automatic persistence has stopped completing, most often disk-full or a permissions problem on the dump directory (A stalled snapshot below) |
| host-disk occupancy on the state volume | Rising, alongside the stalled-snapshot signal | Warn at 70%, page at 85% of the provisioned volume | The dump rotation, the swap file, and a running backup's staged copy share this volume (Sizing the state volume below); a full disk first surfaces as `dump_state` erroring out (Common failure modes below) |
| process RSS | Trending up beyond the documented chunk-ahead growth | Page when RSS keeps climbing across polls under an otherwise-flat workload | Resident memory runs several times the on-disk image because the dynamic allocator arena grows in chunks ahead of use (`docs/configuration.md` Limits and capacity); a climbing trend under flat traffic is the signal to distinguish from that expected growth, not the percentage in the `status()` block (static/dynamic, above) |

The threshold column is a starting point, not a guarantee -- the same posture as the production-shape starting point under `docs/configuration.md` Limits and capacity: numbers to write the first alert rule with, then tune against the occupancy your own workload measures. The gap between the swap-sector thresholds and the degrading rows is deliberate: the fatal ceiling gets the earlier warning.

No published run holds these signals over days or weeks (`docs/configuration.md` Unmeasured today), so read a trend, not just a level, when tuning: the dynamic-arena total is expected to grow in chunks and plateau once a workload's working set stops widening, while `objects`, `call_outs`, and swap-sector occupancy are expected to plateau under a steady workload shape and climb only when the workload itself is growing -- a signal that keeps climbing under otherwise-flat traffic is the leak or backlog shape the count rows above call out, not the documented chunk-ahead behavior.

**When the latency row fires and every count row is green**, the cause is one of three, each with its own signature. A burst of near-budget tasks: elevated medians that recover between bursts; attribute with `rsrc ticks` (`docs/configuration.md` Resource limits), which names the owner far above its peers. The recurring dump pause: elevation at the `dump_interval` cadence, bounded by the measured pause and expected (Availability and data-loss model above). Sustained saturation of the one serialization point: elevation that tracks offered load and recovers only when load drops (`docs/execution-model.md` Under sustained load). The measured worst case makes the blind spot concrete: a saturated driver held the health route's median latency around 260 ms while `users` -- the capacity count that measurement reported -- read 2/255 (`docs/configuration.md` Limits and capacity).

Per-owner tick consumption is the other capacity signal. `rsrc ticks` (the resource daemon, `docs/configuration.md` Resource limits) reports each owner's tick usage against its budget. An owner far above its peers is running away. A tick-exhausted call rolls back rather than hanging the platform.

**Alertable log lines.** `logd` writes `system.log` and tees `errord`'s reports there at ERROR level (Logging and diagnostics above):

- ERROR lines in `system.log`: every uncaught runtime, atomic, and compile error routes through `errord` to this sink. A rising ERROR rate is the platform-level fault signal.
- `cascade-aborted` and cycle-detection lines in `/usr/Merry/log/dispatch.log`: the property-change dispatcher records observer-cycle detection and cascade-depth overflow here (Logging and diagnostics above). These mark misbehaving application observer wiring.

**A stalled snapshot.** The most reliable unattended signal that automatic persistence has stopped is the `dump_file` modification time. A successful dump rewrites it (automatic every `dump_interval`, plus any explicit one), and the rotation moves the prior file to `<dump_file>.old` first (State persistence above). A `dump_file` mtime older than `dump_interval` plus the dump duration means automatic snapshots are no longer completing, most often disk-full or a permissions problem on the dump directory (Common failure modes below). Because the rotation writes `<dump_file>.old` before the new file, a failed write leaves the prior snapshot intact as a restore point. An operator-invoked `snapshot` that fails also reports to the console and, through `errord`, to `system.log`.

**Headless polling.** Reaching these signals without an operator borrows the mechanism the regression harness already relies on: a client drives the `admin_console` verbs over the telnet port and checks the replies. `scripts/drive-verbs.py` (documented in `scripts/README.md` and run by `drive-verbs-smoke.sh`) connects, authenticates, runs verbs such as `status`, and matches expected output. A monitoring probe follows the same shape: it reads the health vector on an interval and alerts on the thresholds above -- the worked collector, credential to verbset to field-by-field threshold mapping, is `docs/common-tasks.md` Poll the health vector without an HTTP route. An application can instead expose a runtime-derived health check on its own HTTP transport; the shipped `examples/http-app` demonstrates exactly that with `GET /status`, returning the capacity-headroom counts as `key=used/cap` lines a probe parses without a console login (`docs/common-tasks.md` Expose a health check for monitoring). That check rides `binary_port`, cleartext or TLS (Network boundary and transport security below).

## When the image approaches the state ceiling

The swap-sector alert above has a hard endpoint: 65535 sectors times the 65535-byte maximum `sector_size` is just under 4.0 GiB of total persistent state on a stock build, absolute however much RAM the host has (`docs/configuration.md` Limits and capacity). Attribution of what is growing is the console-side procedure (`docs/admin-console.md` Debugging a stuck platform, the "Swap is growing but object counts are flat" scenario). The headroom moves, in the order to spend them:

1. **Raise `sector_size`** while sector count is the binding axis: a config change and a reboot from snapshot (Config changes across a restore above), buying headroom up to the compiled 65535-byte maximum at the cost of coarser allocation -- `scripts/measure-baseline.py` does exactly this to fit its grown images.
2. **Audit Vault-export coverage ahead of need.** The terminal move below carries only schema-exported state (`docs/persistence.md` Getting data out), so widening what the application persists through the Vault is work done before the ceiling, not at it. The recovery ladder's cold rung (What the Vault XML backup can recover, above) is the same boundary: what is not in the XML does not come back.
3. **Re-check the wider-index driver rebuild's status.** It is the in-principle fix `docs/configuration.md` names for the compiled ceilings, and the same document records it as observed to segfault naively (`docs/building.md`) -- so as of that record the move is unavailable: confirm nothing has changed, then plan as if 65535 objects and just-under-4-GiB are hard.
4. **Terminal move: migrate to a fresh instance by export.** Cold-boot a new instance from the tree and respawn per domain -- operationally the disaster rung of the Vault recovery ladder run deliberately, with the same constraints: respawn is application code, reference order matters, and the two `system.log` warning lines are the only trace of a failed import. What does not survive is the same list: the non-Vault object graph and pending `call_out`s. Splitting the workload by domain across two instances is mechanically the same move done selectively, but it leaves the platform's fit envelope: two instances are two coherence domains -- nothing platform-level spans them, and `docs/evaluating.md` names multi-machine workloads as a non-fit. That fork is an application-architecture decision to price at adoption time (`docs/evaluating.md` The ceilings), not an operator remedy.

## Network boundary and transport security

The platform listens on two kinds of port (`docs/configuration.md` The .dgd configuration file): `telnet_port` for the operator console, and `binary_port` for application transports (HTTP and others). Their exposure and transport security are separate decisions.

**The operator console is unencrypted.** `admin_console` speaks plain telnet: the wire carries the operator's password and every command in clear text. Bind `telnet_port` to a loopback interface or a dedicated maintenance network, never a public one, and reach it through an SSH tunnel or a host-terminated TLS tunnel. Console access is equivalent to host shell access on the platform's process (`docs/admin-console.md` Console security posture), so the tunnel endpoint and its credentials carry that weight.

**A worked console tunnel.** The prescription above made literal, in the same spirit as the supervisor unit and proxy blocks: a dedicated account on the platform host that can forward exactly one port and do nothing else. In `sshd_config`:

```text
Match User dgd-console
    AllowTcpForwarding local
    PermitOpen 127.0.0.1:8023
    PermitTTY no
    ForceCommand /usr/bin/false
    PasswordAuthentication no
    AllowAgentForwarding no
    X11Forwarding no
```

The operator's side is one invocation, then a local telnet against the near end of the tunnel:

```text
ssh -N -L 8023:127.0.0.1:8023 dgd-console@<host>
telnet 127.0.0.1 8023
```

`-N` asks for no remote command, which is also all the account allows: `PermitOpen` pins forwarding to the console port, `PermitTTY no` and the `ForceCommand` refuse an interactive session, and `PasswordAuthentication no` makes the account key-only -- one `authorized_keys` entry per operator, which doubles as the access roster. Adjust `8023` to the deployment's `telnet_port` value.

**Where session recording attaches.** The platform records no operator actions (`docs/security-posture.md` Non-goals and known limits), so a deployment that requires a console audit trail builds it into the tunnel endpoint. The mechanism: a second account whose forced command IS the console client wrapped in a recorder, instead of a port forward --

```text
Match User dgd-console-rec
    AllowTcpForwarding no
    PermitTTY yes
    ForceCommand /usr/bin/script -a /var/log/console-sessions/$(date +%Y%m%d-%H%M%S)-$$.log -c "telnet 127.0.0.1 8023"
    AllowAgentForwarding no
    X11Forwarding no
```

(`ForceCommand` runs through the account's login shell, so the account needs a real shell for the substitutions; `script` here is the util-linux form.) Every keystroke and response lands in a per-session log on the host -- treat that directory as security log storage: root-owned with the account able to create but not prune (a sticky drop-box directory), shipped or rotated off-host promptly. The recording is only as trustworthy as the host that takes it: an operator with separate shell access to the platform host can bypass the recorded path entirely, the same boundary the console-equals-host-shell equivalence above already draws.

**Application HTTP terminates TLS natively.** Native TLS 1.3 termination is the platform transport (`docs/runtime-platform-roadmap.md` Transport posture records the activation): the HTTPS bootstrap (`src/usr/System/sys/https_server.c`) serves the labeled `https` binary port, cloning the application's TLS server mount (`/usr/WWW/obj/tls_server`) per connection -- `examples/https-app/` is the reference application. Activation needs three things: the lpc-ext crypto module (the TLS stack's body is gated on the host driver being built with `KF_SECURE_RANDOM`; Loading host-driver extensions below), a second `binary_port` entry (the port-label registry declares `https` for index 1), and PEM credentials at the configured paths (`/usr/System/data/tls/cert.pem` and `key.pem` by default). Anything missing is a logged stand-down, not an error. The `tls-cert` console verb reports status, revalidates the files, re-points the paths, and completes a deferred registration without a restart (`docs/admin-console.md`). Certificate acquisition and renewal stay with the host's ACME client writing those paths; credentials are read per connection, so a renewed file is picked up on the next handshake with no verb. The platform sees the real client address -- there is no proxy hop whose origin information would need reconstructing from forwarded headers.

**The private key does not persist.** The HTTPS bootstrap holds no key material in object state, and the TLS session drops the certificate key at handshake completion, so neither an idle image nor one with live established connections carries the key into the snapshot or swap files. This is a tested property, not a design intention: `scripts/https-smoke.sh` takes a console-driven statedump twice -- idle, and with an established TLS connection held open -- and scans it for the key as DER, PEM base64, and the raw private scalar. The state-file sensitivity table below is unchanged by TLS activation: those files hold the entire application state, just never the TLS private key.

**A reverse proxy remains an option, not the doctrine.** Where one host fronts several services with a single proxy, terminating TLS at the proxy and forwarding cleartext HTTP to `binary_port` on the loopback remains a valid deployment (the alternative block under Running under a supervisor above); behind a proxy the platform sees the proxy's address as the client, the usual forwarded-header tradeoff. The choice also has an assurance dimension: the native stack is a from-scratch TLS 1.3 implementation in interpreted LPC with no external audit (`docs/security-posture.md` Non-goals and known limits), so where that posture is insufficient the proxy path terminates TLS with a mainstream implementation.

**Exposure to abuse.** The platform ships no defense against a deliberate client: one peer can hold connection slots until the `users` cap silences every port (the console's included), and nothing in the image rate-limits, sheds, or evicts it (`docs/security-posture.md` Non-goals and known limits). Host-level controls own this surface: per-IP connection limits at the firewall (conntrack or equivalent), a fronting proxy's limits where one is deployed, and alerting on the `users` count as the early warning (Monitoring signals above). Plan the locked-out case before it happens: at the cap the driver refuses new connections only, so a console session already open keeps working -- hold one standing from the maintenance network, and its `reboot` verb still executes a controlled snapshot-and-exit from inside. Without one, the host is the fallback: SIGTERM writes the final incremental snapshot and exits (Running under a supervisor above), and the supervisor's restart clears the connection table.

**A worked per-IP limit.** The firewall rule above made literal, syntax-checked with `nft -c`, sized against the `users` cap's stock 255 slots (Connection-slot economics below):

```text
table inet eos_slots {
    chain input {
        type filter hook input priority filter; policy accept;

        tcp dport 8080 ct state new \
            meter eos_binary_port_per_ip { ip saddr ct count over 32 } \
            counter drop
    }
}
```

`ct count over 32` refuses a new connection from an address already holding 32 concurrent ones on `binary_port` -- roughly an eighth of the 255-slot table, chosen so no single peer can exhaust it alone even after every other client is idle, while staying well above what one legitimate multi-tab or multi-device client needs (Connection-slot economics below: a keep-alive or streaming subscriber is one slot each). Lower it for a deployment expecting few concurrent peers per legitimate client; a deployment behind a NAT-heavy client base (many real users sharing one egress address) needs a higher ceiling or an exemption list, since the rule cannot distinguish one abusive peer from many legitimate ones behind the same address. Add the equivalent two lines to the nginx alternative above when that is the fronting shape:

```nginx
limit_conn_zone $binary_remote_addr zone=eos_perip:10m;
# inside the server or location block:
limit_conn eos_perip 32;
```

`limit_conn_zone` is unvalidated against a running nginx on this machine (no local nginx binary); the directive names and zone/limit shape follow nginx's documented `ngx_http_limit_conn_module` syntax, and the `32` mirrors the nftables rule's sizing above so both paths agree on the same ceiling.

### Connection-slot economics

The `users` cap counts **concurrent connections, not registered users**, and translating its stock 255 slots into a servable population is arithmetic a public-facing deployment should run before it ships. What holds a slot, and for how long:

- an **in-flight request** holds a slot for its service time -- sub-millisecond for a light route, bounded above by the tick budget (a full default budget is roughly 90-120 ms of driver wall on the measured rig; `docs/configuration.md` Concurrency, measured twice);
- a **keep-alive connection** holds its slot between requests until the client closes or the flow layer's inactivity timeout reaps it (60 seconds by default; the server class's `inactivityTimeout()` override -- `docs/http-applications.md` API signatures);
- a **streaming subscriber** holds a slot for the lifetime of the stream, by design (`docs/composite-applications.md` The event streams): a dashboard tab with an open SSE subscription is one slot for as long as the tab lives;
- a **stranded connection** -- a server that never releases via `doneRequest()` -- holds its slot for the full inactivity window; under load that outpaces the reclaim rate the cap exhausts and every port goes silent (Common failure modes below).

So the honest sizing unit is concurrent streams. Thousands of occasional-request users fit when connections are brief or idle ones are reaped; a few hundred always-on streaming subscribers saturate the table regardless of how few humans they represent.

The fronting shape changes the arithmetic. **Direct exposure** spends a platform slot per client connection, keep-alive idle time included. **A pooling reverse proxy** holds the client connections itself and occupies platform slots only for in-flight upstream requests (plus whatever idle upstream keep-alives its pool retains), so many mostly-idle clients ride over few slots, and proxy buffering absorbs slow clients that would otherwise pin a slot for their transfer time -- at the price of the documented forwarded-address tradeoff (the reverse-proxy block above). What no proxy collapses: **live streams**. Each SSE subscriber needs its own end-to-end connection through the proxy, so streaming fan-out is bounded by the slot table whichever shape fronts it.

The ceiling itself is compiled into the driver: the config parser refuses `users` past 255 at any boot (Booting above; Common failure modes carries the at-the-cap row), and a wider build is a driver-level task that is unproven today, with the snapshot-compatibility question unmeasured (`docs/building.md` Wider index types). Plan the fronting shape and the workload's slot profile, not a bigger ceiling.

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

**Sizing the state volume.** Three consumers share it: the dump rotation holds two full images at once (`dump_file` plus `<dump_file>.old`, about twice the in-memory image -- and the image grows with the workload); the swap file grows toward `swap_size` × `sector_size` (about 64 MiB at the demo config, about 1 GiB at the `docs/configuration.md` production-shape starting point); and the scheduled backup's copy step stages a third image cut while it runs (Backing up and restoring state above). Provision the volume as a multiple of the expected image size with room for all three, and alert on host-disk occupancy beside the stalled-snapshot signal (Monitoring signals above): a full disk otherwise first surfaces as `dump_state` erroring out (Common failure modes below).

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
| Platform dies with a fatal `out of sectors` error | The in-memory image outgrew the swap device: `swap_size` (capped at 65535 sectors in the stock build) times `sector_size` (`docs/configuration.md` Limits and capacity) | Raise `sector_size` in the `.dgd` config and reboot from the latest snapshot. Alert on the `status` swap-sector percentage before it gets here (Monitoring signals above) |
| New connections open but nothing answers; existing service goes quiet | The `users` table is full: genuine concurrent load at the cap, or a leak outpacing reclaim -- a server that never releases completed requests holds each slot until the flow layer's 60-second inactivity timeout | At the cap both count surfaces (the `status` verb, the health route's `users=` line) need a connection the driver will no longer grant, so confirm from a console session already open -- or from outside: TCP connects that nothing ever answers, on every port at once, are this row. Recovery: a standing session's `reboot`, or SIGTERM from the host (the graceful stop writes the snapshot) and a supervisor restart; either clears the table but not the cause. Then find the leak (each HTTP example releases via `doneRequest()` when its response completes) or the abuser (Exposure to abuse above) |
| `system.log` grows without bound, repeating the same block | Error-manager feedback storm: an error caught inside an `atomic` function re-entering the error manager and looping a file write | Read the repeating block's trace for the atomic call site. `logd`'s deferred writes exist to prevent the class (Logging and diagnostics above); `scripts/base-boot-guard.sh` is its regression guard |
| Application kfun call returns "unknown function" or "extension not loaded" | A required `modules` entry is missing | Check `.dgd modules` mapping. Load the missing extension or remove the application code that depends on it |
| Per-owner ticks exhausted | Owner code is consuming ticks faster than `rsrc` allows | Use admin_console `quota` to inspect. Either raise the owner's quota or fix the looping code |
| `shutdown(1)` raises "Hotbooting is disabled" | The `.dgd` config has no `hotboot` tuple | Add the tuple to the `.dgd` config (see `docs/configuration.md` The .dgd configuration file) and reboot, or use cold reboot via `shutdown()` instead |
| Hot boot fails after `execv` (platform exits without restore) | `hotboot` tuple's paths point at a different binary or config than the running one, or `execv` fails | Check the tuple's paths against the running process. Fall back to cold reboot via `shutdown()` |

## Where to next

- **[`docs/configuration.md`](configuration.md)** covers the `.dgd` configuration fields, resource limits, and capacity ceilings referenced throughout this document.
- **[`docs/architecture.md`](architecture.md)** covers the platform tier model, daemons, and boot sequence in detail.
- **[`docs/runtime-primitives.md`](runtime-primitives.md)** covers the platform's eight runtime primitives, including the atomicity (§1) and hot-reload (§4) guarantees referenced above.
- **[`docs/application-authoring.md`](application-authoring.md)** covers writing a tier-E application on top of this platform.
- **DGD upstream reference** at <https://github.com/dworkin/dgd>: full kfun reference, `.dgd` field reference, host-binary build instructions.

[dworkin/lpc-ext]: https://github.com/dworkin/lpc-ext
