# Persistence

The platform's signature property: in-memory state is the primary state of the system. Objects, their variables, their dataspaces, their pending `call_out`s, their access bits, their resource counters (every piece of runtime state) survive process exit and return on the next boot. Applications do not write save/load code for the platform-tracked state. The runtime does the work.

This document covers why the architecture exists, what persists, how the platform captures and restores it, how the operator drives that cycle, and where the platform's persistence model differs from common alternatives (databases, ORMs, file-backed serialization, untyped process checkpointing).

For the language-level interaction between LPC's `static` modifier and persistence, see `docs/lpc-essentials.md` Type modifiers. For operator commands that drive the persistence cycle, see `docs/admin-console.md` Snapshot, restore, and shutdown. For the per-primitive runtime guarantee, see `docs/runtime-primitives.md` §3.

**Audience**: a developer, architect, or operator reasoning about what survives a restart, what does not, and how the platform captures and restores state. This document assumes `docs/architecture.md` for the structural model, and basic familiarity with the platform's boot sequence.

## Why orthogonal persistence

Start from the pain. An application with long-lived state on a conventional stack maintains the same data in two representations: the in-memory objects the code computes with, and the storage representation (rows, documents, serialized files) the data survives in, plus the mapping code that translates between them. Every feature that touches persistent state pays the translation toll twice, once per direction, and the mapping layer itself becomes load-bearing code with its own bugs, its own schema-evolution story, and its own failure modes. Atkinson and Morrison open their survey of the field with the cost: studies they cite estimate roughly thirty percent of a typical database application's code is this mapping and transfer plumbing (King 1978, via [Atkinson & Morrison 1995][am-1995], p. 333). This is code that contributes nothing to the application's actual behavior.

The deeper observation is about **lifetimes**. Data in a running system spans a spectrum of lifetimes: from intermediate expression results that live microseconds, through session state, to records that outlive the program and the machine. The conventional stack forces a representation change at an arbitrary point on that spectrum: below the line, language values. Above it, storage formats. Atkinson and Morrison's framing deliberately says *transient versus long-lived*, not *in-memory versus on-disk*. Where data lives is an implementation choice, and binding the representation to the storage medium is exactly the mistake the architecture removes.

Orthogonal persistence removes the line. Atkinson and Morrison state three principles (their §2.2.2):

- **Persistence independence**: the form of a program does not depend on the longevity of the data it manipulates. The same code operates on transient and long-lived values.
- **Data type orthogonality**: every type may have persistent and transient instances. Persistence is not a property of special "storable" types.
- **Persistence identification**: which objects persist is determined by the system (here: reachability in the image), not declared through the type system or special calls.

