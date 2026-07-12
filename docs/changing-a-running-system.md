# Changing a running system

The platform takes every kind of change: a one-line bug fix, a library upgrade with dependents, a data-shape migration, new behavior added by an end user's script, even a new host binary. It does all of this without a restart and without a deploy step. The mechanisms live in separate references (`docs/code-lifecycle.md`, `docs/dispatcher.md`, `docs/persistence.md`, `docs/operations.md`). This document is the consolidated story: the ladder of change, ordered by blast radius, and the safety net under all of it.

**Audience**: an operator or application author planning how a change will enter a running deployment, wanting the whole change story in one place before drilling into the mechanism references, and assuming `docs/architecture.md` for the structural model.

## The ladder of change

Each rung names a change shape, the mechanism that takes it, and where the full reference lives.

### 1. Hot-fix one object

Edit the source, `compile <path>` from the operator console (or `compile_object` from code). The path's master is replaced in place: calls already executing finish on the old program. The next call dispatches to the new one. No restart, no disconnect, no deploy pipeline.

Compilation itself runs in atomic context: a syntax error or failed type-check leaves the prior master untouched. The failed compile is a no-op, not an outage.

The change is durable only if it reaches the on-disk source. Editing the file (with `ed` or through the filesystem) and then `compile <path>` writes the fix to the source tree, so a cold boot rebuilds it from source. Compiling from an inline source string instead (`compile_object(path, source)` from `code`) replaces the master in memory without touching the file, so the source tree still holds the old code and a cold boot comes up with the old behavior. Write a lasting fix to the source tree, not just into the running image.

Reference: `docs/code-lifecycle.md` Compile and Hot reload, `docs/admin-console.md` Hot-fixing code in production, and the working demonstration at `examples/hot-reload-demo/`.

### 2. Upgrade a library and its dependents

Recompiling a library does not automatically rebuild the objects that inherit it. They keep running against the old parent until rebuilt. The `upgrade [-a|-p] <file>` verb on the operator login drives the platform's cascade: the object manager's inheritance graph identifies every direct and transitive dependent, and the upgrade daemon recompiles them (with `-a`, as one all-or-nothing atomic operation), so a compile error anywhere leaves the entire dependency tree on the old version.

Reference: `docs/code-lifecycle.md` Library upgrade and the working demonstration at `examples/upgrade-cascade/`.

### 3. Migrate live state without a maintenance window

When an upgrade changes an object's data shape, existing clones carry the old shape. The `-p` flag on `upgrade` queues `call_touch` patching: the upgrade daemon sweeps the object table to mark every live clone, then forces each one through the patch function immediately afterward via chained callouts, rather than waiting for the clone's next natural reference. Migration completes with no downtime, though it is an active sweep, not a lazy background process.

Reference: `docs/code-lifecycle.md` `call_touch` and `_F_touch`, `docs/application-authoring.md` Live code upgrade through call_touch, and the working demonstration at `examples/upgrade-cascade/`.

### 4. Change data and let the system react

Not every change is code. Writing a typed property is itself a change-entry point: the property-change dispatcher fires registered observers before the write (which can refuse it), during it, and after, all within the same atomic envelope as the write. Reactive behavior changes by changing the data that drives it, with no code touched.

Reference: `docs/dispatcher.md` and `docs/signal-applications.md` for the smallest working example.

### 5. Add behavior under capability bounds

New behavior can enter the running system from sources the platform does not fully trust. Merry scripts (stored as property values, compiled through the sandbox, denied the dangerous kernel functions at the language layer) let applications and their users add reactive logic at runtime without widening the capability surface. The script cannot exceed the bounds set at load time, and an error inside it rolls back the dataspace mutations of its atomic envelope.

Reference: `docs/merry-applications.md`, `docs/merry-language.md`, and `docs/runtime-primitives.md` §5.

### 6. Replace the host binary

