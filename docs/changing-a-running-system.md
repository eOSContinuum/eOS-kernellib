# Changing a Running System

The platform takes every kind of change — a one-line bug fix, a library upgrade with dependents, a data-shape migration, new behavior added by an end user's script, even a new host binary — without a restart and without a deploy step. The mechanisms live in separate references (`docs/code-lifecycle.md`, `docs/dispatcher.md`, `docs/persistence.md`, `docs/operations.md`); this document is the consolidated story: the ladder of change, ordered by blast radius, and the safety net under all of it.

**Audience**: an operator or application author planning how a change will enter a running deployment; wants the whole change story in one place before drilling into the mechanism references; assumes `docs/architecture.md` for the structural model.

## The ladder of change

Each rung names a change shape, the mechanism that takes it, and where the full reference lives.

### 1. Hot-fix one object

Edit the source, `compile <path>` from the operator console (or `compile_object` from code). The path's master is replaced in place: calls already executing finish on the old program; the next call dispatches to the new one. No restart, no disconnect, no deploy pipeline.

Compilation itself runs in atomic context — a syntax error or failed type-check leaves the prior master untouched. The failed compile is a no-op, not an outage.

Reference: `docs/code-lifecycle.md` Compile and Hot reload; `docs/admin-console.md` Hot-fixing code in production; the working demonstration at `examples/hot-reload-demo/`.

### 2. Upgrade a library and its dependents

Recompiling a library does not automatically rebuild the objects that inherit it — they keep running against the old parent until rebuilt. The `upgrade [-a|-p] <file>` verb on the operator login drives the platform's cascade: the object manager's inheritance graph identifies every direct and transitive dependent, and the upgrade daemon recompiles them — with `-a`, as one all-or-nothing atomic operation, so a compile error anywhere leaves the entire dependency tree on the old version.

Reference: `docs/code-lifecycle.md` Library upgrade.

### 3. Migrate live state without a maintenance window

When an upgrade changes an object's data shape, existing clones carry the old shape. The `-p` flag on `upgrade` queues `call_touch` patching: each live clone is marked, and the patch function runs the next time the clone is naturally referenced. State migrates lazily, invisibly, with no downtime and no global sweep-and-stop.

Reference: `docs/code-lifecycle.md` `call_touch` and `_F_touch`; `docs/application-authoring.md` Live code upgrade through call_touch.

### 4. Change data and let the system react

Not every change is code. Writing a typed property is itself a change-entry point: the property-change dispatcher fires registered observers — before the write (which can refuse it), at commit, and after — within the same atomic envelope as the write. Reactive behavior changes by changing the data that drives it, with no code touched.

Reference: `docs/dispatcher.md`; `docs/signal-applications.md` for the smallest working example.

### 5. Add behavior under capability bounds

New behavior can enter the running system from sources the platform does not fully trust. Merry scripts — stored as property values, compiled through the sandbox, denied the dangerous kernel functions at the language layer — let applications and their users add reactive logic at runtime without widening the capability surface. The script cannot exceed the bounds set at load time, and an error inside it rolls back the dataspace mutations of its atomic envelope.

Reference: `docs/merry-applications.md`, `docs/merry-language.md`; `docs/runtime-primitives.md` §5.

### 6. Replace the host binary

The largest change — a new build of the host runtime itself — also takes effect without losing the system. Hotboot (the `.dgd` configuration's `hotboot` tuple) re-executes the new binary against a freshly written snapshot; connections survive, and state restores from the snapshot.

Reference: `docs/operations.md` boot modes; `docs/admin-console.md` Snapshot, restore, and shutdown.

## The safety net

Two properties hold under every rung above:

- **Atomicity bounds the failure.** A failed compile, a failed atomic upgrade, an erroring observer, a misbehaving script — each rolls back its own envelope's mutations. Partial effects do not escape; the change either lands or it never happened (`docs/runtime-primitives.md` §1).
- **The image is the recovery point.** Statedump snapshots capture the entire object graph; a deployment that needs a coarse undo restores the prior snapshot and replays from there (`docs/persistence.md`).

## What never happens

- **No deploy step.** Source compiles into the live runtime; there is no build-package-ship-restart pipeline.
- **No restart to pick up code.** Every rung above applies to the running image. (Runtime *configuration* is the exception today — the `.dgd` configuration is read at boot; a runtime-configurable values store is a committed roadmap surface, `docs/runtime-platform-roadmap.md` Wave 3.)
- **No connection loss on upgrade.** Object and library upgrades happen under live connections; even the host-binary swap preserves them via hotboot.

## Where to next

- `docs/code-lifecycle.md` — the mechanism reference for rungs 1-3.
- `docs/dispatcher.md` — the mechanism reference for rung 4.
- `docs/merry-applications.md` — the mechanism reference for rung 5.
- `docs/operations.md` — deployment configuration, including hotboot.
- `docs/runtime-platform-roadmap.md` — committed forward surfaces, including the demonstration and registry gaps still open around the upgrade cascade.