What falls away for the application author when the runtime honors all three: the storage representation (there is only the image), the mapping layer (nothing to map), explicit save and load calls (the snapshot cycle is the runtime's), cache invalidation between representations (there is one representation), and the class of bugs where the two representations disagree. The rest of this document describes the machinery that pays for this, and its boundaries, which are real.

## Orthogonal persistence as architectural property

**Orthogonal persistence** is the architectural property that an object's lifetime is decoupled from the lifetime of the program that created it. The same code that operates on a transient value operates on a persistent value. The persistence machinery is the runtime's concern, not the application's. Atkinson and Morrison's "Orthogonally Persistent Object Systems" (VLDB Journal 4, 1995) is the canonical academic statement of the property. KeyKOS and EROS extended the model with capability-based access control.

DGD has implemented orthogonal persistence since 1993. Christopher Allen's [2000 MUD-Dev description][allen-dgd-2000] names the property concisely: "DGD maintains persistence as a characteristic of its runtime environment ... full system state dump files implement persistence across reboots as well as snapshot-style state backups."

The practical consequence for application authors: **no `save_to_db` call, no `serialize` method, no `restore_from_storage` plumbing**. An object created in the runtime persists by default. Modifying its variables modifies the in-memory image. Statedump captures the image. Restore reconstructs it. The platform's storage manager handles the bookkeeping.

The property is foundational for the platform's other primitives. Atomic rollback (§1) presumes there's an in-memory state to roll back to. Hot reload (§4) presumes the state survives the code transition. State introspection (§8) presumes the state graph is queryable. Orthogonal persistence makes "the state graph" a coherent runtime concept rather than a database/cache/file-tree archipelago.

## Compared with common alternatives

The intro promised the comparison. Here it is. Each alternative solves real problems. The comparison is about what each one makes the *application author* responsible for.

| | Database / ORM | File-backed serialization | Untyped checkpointing (CRIU-class) | Orthogonal persistence (this platform) |
|---|---|---|---|---|
| Representations the author maintains | Two (objects + schema) plus the mapping | Two (objects + format) plus save/load code | One | One |
| What persists | What the schema covers | What the author remembered to serialize | The whole process image, untyped | The whole image, typed |
| Save points | Per-transaction, explicit | Explicit calls | External snapshot | Runtime snapshot cycle (automatic + on-demand) |
| Restore | Reconnect + rehydrate | Explicit load + validation | Same machine/kernel shape, fragile | Type-aware restore from the versioned snapshot format |
| Schema / code evolution | Migration framework | Hand-written format versioning | None: the image is opaque | Hot reload + lazy upgrade (`call_touch`). See `docs/code-lifecycle.md` |
| Transactionality | In the database, separate from program state | None | None | Atomic functions over the same state the snapshot captures (§1) |

**Database / ORM**: the canonical two-representation architecture. The database brings real strengths (declarative queries, concurrent writers across machines), and for multi-machine workloads it is the right call. This platform is deliberately single-coherence-domain. The cost is the impedance mismatch: program logic lives in one type system, durable state in another, and transactions protect the storage side only. In-memory state that depends on a rolled-back write is the application's problem.

**File-backed serialization** (JSON/pickle/protobuf save files): cheap to start, and coverage decays from there: persistence is exactly as complete as the author's save code, save points are exactly as frequent as the author's calls, and format evolution is hand-maintained. The platform keeps `save_object` (below) for the narrow cases where an explicit, portable, per-object file is the point. It is not the persistence mechanism.

**Untyped process checkpointing** (CRIU-class snapshots of process memory): the closest cousin, and the instructive contrast. It achieves persistence independence (the program does not change), but the captured image is raw memory: no portable representation, no reliable type information, restore bound to the platform shape that wrote it. Atkinson and Morrison treat memory-image schemes of this kind as incomplete orthogonal persistence for exactly this reason (their discussion of untyped-store systems, p. 354). DGD's statedump clears the bar the untyped image fails. The snapshot is a **typed** image in a versioned format: the driver restores objects, not bytes, which is why a snapshot survives driver upgrades, why `call_touch` can migrate restored objects to recompiled programs, and why the restore path can re-key structures rather than hoping the memory layout still matches.

In Atkinson and Morrison's taxonomy of implementation architectures (their §4.1), DGD sits in the deepest tier: the persistent-world architecture, where the runtime owns the entire object world and persistence is a property of the world rather than a service objects call. Citation details in [references.md](references.md#atkinson-morrison-1995).

[am-1995]: references.md#atkinson-morrison-1995

## What persists

The platform's statedump captures the entire in-memory image:

| State | Captured? | Notes |
|---|---|---|
| **Object variables** (non-static and static both) | Yes | The image is the image; statedump does not respect `static` |
| **Clone dataspaces** | Yes | Each clone's per-instance variables |
| **Object identity** | Yes | Master paths, clone indices, owner attributions |
| **Inheritance graph** | Yes | The objectd-maintained registry |
| **Pending `call_out`s** | Yes | The deferred-call queue, with original argument values |
| **Access bits** | Yes | Per-user, per-directory access state. Grants made through the kernel console's `grant` / `ungrant` verbs are also flushed to `src/kernel/data/access.data`, independent of the snapshot cycle; grants made by other paths reach that file only at the next such flush |
| **Resource consumption** | Yes | Per-owner tick / object / call_out counters |
| **Admin credentials** | Yes | The hash rides the image as an ordinary user-object variable, and is also file-backed: the kernel user object writes it to `src/kernel/data/admin.pwd` via `save_object` the moment it changes and re-reads that file at the next login, independent of the snapshot cycle (`docs/runtime-primitives.md` section 3) |
| **Open connections** | **No** | File descriptors are not in the snapshot |
| **Swap-file contents** | **No** | The swap is per-boot scratch; objects swapped to disk are paged into the snapshot at dump time |
| **Static variables, w.r.t. `save_object`** | **No** | `static` excludes from the per-object save format; full statedump is independent of the modifier |

The "what doesn't persist" entries above are the platform's contract boundary: the application is responsible for any state outside this set. The most common case is connections: clients reconnect after a restart. Hot boot is the platform's mechanism for preserving connections across a binary upgrade. See Hot boot below.

## The statedump cycle

`dump_state(int incr)` writes the in-memory image to the configured `dump_file`.

- `dump_state(0)` (or `dump_state()`): **full snapshot**. Writes the entire image. The prior `dump_file` rotates to `<dump_file>.old`.
- `dump_state(1)`: **incremental snapshot**. Writes changes since the prior full snapshot. The platform's recovery sequence restores the most-recent full snapshot, then applies the most-recent incremental on top.

Two trigger mechanisms:

1. **Automatic**: the `.dgd` configuration's `dump_interval` field (typical: 3600 seconds). The platform writes a snapshot every interval. The rotation moves `<dump_file>` → `<dump_file>.old` before the new snapshot is written, so if the new write is interrupted, the prior snapshot survives.
2. **Explicit**: an operator invokes the `snapshot` verb in `admin_console`, or code invokes `dump_state()` directly. Useful before maintenance windows, before risky deploys, or when an externally-triggered consistent backup is needed.

`dump_state` is **atomic with respect to in-flight operations**: the snapshot represents a consistent commit boundary. The runtime guarantees that the snapshot does not capture a state in the middle of an atomic context. Statedumps occur between timeslices, never inside an atomic operation.

The cost of a snapshot is proportional to the in-memory image size. A multi-gigabyte image can take seconds to write. The runtime briefly blocks during the dump. Workloads that cannot tolerate a brief pause should plan `dump_interval` against their peak-traffic schedule.

## Snapshot restore: cold boot from snapshot

When the host driver starts and finds a valid `dump_file`, it restores from the snapshot instead of running the cold-boot initd cascade:

1. The driver reads the snapshot file.
2. The in-memory image is reconstituted from the snapshot data.
3. The driver invokes the registered `restored(int hotboot)` hook in the kernel driver object (`src/kernel/sys/driver.c`) with `hotboot = 0`.
4. `restored` re-attaches managers and re-arms any external resources the snapshot did not preserve.
5. The driver begins accepting connections on the configured ports.

No `initd::create()` runs. The application's bootstrap was captured in the snapshot. Restore returns the platform to that state. Initd cascades only run on cold-boot-from-scratch.

If the snapshot is corrupt or `dump_file` is absent, the driver falls back to cold boot from scratch: the initd cascade runs, the platform boots without prior state, and connections start fresh.

The `<dump_file>.old` file is the rollback target if the most recent snapshot is corrupt or undesirable: remove the current `dump_file`, the driver finds `<dump_file>.old` (or the operator renames it), and restore proceeds from the prior snapshot.

## Hot boot: snapshot + execv + fd inheritance

Hot boot is the platform's mechanism for upgrading the host binary or `.dgd` configuration without losing connections or pending `call_out`s.

Prerequisites: the `.dgd` configuration carries a `hotboot` tuple naming the new binary, the new config, and the snapshot paths:

```text
hotboot = ({ "/path/to/new/dgd", "/path/to/new/config.dgd", "/path/to/dump_file", "/path/to/dump_file.old" });
```

Trigger: `shutdown(1)` from the kernel driver (operator-facing: the `hotboot` verb on the System login console. See `docs/admin-console.md`).

Sequence:

1. The runtime serializes per-connection state.
2. `dump_state(1)` writes an incremental snapshot.
3. `execv()` replaces the running executable with the binary named in the `hotboot` tuple. The new process inherits the open file descriptors of the old process (POSIX `execv` semantics).
4. The new binary restores from the snapshot.
5. The driver invokes `restored(int hotboot)` with `hotboot = 1`.
6. The connection-handling code re-attaches to the inherited file descriptors. Clients see no disconnect.

The `restored(int hotboot)` hook distinguishes hotboot-resume (`hotboot = 1`) from statedump-resume (`hotboot = 0`) so the platform can re-attach to surviving connections in one case and skip the re-attach in the other.

Failure modes:

- `hotboot` tuple absent: `shutdown(1)` raises "Hotbooting is disabled".
- New binary or config path invalid: `execv` fails. Platform exits without restore. Operator falls back to cold restart.
- Snapshot incompatible with the new binary (different `auto_object`, different `driver_object`, missing modules): restore fails. Platform exits.

## save_object / restore_object: explicit per-object save

The platform-wide statedump is the **default** mechanism for persistence. Application code does not call it. For applications that need explicit per-object save points independent of the full image (logging an event to disk, checkpointing a derived value), the host provides `save_object(filename)` and `restore_object(filename)`.

- `save_object(filename)`: writes the calling object's non-static variables to the named file. Format is host-defined (a structured text format roughly like LPC literal syntax).
- `restore_object(filename)`: reads the file and assigns to the calling object's matching variables.

The `static` modifier on a variable excludes it from `save_object`'s output. Static globals are runtime-only state with respect to the per-object save format. The full-image statedump captures them regardless (the image is the image, and the modifier affects only the save-object format).

Use `save_object`/`restore_object` when:

- An object needs a checkpoint independent of the platform's snapshot cadence (e.g., a critical operation just completed and needs durable confirmation before the next `dump_interval`).
- State needs to be portable across platforms (the per-object save file is human-readable and can be hand-edited or imported into a different platform).
- A subset of an object's state needs a stable on-disk representation distinct from the snapshot.

Do **not** use `save_object`/`restore_object` as a general persistence mechanism. That role is the platform's, and reinventing it on top is reproducing the runtime's work in application code (`docs/runtime-primitives.md` §3 names this as the platform's architectural commitment).

## Variable persistence semantics

The interaction between LPC's `static` modifier and the platform's persistence mechanisms:

| Variable kind | In statedump? | In save_object? |
|---|---|---|
| Non-static global | Yes | Yes |
| `static` global | Yes | No |
| Local variables | Not applicable (not part of object state) | Not applicable |

The platform-wide statedump captures the in-memory image without consulting modifiers. The `static` modifier exists specifically to interact with the per-object `save_object` format: marking a variable `static` says "do not write this to the per-object save file."

The application-level implication: a `static` variable is for state that is part of the object's in-memory identity (so it survives full statedump) but is not part of the object's portable per-object save (so it can hold derived values, caches, or runtime-only state without polluting the save file). A non-static variable is the default: captured in both places.

See `docs/lpc-essentials.md` Type modifiers for the language reference. The 2003 DGD-list discussion of the `static` keyword by Felix Croes covers the runtime semantics in detail.

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
- **Scheduled rotation**: the platform writes automatic snapshots at `dump_interval`. `snapshot` is the operator-on-demand variant.
- **Hot binary upgrade**: requires the `.dgd` `hotboot` tuple. Preserves connections and pending state across an `execv` replace.
- **Recovery from a wedge**: `reboot` returns the platform to the last consistent state without operator coordination across the host filesystem.

## Persistence boundaries

The platform's persistence model has explicit boundaries. Each requires application-layer handling:

- **Connections**: not in the snapshot. Statedump-restore does not preserve open client connections. Clients must reconnect. Hot boot preserves connections via `execv` fd inheritance.
- **External resources**: any state outside the platform's in-memory image is the application's responsibility. File-system state outside the platform's chrooted `directory`, external API tokens, and in-flight network requests do not persist across restart unless the application captures them in the platform's state.
- **Time**: the platform restores to the snapshot's logical time, but the host clock advances independently. `call_out` deferrals scheduled before the snapshot fire at the original target time relative to the snapshot's clock, which means a snapshot restored hours after capture has accumulated overdue `call_out`s ready to fire. Applications relying on `call_out` for time-sensitive work should handle the case where a deferred call fires later than originally scheduled:
  - **Detect lateness by carrying the deadline.** A `call_out`'s arguments are captured in the snapshot with their original values, so the cheapest detection is to schedule with the intended fire time as an argument (`call_out("expire", delay, time() + delay)`) and compare against `time()` when the function fires. A fire time well past the carried deadline means the deferral crossed a restore (or a long pause).
  - **Decide per work type.** Idempotent maintenance (flush, sweep, re-index) can simply run late. Expiry-shaped work (timeouts, token lifetimes) should evaluate against the carried deadline, not assume "now is approximately when I asked to run". An expiry that fires late should still expire against the original deadline. Interaction-shaped work (a reminder, a turn timer) may need to be dropped or re-derived when it fires meaningfully late, since the condition it was scheduled for may no longer hold.
  - **Re-anchor recurring schedules.** A self-re-arming `call_out` chain that carries its own cadence should compute the next delay from the current `time()` after a late fire rather than scheduling at the stale cadence offset, or the whole chain stays phase-shifted by the restore gap.
- **Snapshot integrity**: the platform writes snapshots atomically (rotation + write), but a host-level failure during the write (disk full, power loss) can produce a corrupt `dump_file`. The `<dump_file>.old` rotation is the platform's recovery mechanism. Backup snapshots to off-host storage for disaster recovery.

For operator-level recovery procedures when any of these boundaries are hit, see `docs/operations.md` Common failure modes (table of symptoms and diagnoses) and `docs/admin-console.md` for the `snapshot`, `reboot`, and `shutdown` verbs that manage the persistence cycle.

## Getting data out

Three paths move platform state to a portable, external representation today:

| Path | Covers | Format |
|---|---|---|
| `save_object` / `restore_object` | One object's non-static variables | Structured text, human-readable and portable (see save_object / restore_object above) |
| Vault + Schema | Any object with a registered per-app schema (`queryStateRoot()` names a `schema_node`) | XML, round-trips through Vault's store / spawn cycle (`docs/vault-applications.md`) |
| Property-table ascii marshal | A bare property-bearing object with no per-app schema, via the built-in `Core:Entries` schema | XML; values through `query_ascii_property` / `set_ascii_property` and the `/lib/util/coercion` codec (`docs/schema.md` Property-table marshaling) |

Outside these three, there is no export path today:

- **Non-schematized object graphs**: typed members with no registered schema and no property-table shape have nothing to walk them without one.
- **Observer slots carrying light-weight objects**: the `/lib/util/coercion` codec behind the property-table route refuses light-weight objects by design, not as an oversight (`docs/schema.md` Property-table marshaling).

The full-image statedump still captures this state, but its versioned snapshot format is for restoring into another DGD host, not for portable export outside the platform.

The committed direction is the Wave 2 generalized value serializer (`docs/runtime-platform-roadmap.md` Wave 2): round-trip serialization for any LPC value, schema-free, including recursive structures and light-weight objects. It is trigger-gated on the first cross-system transfer or non-schema marshal need. It is not yet built.

## Substrate verification

The platform's persistence contract is exercised by the bundled examples. The richer-state composition that lands with the property-change dispatcher (`docs/dispatcher.md` Persistence; `docs/runtime-primitives.md` §3 Extensions) is verified end-to-end by `examples/merry-app/sys/test.c` phases 16 and 17:

- Phase 16 sets up a parent / child pair with a property-bound Merry-script observer on the child, stashes the child as an LPC global on the test driver, schedules a `call_out` for the verification phase, and triggers a snapshot via `/usr/System/sys/persist_helper->trigger_dump_and_exit()`: the helper calls `dump_state(0)` followed by `shutdown()`.
- An external restart against the snapshot (`dgd example.dgd state/snapshot`) restores the image. The pre-snapshot `call_out` fires after restore.
- Phase 17 reads the saved LPC global, writes the observed property, and asserts that the value landed and that the observer's compiled source ran against the resurrected host.

Five orthogonal-persistence guarantees compose in the same verification: LPC global variables, property storage on host objects, references to compiled Merry-script clones at `/usr/Merry/merry/<md5>`, the observer-source contract (`$this` binding to the dispatch host, `Set` re-entry), and the scheduled call_out queue. Any future regression in those guarantees surfaces here as a `FAIL: PERSIST VERIFY ...` sentinel on the second-boot run.

`/usr/System/sys/persist_helper` is reusable: any future kernel-layer subsystem (Vault, schema, marshal) needing the same snapshot+restore verification harness calls `trigger_dump_and_exit()` from a test driver and follows the same two-boot pattern.

## Persistence under host-driver extensions

Loading a host-driver extension (`docs/operations.md` Loading host-driver extensions; `docs/architecture.md` Host-driver extensions) binds the platform's snapshot to that extension's presence: a snapshot taken with the extension active requires the same extension to restore. This is documented in [Felix Croes' 2010 Hydra mailing-list note][croes-hydra-2010] and is a durable architectural commitment, not an opt-in convenience.

Operational implication: removing an extension and restoring an old snapshot loses state. Plan extension choices accordingly. Treat extension addition as a one-way schema migration of the platform's persistence format.

For the question of how extension-loaded codepaths interact with the platform's atomic-rollback path (and therefore with the atomicity-of-state property that persistence relies on), see `docs/runtime-primitives.md` §1 Open and `docs/operations.md` Open empirical questions.

[croes-hydra-2010]: https://mail.dworkin.nl/pipermail/dgd/2010-August/006717.html

## Where to next

- **[`docs/runtime-primitives.md`](runtime-primitives.md)** §3: the per-primitive foundation-and-status statement for persistent state.
- **[`docs/admin-console.md`](admin-console.md)** Snapshot, restore, and shutdown: operator verbs for driving the persistence cycle interactively.
- **[`docs/operations.md`](operations.md)**: `.dgd` configuration fields (`dump_file`, `dump_interval`, `swap_file`, `hotboot` tuple), boot modes, failure-mode table.
- **[`docs/lpc-essentials.md`](lpc-essentials.md)** Type modifiers: language semantics for `static` and how it interacts with persistence.
- **[`docs/architecture.md`](architecture.md)** Boot sequence: the three boot modes (cold, statedump-restore, hot boot) at the platform level.
- **[`docs/vault-applications.md`](vault-applications.md)**: Vault subsystem for per-domain typed-property persistence on top of the snapshot cycle: participating-domain contract, on-disk XML shape, round-trip cycle.
- **[`src/kernel/sys/driver.c`](../src/kernel/sys/driver.c)**: the `restored(int hotboot)` hook implementation.

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html