The largest change (a new build of the host runtime itself) also takes effect without losing the system. Hotboot (the `.dgd` configuration's `hotboot` tuple) re-executes the new binary against a freshly written snapshot. Connections survive, and state restores from the snapshot.

Reference: `docs/operations.md` boot modes and `docs/admin-console.md` Snapshot, restore, and shutdown.

## Shipping a release

A multi-file application release composes the ladder's rungs into one operator sequence. Nothing here is new mechanism -- it is the order that keeps every step recoverable.

1. **Snapshot first.** `snapshot` writes the restore point (`docs/operations.md` Backing up and restoring state). If anything below goes wrong, the recovery is that snapshot, not a reverse migration.
2. **Sync the source tree.** Bring the domain's source to the release state (a git checkout or copy on the host). Nothing changes in the image yet: masters rebuild only when compiled.
3. **Upgrade through the cascade.** For each changed library, `upgrade -a <file.c>` from the operator login (rung 2): the daemon recompiles every direct and transitive dependent, and `-a` makes the tree all-or-nothing, so a compile error anywhere leaves the whole release un-applied. Add `-p` when the release changes a clone's data shape (rung 3); the patch sweep runs eagerly after the recompile. Standalone masters with no shared library are a plain `compile <path>` each (rung 1).
4. **Verify.** `issues <file.c>` per changed source reads back whether the cascade converged (more than one issue means older program versions are still bound); the application's own sentinel driver or a `code` probe exercises the changed behavior; `log 40` reads the error log for anything the release provoked.

**Detecting image-versus-source drift.** No shipped tooling compares the compiled image against the on-disk tree, in either direction -- there is no manifest, no checksum walk, and no report of masters whose source changed since compile. The primitives to hand-roll a per-object check exist (an object's compile time via `status(obj)[O_COMPILETIME]`, the source's modification time via `file_info`), but nothing walks the tree for you. What holds drift down is discipline plus one structural fact: compile from files rather than the two-argument inline-source form (rung 1 names that escape hatch as exactly the drift hole), and drift does not survive a cold boot -- though not symmetrically. A cold boot starts from an empty object table and compiles what the initd cascade reaches from the tree: a stale on-disk master is rebuilt from the current file, while an inline-only master with no file behind it simply does not come back (it vanishes if nothing references its path again, and a later `compile_object` against the fileless path errors).

## Changing the kernel layer

The ladder above is written for application-tier change. Kernel- and System-tier sources take the same mechanisms, with one matrix worth stating once (each row below was exercised against a live boot):

| What changes | Live viability | The mechanism and the catch |
|---|---|---|
| A System daemon (`errord`, `logd`, `userd`) | Yes | `compile <path>` from the admin console. Master variables survive; `create()` does not re-run, so driver registrations held by reference persist. This holds for capabilityd too: its grant tables ride the recompile unchanged (`docs/capability.md`); only a cold boot re-seeds the bootstrap table. |
| `objectd` / `upgraded` | Yes, between cascades | Same as any daemon, but not while an upgrade or patch sweep is in flight: the upgrade daemon tracks in-flight cascade state in its master variables (it refuses new upgrades with "Still patching" while a sweep runs), and the machinery does not patch itself. |
| The System auto (`/usr/System/lib/auto.c`) | Yes, with breadth | A direct `compile` replaces the master but leaves every inheritor on the old issue (`issues` reads the divergence). The full cascade is `upgrade /usr/System/lib/auto.c` from an operator login -- and because everything user-tier inherits the System auto, the cascade wants write access to every dependent source: an operator with partial grants is refused per file (`<file>: Access denied`), while a full-access operator drives it end to end. This is also the path that picks up a changed `~System/data/EXT` (`docs/where-code-belongs.md`). |
| The kernel auto (`/kernel/lib/auto.c`) | Recompiles live; cascade uncharted | The file-based `compile` succeeds from the admin console and the image stays sound, but kernel-tier objects are excluded from `call_touch` patching in the upgrade daemon, and no shipped procedure cascades kernel-tier dependents. Treat a kernel-auto change as a cold-boot change until someone proves the live path. |
| The driver object (`/kernel/sys/driver.c`) | Yes | Recompiles live; it has no `create()` to re-run and its registrations survive by reference. |
| An include file (`.h`) | Via the cascade | objectd records include relationships and the upgrade daemon walks them, so upgrading the include's dependents recompiles transitive includers. A bare edit with no `upgrade` changes nothing until the includers recompile. |

The contributor's edit loop follows from the matrix: day-to-day kernel work is edit, `compile`, exercise from the console; a change to either auto or to widely-included headers is verified with a cold boot plus the regression sweep (`CONTRIBUTING.md`), because that is the only path that proves the whole tree rebuilds against the change.

## The safety net

Two properties hold under every rung above:

- **Atomicity bounds the failure.** A failed compile, a failed atomic upgrade, an erroring observer, a misbehaving script: each rolls back its own envelope's mutations. Partial effects do not escape. The change either lands or it never happened (`docs/runtime-primitives.md` §1).
- **The image is the recovery point.** Statedump snapshots capture the entire object graph. A deployment that needs a coarse undo restores the prior snapshot and replays from there (`docs/persistence.md`).

## What never happens

- **No deploy step.** Source compiles into the live runtime. There is no build-package-ship-restart pipeline.
- **No restart to pick up code.** Every rung above applies to the running image. (Runtime *configuration* is the exception today: the `.dgd` configuration is read at boot. A runtime-configurable values store is a committed roadmap surface, `docs/runtime-platform-roadmap.md` Wave 3.)
- **No connection loss on upgrade.** Object and library upgrades happen under live connections. Even the host-binary swap preserves them via hotboot.

## Where to next

- [`docs/code-lifecycle.md`](code-lifecycle.md): the mechanism reference for rungs 1-3.
- [`docs/dispatcher.md`](dispatcher.md): the mechanism reference for rung 4.
- [`docs/merry-applications.md`](merry-applications.md): the mechanism reference for rung 5.
- [`docs/operations.md`](operations.md): deployment configuration, including hotboot.
- [`docs/runtime-platform-roadmap.md`](runtime-platform-roadmap.md): committed forward surfaces, including the demonstration and registry gaps still open around the upgrade cascade.
