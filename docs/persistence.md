# Persistence

The platform's signature property: in-memory state is the primary state of the system. Objects, their variables, their dataspaces, their pending `call_out`s, their access bits, their resource counters — every piece of runtime state — survive process exit and return on the next boot. Applications do not write save/load code for the platform-tracked state; the runtime does the work.

This document covers what persists, how the platform captures and restores it, how the operator drives that cycle, and where the platform's persistence model differs from common alternatives (databases, ORMs, file-backed serialization).

For the language-level interaction between LPC's `static` modifier and persistence, see `docs/lpc-essentials.md` Type modifiers. For operator commands that drive the persistence cycle, see `docs/admin-console.md` Snapshot, restore, and shutdown. For the per-primitive runtime guarantee, see `docs/runtime-primitives.md` §3.

**Audience**: a developer, architect, or operator reasoning about what survives a restart, what does not, and how the platform captures and restores state; assumes `docs/architecture.md` for the structural model and basic familiarity with the platform's boot sequence.

## Orthogonal persistence as architectural property

**Orthogonal persistence** is the architectural property that an object's lifetime is decoupled from the lifetime of the program that created it. The same code that operates on a transient value operates on a persistent value; the persistence machinery is the runtime's concern, not the application's. Atkinson and Morrison's "Orthogonally Persistent Object Systems" (VLDB Journal 4, 1995) is the canonical academic statement of the property; KeyKOS and EROS extended the model with capability-based access control.

DGD has implemented orthogonal persistence since 1993. Christopher Allen's [2000 MUD-Dev description][allen-dgd-2000] names the property concisely: "DGD maintains persistence as a characteristic of its runtime environment ... full system state dump files implement persistence across reboots as well as snapshot-style state backups."

The practical consequence for application authors: **no `save_to_db` call, no `serialize` method, no `restore_from_storage` plumbing**. An object created in the runtime persists by default; modifying its variables modifies the in-memory image; statedump captures the image; restore reconstructs it. The platform's storage manager handles the bookkeeping.

The property is foundational for the platform's other primitives. Atomic rollback (§1) presumes there's an in-memory state to roll back to. Hot reload (§4) presumes the state survives the code transition. State introspection (§8) presumes the state graph is queryable; orthogonal persistence makes "the state graph" a coherent runtime concept rather than a database/cache/file-tree archipelago.

## What persists

The platform's statedump captures the entire in-memory image:

| State | Captured? | Notes |
|---|---|---|
| **Object variables** (non-static and static both) | Yes | The image is the image; statedump does not respect `static` |
| **Clone dataspaces** | Yes | Each clone's per-instance variables |
| **Object identity** | Yes | Master paths, clone indices, owner attributions |
| **Inheritance graph** | Yes | The objectd-maintained registry |
| **Pending `call_out`s** | Yes | The deferred-call queue, with original argument values |
| **Access bits** | Yes | Per-user, per-directory access state |
| **Resource consumption** | Yes | Per-owner tick / object / call_out counters |
| **Admin credentials** | Yes | Persisted under the access daemon |
| **Open connections** | **No** | File descriptors are not in the snapshot |
| **Swap-file contents** | **No** | The swap is per-boot scratch; objects swapped to disk are paged into the snapshot at dump time |
| **Static variables, w.r.t. `save_object`** | **No** | `static` excludes from the per-object save format; full statedump is independent of the modifier |

The "what doesn't persist" entries above are the platform's contract boundary: the application is responsible for any state outside this set. The most common case is connections — clients reconnect after a restart. Hot boot is the platform's mechanism for preserving connections across a binary upgrade; see Hot boot below.

## The statedump cycle

`dump_state(int incr)` writes the in-memory image to the configured `dump_file`.

- `dump_state(0)` (or `dump_state()`) — **full snapshot**. Writes the entire image; the prior `dump_file` rotates to `<dump_file>.old`.
- `dump_state(1)` — **incremental snapshot**. Writes changes since the prior full snapshot. The platform's recovery sequence restores the most-recent full snapshot, then applies the most-recent incremental on top.

Two trigger mechanisms:

1. **Automatic** — the `.dgd` configuration's `dump_interval` field (typical: 3600 seconds). The platform writes a snapshot every interval. The rotation moves `<dump_file>` → `<dump_file>.old` before the new snapshot is written, so if the new write is interrupted, the prior snapshot survives.
2. **Explicit** — an operator invokes the `snapshot` verb in `admin_console`, or code invokes `dump_state()` directly. Useful before maintenance windows, before risky deploys, or when an externally-triggered consistent backup is needed.

`dump_state` is **atomic with respect to in-flight operations**: the snapshot represents a consistent commit boundary. The runtime guarantees that the snapshot does not capture a state in the middle of an atomic context — statedumps occur between timeslices, never inside an atomic operation.

The cost of a snapshot is proportional to the in-memory image size. A multi-gigabyte image can take seconds to write; the runtime briefly blocks during the dump. Workloads that cannot tolerate a brief pause should plan `dump_interval` against their peak-traffic schedule.

## Snapshot restore: cold boot from snapshot

When the host driver starts and finds a valid `dump_file`, it restores from the snapshot instead of running the cold-boot initd cascade:

1. The driver reads the snapshot file.
2. The in-memory image is reconstituted from the snapshot data.
3. The driver invokes the registered `restored(int hotboot)` hook in the kernel driver object (`src/kernel/sys/driver.c`) with `hotboot = 0`.
4. `restored` re-attaches managers and re-arms any external resources the snapshot did not preserve.
5. The driver begins accepting connections on the configured ports.

No `initd::create()` runs. The application's bootstrap was captured in the snapshot; restore returns the platform to that state. Initd cascades only run on cold-boot-from-scratch.

If the snapshot is corrupt or `dump_file` is absent, the driver falls back to cold boot from scratch — the initd cascade runs, the platform boots without prior state, and connections start fresh.

The `<dump_file>.old` file is the rollback target if the most recent snapshot is corrupt or undesirable: remove the current `dump_file`, the driver finds `<dump_file>.old` (or the operator renames it), and restore proceeds from the prior snapshot.

## Hot boot: snapshot + execv + fd inheritance

Hot boot is the platform's mechanism for upgrading the host binary or `.dgd` configuration without losing connections or pending `call_out`s.

Prerequisites: the `.dgd` configuration carries a `hotboot` tuple naming the new binary, the new config, and the snapshot paths:

```text
hotboot = ({ "/path/to/new/dgd", "/path/to/new/config.dgd", "/path/to/dump_file", "/path/to/dump_file.old" });
```

Trigger: `shutdown(1)` from the kernel driver (operator-facing: a hot-boot script invoking `code "/usr/System/..."->hotboot()` or equivalent).

Sequence:

1. The runtime serializes per-connection state.
2. `dump_state(1)` writes an incremental snapshot.
3. `execv()` replaces the running executable with the binary named in the `hotboot` tuple. The new process inherits the open file descriptors of the old process (POSIX `execv` semantics).
4. The new binary restores from the snapshot.
5. The driver invokes `restored(int hotboot)` with `hotboot = 1`.
6. The connection-handling code re-attaches to the inherited file descriptors; clients see no disconnect.

The `restored(int hotboot)` hook distinguishes hotboot-resume (`hotboot = 1`) from statedump-resume (`hotboot = 0`) so the platform can re-attach to surviving connections in one case and skip the re-attach in the other.

Failure modes:

- `hotboot` tuple absent — `shutdown(1)` raises "Hotbooting is disabled".
- New binary or config path invalid — `execv` fails; platform exits without restore. Operator falls back to cold restart.
- Snapshot incompatible with the new binary (different `auto_object`, different `driver_object`, missing modules) — restore fails; platform exits.

## save_object / restore_object: explicit per-object save

The platform-wide statedump is the **default** mechanism for persistence; application code does not call it. For applications that need explicit per-object save points independent of the full image (logging an event to disk; checkpointing a derived value), the host provides `save_object(filename)` and `restore_object(filename)`.

- `save_object(filename)` — writes the calling object's non-static variables to the named file. Format is host-defined (a structured text format roughly like LPC literal syntax).
- `restore_object(filename)` — reads the file and assigns to the calling object's matching variables.

The `static` modifier on a variable excludes it from `save_object`'s output. Static globals are runtime-only state with respect to the per-object save format; the full-image statedump captures them regardless (the image is the image; the modifier affects only the save-object format).

Use `save_object`/`restore_object` when:

- An object needs a checkpoint independent of the platform's snapshot cadence (e.g., a critical operation just completed and needs durable confirmation before the next `dump_interval`).
- State needs to be portable across platforms (the per-object save file is human-readable and can be hand-edited or imported into a different platform).
- A subset of an object's state needs a stable on-disk representation distinct from the snapshot.

Do **not** use `save_object`/`restore_object` as a general persistence mechanism — that role is the platform's, and reinventing it on top is reproducing the runtime's work in application code (`docs/runtime-primitives.md` §3 names this as the platform's architectural commitment).

## Variable persistence semantics

The interaction between LPC's `static` modifier and the platform's persistence mechanisms:

| Variable kind | In statedump? | In save_object? |
|---|---|---|
| Non-static global | Yes | Yes |
| `static` global | Yes | No |
| Local variables | Not applicable (not part of object state) | Not applicable |

The platform-wide statedump captures the in-memory image without consulting modifiers. The `static` modifier exists specifically to interact with the per-object `save_object` format: marking a variable `static` says "do not write this to the per-object save file."

The application-level implication: a `static` variable is for state that is part of the object's in-memory identity (so it survives full statedump) but is not part of the object's portable per-object save (so it can hold derived values, caches, or runtime-only state without polluting the save file). A non-static variable is the default — captured in both places.

See `docs/lpc-essentials.md` Type modifiers for the language reference; the 2003 DGD-list discussion of the `static` keyword by Felix Croes covers the runtime semantics in detail.

## Operator workflow

The persistence cycle from the operator's view:

| Action | Verb | Effect |
|---|---|---|
| Page in-memory objects to swap | `swapout` | Calls `swapout()`; reduces resident set; next access faults objects back in |
| Capture a snapshot | `snapshot` | Calls `dump_state(0)`; full image to disk; rotates prior to `.old` |
| Stop cleanly | `reboot` | Calls `dump_state(1)` + `shutdown()`; next boot restores from the new snapshot |
| Stop without snapshot | `shutdown` | Calls `shutdown()`; next boot restores from the previous snapshot (or cold-boots if absent) |
| Recover from corrupt snapshot | (Host-level) | Move `dump_file` aside; rename `<dump_file>.old` to `dump_file`; restart |
| Recover from a wedged platform | `reboot` | Forces a snapshot at the last consistent state; next boot starts there |

Common operator scenarios:

- **Pre-deployment safety net**: `snapshot` before applying a risky code change. If the change wedges the platform, the operator falls back to `<dump_file>.old`.
- **Scheduled rotation**: the platform writes automatic snapshots at `dump_interval`; `snapshot` is the operator-on-demand variant.
- **Hot binary upgrade**: requires the `.dgd` `hotboot` tuple; preserves connections and pending state across an `execv` replace.
- **Recovery from a wedge**: `reboot` returns the platform to the last consistent state without operator coordination across the host filesystem.

## Persistence boundaries

The platform's persistence model has explicit boundaries. Each requires application-layer handling:

- **Connections**: not in the snapshot. Statedump-restore does not preserve open client connections; clients must reconnect. Hot boot preserves connections via `execv` fd inheritance.
- **External resources**: any state outside the platform's in-memory image is the application's responsibility. File-system state outside the platform's chrooted `directory`, external API tokens, in-flight network requests — none persist across restart unless the application captures them in the platform's state.
- **Time**: the platform restores to the snapshot's logical time, but the host clock advances independently. `call_out` deferrals scheduled before the snapshot fire at the original target time relative to the snapshot's clock, which means a snapshot restored hours after capture has accumulated overdue `call_out`s ready to fire. Applications relying on `call_out` for time-sensitive work should handle the case where a deferred call fires later than originally scheduled:
  - **Detect lateness by carrying the deadline.** A `call_out`'s arguments are captured in the snapshot with their original values, so the cheapest detection is to schedule with the intended fire time as an argument — `call_out("expire", delay, time() + delay)` — and compare against `time()` when the function fires. A fire time well past the carried deadline means the deferral crossed a restore (or a long pause).
  - **Decide per work type.** Idempotent maintenance (flush, sweep, re-index) can simply run late. Expiry-shaped work (timeouts, token lifetimes) should evaluate against the carried deadline, not assume "now is approximately when I asked to run" — an expiry that fires late should still expire against the original deadline. Interaction-shaped work (a reminder, a turn timer) may need to be dropped or re-derived when it fires meaningfully late, since the condition it was scheduled for may no longer hold.
  - **Re-anchor recurring schedules.** A self-re-arming `call_out` chain that carries its own cadence should compute the next delay from the current `time()` after a late fire rather than scheduling at the stale cadence offset, or the whole chain stays phase-shifted by the restore gap.
- **Snapshot integrity**: the platform writes snapshots atomically (rotation + write), but a host-level failure during the write (disk full, power loss) can produce a corrupt `dump_file`. The `<dump_file>.old` rotation is the platform's recovery mechanism. Backup snapshots to off-host storage for disaster recovery.

For operator-level recovery procedures when any of these boundaries are hit, see `docs/operations.md` Common failure modes (table of symptoms and diagnoses) and `docs/admin-console.md` for the `snapshot`, `reboot`, and `shutdown` verbs that manage the persistence cycle.

## Substrate verification

The platform's persistence contract is exercised by the bundled examples. The richer-state composition that lands with the property-change dispatcher (`docs/dispatcher.md` Persistence; `docs/runtime-primitives.md` §3 Extensions) is verified end-to-end by `examples/merry-app/sys/test.c` phases 16 and 17:

- Phase 16 sets up a parent / child pair with a property-bound Merry-script observer on the child, stashes the child as an LPC global on the test driver, schedules a `call_out` for the verification phase, and triggers a snapshot via `/usr/System/sys/persist_helper->trigger_dump_and_exit()` — the helper calls `dump_state(0)` followed by `shutdown()`.
- An external restart against the snapshot (`dgd mva.dgd state/snapshot`) restores the image; the pre-snapshot `call_out` fires after restore.
- Phase 17 reads the saved LPC global, writes the observed property, and asserts that the value landed and that the observer's compiled source ran against the resurrected host.

Five orthogonal-persistence guarantees compose in the same verification: LPC global variables, property storage on host objects, references to compiled Merry-script clones at `/usr/Merry/merry/<md5>`, the observer-source contract (`$this` binding to the dispatch host, `Set` re-entry), and the scheduled call_out queue. Any future regression in those guarantees surfaces here as a `PERSIST VERIFY FAIL:` sentinel on the second-boot run.

`/usr/System/sys/persist_helper` is reusable: any future kernel-layer subsystem (Vault, schema, marshal) needing the same snapshot+restore verification harness calls `trigger_dump_and_exit()` from a test driver and follows the same two-boot pattern.

## Persistence under host-driver extensions

Loading a host-driver extension (`docs/operations.md` Loading host-driver extensions; `docs/architecture.md` Host-driver extensions) binds the platform's snapshot to that extension's presence: a snapshot taken with the extension active requires the same extension to restore. This is documented in [Felix Croes' 2010 Hydra mailing-list note][croes-hydra-2010] and is a durable architectural commitment, not an opt-in convenience.

Operational implication: removing an extension and restoring an old snapshot loses state. Plan extension choices accordingly. Treat extension addition as a one-way schema migration of the platform's persistence format.

For the question of how extension-loaded codepaths interact with the platform's atomic-rollback path (and therefore with the atomicity-of-state property that persistence relies on), see `docs/runtime-primitives.md` §1 Open and `docs/operations.md` Open empirical questions.

[croes-hydra-2010]: https://mail.dworkin.nl/pipermail/dgd/2010-August/006717.html

## Where to next

- **`docs/runtime-primitives.md`** §3 — the per-primitive foundation-and-status statement for persistent state.
- **`docs/admin-console.md`** Snapshot, restore, and shutdown — operator verbs for driving the persistence cycle interactively.
- **`docs/operations.md`** — `.dgd` configuration fields (`dump_file`, `dump_interval`, `swap_file`, `hotboot` tuple), boot modes, failure-mode table.
- **`docs/lpc-essentials.md`** Type modifiers — language semantics for `static` and how it interacts with persistence.
- **`docs/architecture.md`** Boot sequence — the three boot modes (cold, statedump-restore, hot boot) at the platform level.
- **`docs/vault-applications.md`** — Vault subsystem for per-domain typed-property persistence on top of the snapshot cycle: participating-domain contract, on-disk XML shape, round-trip cycle.
- **`src/kernel/sys/driver.c`** — the `restored(int hotboot)` hook implementation.

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html
