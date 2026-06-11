# Dispatcher

The dispatcher routes every `set_property` write through the Merry daemon so registered observers fire at pre / main / post timings, cascading writes stay bounded, recursive cycles are caught, and multi-key writes share a batch identity. It is the kernel layer's mechanism for "any property change can have side effects, anywhere in the object graph, expressed as Merry script."

For the smallest working introduction -- one observer, one write, one assertion -- start with `signal-applications.md` and `examples/signal-app/`; this page is the full reference. The substrate is small: one entry point (`dispatch_set`) on the Merry daemon, one property-layer hook in `/lib/util/properties`, one capability-gated registration LFUN (`register_observer`), two batching LFUNs (`batch` and `batched_set`), and a handful of accessors. Observers themselves are ordinary Merry scripts -- the same `/usr/Merry/data/merry` clones that `find_merry` / `run_merry` already dispatch elsewhere. The dispatcher is one application of the script-binding primitive, layered on top of it rather than parallel to it.

`set_property` and the dispatcher cooperate: the property-layer hook calls `MERRY->dispatch_set(obj, path, val)` when the Merry daemon is loaded; the dispatcher's main timing then calls `obj->set_raw_property(path, val)` to perform the actual write and recurses through pre and post observers. Hosts that need to escape the dispatcher (early bootstrap, raw schema initialization) call `set_raw_property` directly.

## Audience and how to read this

This document covers two views of the dispatcher:

- The **application-author view** (sections "Worked example" through "Observer-source contract") covers the public surface a `/usr/<App>/` author calls and writes against -- the LFUNs, the property naming, the observer-source contract, and the visible behaviors.
- The **kernel-author view** (section "Kernel-layer internals") covers the implementation: TLS keys, observer caching, private helpers, audit-log shape, and the contracts internal helpers honor.

The "Relationship to Merry script binding" and "Persistence" sections bridge the two -- both audiences need them.

The "Verification" section maps each documented signature to the MerryApp smoke phase that exercises it, so a reviewer can step from prose to evidence.

## Worked example

A host registers a main-timing observer that records firing, then writes the observed property:

```c
# define MERRY   "/usr/Merry/sys/merry"
# define THING   "/usr/MerryApp/obj/thing"

object child;

child = clone_object(THING);

MERRY->register_observer(child, "test:val", "main",
    "Set($this, \"test:fired\", 1); return TRUE;");

child->set_property("test:val", 42);

/* afterwards:
 *   child->query_raw_property("test:val")   == 42
 *   child->query_raw_property("test:fired") == 1
 */
```

Three things happened on `set_property`:

1. The property-layer hook in `/lib/util/properties` saw that the Merry daemon was loaded and forwarded to `MERRY->dispatch_set(child, "test:val", 42)`.
2. `dispatch_set` entered an implicit batch (since no explicit one was active), checked the cascade-depth bound and the per-execution cycle chain, walked the ancestry via `query_ur_object` looking for observers at `merry:on:test:val:pre` (none), wrote `42` to the raw property, then fired the main observer.
3. The observer's compiled Merry script executed against an `$this` of `child`; its `Set($this, "test:fired", 1)` re-entered `set_property` and (a second, cleanly nested) `dispatch_set` for `test:fired`, which (no observers, no cycle, depth still small) wrote `1` and returned.

The implicit batch is recorded as `completed` in the batch-status log on exit. The dispatcher has run with no application-author involvement beyond the `set_property` call.

## Application surface

### `register_observer(object ob, string path, string timing, string source)`

Registers a Merry-script observer at the given (`path`, `timing`) on `ob`. Stored as a property-list under `merry:on:<path>:<timing>`. Multiple observers may be registered for the same slot; they fire in registration order.

- `ob` -- host object. Must be non-nil and must inherit `/lib/util/properties`; the daemon refuses a target without the property API (before that validation, such a registration reported success while storing nothing, since `call_other` on a missing function returns nil).
- `path` -- property path (dot-notation; arbitrary string).
- `timing` -- one of `"pre"`, `"main"`, `"post"` (case-insensitive). Anything else errors; nil collapses to `"main"`.
- `source` -- Merry source string. Compiled at registration time (a `/usr/Merry/data/merry` clone is created); the compiled object is what gets stored.

Capability-gated per `DD-1 (e)`: callable from `/kernel/`, from a `KERNEL()`-trusting registrar program, and from any program whose creator is in the approved-registrar set. Application code typically calls through its own initd or a registered admin path; raw test drivers that want to register from `/usr/<App>/sys/test.c` need their creator listed via `add_approved_registrar(<domain>)` (KERNEL-gated). The MerryApp example registers from the test driver directly, so `MerryApp` lives in the approved set at boot.

Errors on: nil host, non-property-bearing host, unrecognized timing, capability-gate failure. The compile step may also error if the source has a parse failure.

### `dispatch_set(object obj, string path, mixed val)`

The single entry point that property writes route through when the Merry daemon is loaded. `/lib/util/properties::set_property` calls it; application code rarely calls it directly. Documented here so the application-author can read failure modes that originate in dispatch and the cascade / cycle / veto behaviors.

Cooperates with `set_property` via this contract: dispatcher does the implicit-batch wrap, the cascade-depth check, the cycle-chain check, the pre-observer fan-out, the raw write (`obj->set_raw_property(path, val)`), the main-observer fan-out, the post-observer fan-out, the cascade-depth decrement, and the implicit-batch exit. If a pre observer throws, the write does NOT land and the error propagates. If a main or post observer throws, the write HAS already landed; the error propagates and the batch status records `main-aborted` or `post-aborted`. The cycle check throws `merry: observer cycle detected at <object_name>:<path>` and records `cycle-detected`.

### Property-layer hook architecture

`/lib/util/properties::set_property` is the inheritable LFUN every property-bearing host calls when it sets a logical property. It is the integration point between the property storage primitive and the dispatcher:

```c
/* /lib/util/properties::set_property (paraphrased) */
void set_property(string path, mixed val)
{
    object merry;

    merry = find_object("/usr/Merry/sys/merry");
    if (merry) {
        merry->dispatch_set(this_object(), path, val);
    } else {
        set_raw_property(path, val);
    }
}
```

Two consequences for application authors:

- The dispatcher is *opt-in by environment*. A kernel-layer instance that does not load the Merry daemon (e.g., the cloud-server upstream, or an early-bootstrap probe) sees `find_object(MERRY)` return nil and writes go straight through to `set_raw_property`. The property primitive remains usable without the dispatcher.
- Inside the dispatcher, writes back to the host's raw property use `set_raw_property` rather than `set_property` -- that is what prevents the dispatcher from infinitely re-entering itself during its own main-timing write. Application code that wants the same bypass (early bootstrap, schema initialization, fixture seeding) calls `set_raw_property` directly. The cycle detector exists for accidental observer-source recursion; `set_raw_property` is for intentional bypass.

### Ancestry walk -- the `merry:on-inherit:*` re-enable marker (DD-5 (a))

The dispatcher's observer lookup walks `query_ur_object()` from the host upward. The walk policy is **declarative-dominant with explicit re-enable**:

| Ancestor level state | Effect |
|---|---|
| Local observers present at this level, no re-enable marker | Accumulate this level's observers and **stop** the walk. The local declaration is dominant. |
| Local observers present + re-enable marker `merry:on-inherit:<path>:<timing>` | Accumulate this level's observers and **continue** walking upward. Ancestor observers also fire. |
| No local observers at this level | Continue walking. The level is transparent. |

The default is local-override: if a descendant declares its own observer for `test:val:main`, the ancestor's observer does NOT fire. This is the path of least surprise -- a host that explicitly registers a behavior should not also pull in inherited behaviors it did not opt into.

The re-enable marker is per-`(path, timing)`. Setting `obj->set_property("merry:on-inherit:test:val:main", 1)` says "I have local observers for `test:val:main` AND I want my ancestors' observers for `test:val:main` to also fire." Other timing slots are independent (per `DD-5 (b)`); a host that re-enables inheritance for main does not affect pre or post.

Worked example:

```c
/* parent has a logging observer; child wants its own behavior PLUS the parent's */
MERRY->register_observer(parent, "audit:event", "main",
    "Log(\"audit fired on \" + ObjectName($this));");

MERRY->register_observer(child, "audit:event", "main",
    "Set($this, \"child:audit:seen\", 1);");

child->set_property("merry:on-inherit:audit:event:main", 1);

child->set_property("audit:event", "logged in");
/* fires both: child's main observer AND parent's main observer */
```

Without the marker, only `child`'s observer fires.

The marker is itself an ordinary property. The cache invalidation in `dispatch_set` recognizes both `merry:on:*` and `merry:on-inherit:*` writes and broadly invalidates the cache on either; either kind of registration change must be re-resolved at the next dispatch.

### `batch(object obj, string func, mixed *args, varargs mapping opts)` and `batched_set(object obj, mapping kv_map, varargs mapping opts)`

Two batching APIs per `DD-1 (c)`. `batch` calls `func` on `obj` with `args` inside a fresh batch context; any `set_property` writes the callable performs share the batch id. `batched_set` writes the `kv_map`'s key/value pairs (each via `set_property`) inside a single batch.

Opts is an optional mapping:

- `"atomic": 1` -- run the callable / writes inside a DGD `atomic{}` block per `DD-4 (d)`. On error all writes roll back; the batch-status entry is suppressed (atomic rollback removes the daemon-local entry the same way it removes the application's writes). On success, writes commit and the entry records `completed`.

Both return the callable's return value (`batch`) or `nil` (`batched_set`). Both propagate errors from the callable / writes and from the dispatcher itself.

`batched_set` is also reachable from Merry observer source via the `BatchedSet` merryfun -- the mapping-arg signature composes inline (no function-reference required per `L14 #15`), so observers can perform multi-key writes themselves.

### `set_max_cascade_depth(int n)` and `query_max_cascade_depth()`

Configure the depth-shaped cascade bound. Default is `32`. `set_max_cascade_depth` is `KERNEL()`-gated; `query_max_cascade_depth` is public read-only.

The bound counts depth, not breadth -- a flat batch with many legitimate writes does not increment the counter; a recursive observer chain does. When the counter equals the bound at dispatch entry, the dispatcher throws `merry: cascade depth N exceeded at <object_name>:<path>`, records `cascade-aborted` in the batch-status, and unwinds the implicit batch if it entered one.

### `unregister_observer(object ob, string path, string timing)`

Removes ALL observers at the `(ob, path, timing)` triple by clearing the `merry:on:<path>:<timing>` property. Capability-gated identically to `register_observer` via `_check_registrar`, and refuses a non-property-bearing target the same way. Asymmetric with `register_observer`'s add-one semantics; finer-grained removal (by source string or by compiled-object identity) is a future-work item (see eos-harness BACKLOG `#FH-14`).

The MVA-scope coarse granularity is sufficient for the common operator scenario (clear all observers at a triple to start fresh, e.g., for an in-flight troubleshooting session). For surgical removal in a multi-observer-on-one-triple host, current options are (a) read the property value list, remove one entry by index, write back via `set_raw_property` (operator-tier surgery; admin verb `register-observer` / `unregister-observer` in `admin-console.md` does not yet expose this), or (b) clear all and re-register the keepers.

### `set_dispatch_trace(int flag)` and `query_dispatch_trace()`

Toggle the optional verbose dispatcher trace logging. `set_dispatch_trace` is `KERNEL()`-gated; `query_dispatch_trace` is public read-only. Default is `0` (off). When `1`, the `_trace_dispatch` private helper writes per-entry trace events to `/usr/Merry/log/dispatch.log` alongside the always-on cycle and cascade events from `_log_dispatch`. The flag is statedump-persistent.

Current trace coverage is the `dispatch_set` entry site only (MVA demonstration). Additional trace sites (batch-entry/exit, observer-fire, cascade-depth-increment, cycle-chain mutation, observer-cache hit/miss) are future-work (see eos-harness BACKLOG `#FH-15`). The flag-gating contract is established; site additions are mechanical.

The `dispatch-trace on|off|status` admin verb (see `admin-console.md`) is the operator-facing surface; it routes through `ADMIN_CONSOLE_REGISTRY->verb_set_dispatch_trace` for the KERNEL elevation since the underlying setter is gated.

### Batch status

`_record_batch_status` writes a status entry for each batch; `_query_batch_status(int batch_id)` reads it back. Status values per `DD-3 (c)`:

| Status | When |
|---|---|
| `completed` | The batch finished without error. |
| `cascade-aborted` | The cascade-depth bound was exceeded inside the batch. |
| `cycle-detected` | A recursive `(obj, path)` was attempted inside the batch. |
| `pre-vetoed` | A pre-timing observer threw. The write did NOT land. |
| `main-aborted` | A main-timing observer or the callable itself threw. The write HAS landed. |
| `post-aborted` | A post-timing observer threw. The write HAS landed and main observers HAVE fired. |

Check-before-overwrite contract: a status entry, once written, is not overwritten by a later status for the same batch. This is what makes `pre-vetoed` distinguishable from `cycle-detected` distinguishable from `cascade-aborted` -- the first one to fire wins.

### Observer-source contract

A registered observer's source is plain Merry source compiled to a `/usr/Merry/data/merry` clone. The dispatcher passes the following bindings when it invokes the script:

| Binding | Type | Meaning |
|---|---|---|
| `$this` | `object` | The dispatch host -- the object the property was set on (not the ancestor that owns the observer, when ancestry walks land an observer up-chain). |
| `$path` | `string` | The property path being written. |
| `$new` | mixed | The value being written. |
| `$old` | mixed | The previous value at that path (nil if the property was unset). |
| `$timing` | `string` | One of `"pre"`, `"main"`, `"post"` -- which slot this observer is in. |
| `$seq` | `int` | The dispatcher's per-batch sequence number for this write, starting at 0. |

The observer's return value is currently unused -- it is conventional to `return TRUE;` but the dispatcher does not branch on it.

A `pre` observer that wants to veto the write throws via `error(...)`. The dispatcher does not have a separate "return false to veto" path; errors are the veto mechanism.

A `main` or `post` observer that wants to record state on the host calls `Set($this, "...", ...)` (which re-enters `set_property` and recurses through `dispatch_set`, bounded by cascade + cycle checks). Calls to `Set` from observer source on the same `(obj, path)` that triggered the dispatch are caught by the cycle detector.

An observer may also write a *different* object: `Set($other, "...", ...)` forwards to `$other->set_property(...)` (the `Set` merryfun takes an explicit object), so a post-timing reaction can fan an event out to other objects -- a mention-notify observer writing each mentioned user's tracker, for instance. The cross-object write is a fresh dispatch on the target, independent of the host's.

### Deferred observer continuations (`$delay` / `In` / `Every`)

An observer may defer part of its work to a later tick with `$delay(seconds, retval)`. The first call returns `retval` synchronously -- so the `set_property` that triggered the dispatch returns without waiting -- and the rest of the source resumes `seconds` later in a fresh `call_out` (a new task, a new dispatch batch).

This is for a genuinely deferred *operation*, and it is a different thing from event notification. A normal observer reaction fires synchronously, inside the dispatch, sharing the triggering write's atomic envelope -- if the write rolls back, the reaction rolls back with it. A `$delay`-ed continuation runs in a *separate* envelope on a later tick, so it does NOT roll back if the original write fails. Keep the two distinct: notify-on-change belongs in the synchronous body; only genuinely cross-operation work (a timed retry, a deferred cleanup, a delivery receipt) belongs after a `$delay`.

```c
/* post-timing observer: write a delivery receipt one tick later */
MERRY->register_observer(room, "chat-room.message", "post",
    "$delay(2, FALSE); " +
    "Set($this, \"chat-room.delivery-receipt\", 1); return TRUE;");
```

For this to work the dispatch host must expose the `$delay` glue pair -- `delayed_call(object ob, string fun, mixed delay, mixed args...)` and a `static void perform_delayed_call(object ob, string fun, mixed *args)` companion -- both provided by `/lib/util/delayed`, which the host inherits.

The continuation resumes the **same compiled observer object** rather than re-locating a script by the `merry:<mode>:<signal>` find-convention: `merrynode.c::do_delay` captures `this_object()` (the running compiled observer) into the `mcontext`, and `mcontext::merry_continuation` resumes it via `code->evaluate(host, signal, mode, args, label)`. This is what lets `$delay` compose with dispatcher observers at all -- the dispatcher stores observers under `merry:on:<path>:<timing>` and fires them by direct compiled-object reference, so a convention-only resume (`run_merries` -> `find_merries`) would find nothing. Convention-invoked scripts (e.g. the merry-app `$delay` example) keep working because `merry_continuation` falls back to `run_merries` when no compiled object was captured. The change-context bindings (`$this`, `$new`, ...) survive the delay because `do_delay` copies the args mapping into the `mcontext`.

## Relationship to Merry script binding

The dispatcher is layered on top of the Merry script-binding mechanism documented in `merry-applications.md` -- observers are merry scripts, and `register_observer` writes to a property that `find_merry` / `find_merries` semantics can read.

The property convention `merry:on:<path>:<timing>` parallels `merry:<mode>:<signal>`:

| Property | Mode | Signal | Read by |
|---|---|---|---|
| `merry:lib:greet` | `lib` | `greet` | `find_merry(ob, "greet", "lib")` -- application invocation |
| `merry:on:test:val:main` | `on` | `test:val:main` | `find_observers(ob, "test:val", "main")` -- dispatch |

`on` is the dispatcher's mode namespace; the `<path>:<timing>` composite is its signal. The lookup mechanics are different in detail (the dispatcher iterates the property list per timing slot, while application invocation typically takes the first hit), but the storage shape is the same and the ancestry walk through `query_ur_object` is identical.

The compositional consequence is that the dispatcher reuses, rather than re-implements, the merry-script lifecycle:

- Source is compiled to a `/usr/Merry/data/merry` clone at registration time, the same way `find_merry`'s consumers compile scripts.
- The compiled object lives at `/usr/Merry/merry/<md5>`, the same path the rest of Merry uses.
- The on-disk compile artifact (`/usr/Merry/merry/<md5>.c`) is created the same way; DGD's standard compile-on-demand resurrects it after restore.
- The sandbox restrictions in `merrynode.c` apply -- observers cannot escape via `clone_object`, `dump_state`, or other guarded kfuns.

An application that wants to "register custom callbacks on property changes" does not learn a new mechanism. It learns the dispatcher's registration LFUN and writes Merry source. Everything else -- ancestry, sandboxing, persistence, the property storage convention -- is the script-binding mechanism it already knows.

## Persistence

Observer state survives a snapshot + restore cycle end-to-end. This is verified empirically by the `examples/merry-app` smoke; the mechanism rests on DGD's standard orthogonal-persistence guarantees applied across the dispatcher's specific composition.

What survives, and why:

| Element | Survives because |
|---|---|
| LPC global variables on application objects | DGD pickles them with the object's data segment. |
| Property storage on host objects | The `merry:on:<path>:<timing>` properties are ordinary properties; property storage survives. |
| References to the compiled Merry-script object | An LPC `object` value is an oid that DGD resurrects on restore -- the property mapping's stored reference re-attaches to the same logical clone. |
| The on-disk compile artifact | `data/merry::get_program()` writes `/usr/Merry/merry/<md5>.c` at registration; DGD's compile-on-demand reloads the program when the resurrected oid is first dispatched against. |
| Scheduled `call_out`s | DGD's call_out queue is part of the snapshot. Call_outs scheduled before dump fire at their original wall-clock target after restore (or immediately if the target has passed). |
| `observer_cache` and other daemon-local mappings | The Merry daemon's mappings survive as object data. The cache is conservatively invalidated by `_invalidate_observer_cache` on registration; that contract is unchanged by restore. |

What is verified by the MerryApp smoke (phases 16 and 17):

- Phase 16 clones a fresh parent / child pair, registers a main observer on the child for `test:persist:val`, saves the child as a `static` global on the test driver, schedules a `phase17_verify` call_out three seconds out, and calls `/usr/System/sys/persist_helper->trigger_dump_and_exit()`. The helper writes a full snapshot via `dump_state(FALSE)` and `shutdown()`s the driver.
- An external restart -- `dgd mva.dgd state/snapshot` -- restores. The pre-snapshot `call_out` fires after restore.
- Phase 17 reads the saved `persist_host`, writes `42` to `test:persist:val`, and asserts that both the value landed (property storage survived) and that `test:persist:fired` became `1` (the observer's compiled source ran against the restored object reference).

The smoke is the first end-to-end empirical confirmation that the dispatcher's substrate composes correctly with DGD's persistence. Prior phases (DI-1, DI-2, DI-3) verified each runtime primitive in isolation against cold-boot; phase 17 verifies their composition across restore. Any future regression in the persistence contract -- e.g., a change to how the compiled merry-script artifact is named, a change to property-mapping pickling, an observer cache that fails to rebuild after restore -- surfaces here as a `PERSIST VERIFY FAIL: ...` sentinel.

## Kernel-layer internals

Internals are documented here so a maintainer modifying `merry.c` can keep contracts intact. Application-authors should not need this section.

### TLS keys and globals

The dispatcher uses two TLS keys (defined in `tls.h`):

- `TLS_BATCH_STACK` -- stack of batch-context mappings (`mapping *`). Pushed by `_push_batch_context`, popped by `_pop_batch_context`. Each context carries `batch_id`, `atomic_mode`, `cascade_depth`, `seq`, and copy-of-`opts`.
- `TLS_CYCLE_CHAIN` -- per-execution chain of `_cycle_key` strings (`string *`). Pushed on entry to `_fire_timing_slot`'s main timing, popped on exit. The chain is per-thread (DGD's TLS), so observer cascades across multiple objects are tracked correctly without collision across user sessions.

Daemon-local mappings:

- `approved_registrars` -- the set of creator domains permitted to call `register_observer` from outside `/kernel`. Mutated only via the KERNEL-gated `add_approved_registrar` / `remove_approved_registrar`; queryable read-only via `query_approved_registrars`. Statedump-persistent.
- `observer_cache` -- mapping from `(object_name, path, timing)` triple-key to resolved observer list. Populated on first dispatch through `_find_observers_cached`; invalidated by `_invalidate_observer_cache` on registration. Statedump-persistent.
- `batch_status` -- mapping from `batch_id` to `({ status, reason })`. Written by `_record_batch_status`; read back by `_query_batch_status`. Statedump-persistent.
- `next_batch_id` -- monotonic batch counter. Starts at 1 (0 is sentinel for "no batch active"). Statedump-persistent.
- `max_cascade_depth` -- the configured bound. Default 32; mutated via the KERNEL-gated `set_max_cascade_depth`; read via the public `query_max_cascade_depth`. Statedump-persistent.

### `observer_cache`

The cache exists because the per-dispatch ancestry walk is O(ancestor-count) and observers don't change between registrations. `_find_observers_cached(obj, path, timing)` looks up the triple-key; on miss, runs the walk (`query_ur_object` from `obj` upward, reading `merry:on:<path>:<timing>` at each level), caches the result, and returns.

Invalidation is conservative: `register_observer` calls `_invalidate_observer_cache(ob, path, timing)`, which removes all cache entries whose first key element matches `object_name(ob)` AND whose path matches AND whose timing matches. This is broad enough to catch ancestor-side registrations correctly (a registration on an ancestor invalidates descendant cache entries) at the cost of full mapping iteration on each registration. Acceptable for MVA scope; a more targeted invalidation is post-MVA polish.

### `_resolve_observer(mixed val)`

The dispatcher stores observers as compiled object references after `DI-3` (versus the source strings `DI-1` stored). `_resolve_observer` accepts either shape -- it returns the value unchanged if it is an `object`, looks it up via `new_object`-compile if it is a `string` (the `DI-1` legacy shape, kept for forward-compat through the property-storage transition). Currently called only when iterating the observer list inside `_fire_timing_slot`; the legacy-string path is exercised by no current registration site but kept until a sweep over upgrade-path scenarios is complete.

### `_fire_timing_slot(object obj, string path, string timing, ...)`

The per-timing-slot fan-out. Pulls the cached observer list, advances the per-batch `seq` counter for each fire, pushes the cycle chain on `main` entry (popped on exit), invokes the compiled merry-script via `run_merry` against the static `merryapi` surface, and propagates any thrown error. The throw-propagation is what powers the `pre-vetoed` / `main-aborted` / `post-aborted` distinction in the batch-status enum -- the timing slot the throw originated in is what gets recorded.

`_fire_timing_slot` does NOT consult `_cycle_chain_get` itself; the cycle check happens at `dispatch_set` entry. The chain is maintained per-`(obj, path)` -- pushed for the firing pair, popped before unwinding, so a cousin write at the same `path` on a sibling object is not blocked.

### Cycle detection chain

`_cycle_key(obj, path)` -- formats `<object_name>:<path>`. Cycle detection compares string equality on this key against the current chain (which is an array, queried via the `member` predicate from `/lib/util/lpc`). Per-execution scope keeps the data structure flat and the check O(chain-depth); the upper bound on chain length is `max_cascade_depth` so the linear scan is bounded.

### Audit log -- `_log_dispatch`

`_log_dispatch(string msg)` appends a line to `/usr/Merry/log/dispatch.log` with a timestamp prefix. Currently writes only on dispatcher-detected failures (cycle, cascade overflow, observer source compile error). Volume is therefore low under normal operation. The path is operationally visible per `operations.md`; the log is rotated by ordinary log-management tooling (no per-Merry rotation policy is built in).

### Private helpers

- `_check_registrar(string caller_program, string target_name)` -- enforces the registration capability gate. Called only from `register_observer`.
- `_push_batch_context` / `_pop_batch_context` -- batch lifecycle. Push allocates a `batch_id`, sets the atomic flag and copy-of-opts, initializes cascade_depth and seq, and stashes the previous chain on the TLS stack. Pop restores.
- `_enter_implicit_batch` / `_exit_implicit_batch` -- the implicit-batch wrap for unbatched `set_property` writes. Returns 0 if a batch was already active (no implicit needed) or the new batch_id otherwise.
- `_run_callable` / `_run_kv_writes` -- the non-atomic execution paths for `batch` and `batched_set`.
- `_atomic_run_callable` / `_atomic_run_kv_writes` -- the `private atomic` execution paths. The `atomic` keyword wraps the LFUN body in DGD's atomic{} so any error rolls back ALL writes performed within.
- `_current_batch_id` / `_current_batch_context` / `_current_batch_seq_advance` -- the dispatcher's accessors into the batch stack.

## Verification -- the MerryApp smoke

Each documented signature is exercised by at least one phase of the 17-phase MerryApp smoke (`examples/merry-app/sys/test.c`). The map below lets a reviewer step from this document to evidence:

| Signature / behavior | Phase | Sentinel |
|---|---|---|
| `register_observer` (main timing) | 10 | `DISPATCH MAIN OK` |
| `register_observer` (pre / main / post slots) | 11 | `DISPATCH ORDER OK` |
| `register_observer` (pre veto via throw) | 12 | `DISPATCH VETO OK` |
| `register_observer` (ancestry walk; `$this` is dispatch host, not observer owner) | 14 | `DISPATCH ANCESTRY OK` |
| `dispatch_set` -- pre / main / post sequencing | 11 | `DISPATCH ORDER OK` |
| `dispatch_set` -- pre-veto, write does not land | 12 | `DISPATCH VETO OK` |
| `dispatch_set` -- cycle detection | 13 | `DISPATCH CYCLE OK` |
| `dispatch_set` -- implicit batch wrap | 15 | `DISPATCH IMPLICIT OK` |
| `batch` (non-atomic, callable abort) | 9 | `BATCH ABORT OK` |
| `batched_set` (non-atomic) | 6 | `BATCH OK` |
| `batched_set` (atomic mode) | 7 | `BATCH ATOMIC OK` |
| `batched_set` (from Merry source via `BatchedSet`) | 8 | `BATCH SOURCE OK` |
| Batch status -- `completed` | 6, 7, 8, 15 | various |
| Batch status -- `main-aborted` | 9 | `BATCH ABORT OK` |
| `max_cascade_depth` bound | (DI-3 hits at default; an explicit test phase covering `cascade-aborted` is `DI-8` scope) | -- |
| Observer-source contract -- `$this` binding to dispatch host | 14 | `DISPATCH ANCESTRY OK` |
| Observer-source contract -- `Set` from observer source | 10, 11, 13, 14, 16 | various |
| `BatchedSet` from observer source | 8 | `BATCH SOURCE OK` |
| Observer-state survival across snapshot + restore | 16 + 17 | `PERSIST SETUP OK` + `PERSIST VERIFY OK` |
| `unregister_observer` (clears all observers at triple) | telnet-drive | admin verb `unregister-observer` end-to-end against MerryApp |
| `set_dispatch_trace` / `query_dispatch_trace` | telnet-drive | admin verb `dispatch-trace on|off|status` end-to-end against MerryApp |

A test phase that does not appear above is from the pre-dispatcher lift (phases 1-5 cover the Merry script-binding primitive itself; phase 4's `DELAY OK` covers the `$delay()` continuation path documented in `merry-applications.md`). The two telnet-drive rows reference operator-tier verification rather than smoke-phase markers: the verbs were exercised via direct telnet session against the restore-boot MerryApp; see `admin-console.md` "Dispatcher operator surface" for the full session and the verb-by-verb output format.

## See also

- `merry-applications.md` -- the script-binding mechanism the dispatcher is layered on top of (ancestry walk, find_merry / find_merries, the merry property convention, sandbox semantics).
- `operations.md` -- operator surface for `set_max_cascade_depth`, `set_dispatch_trace`, and the dispatch audit log.
- `admin-console.md` -- "Dispatcher operator surface" section: the nine operator verbs that expose the dispatcher's runtime surface from the kernel admin console (`observers`, `cascade-depth`, `batch-status`, `dispatch-trace`, `register-observer`, `unregister-observer`, `query-approved-registrars`, `approve-registrar`, `unapprove-registrar`).
- `runtime-primitives.md` -- the architectural framing (the dispatcher is the substrate that makes richer persistent state and async events first-class in the kernel layer).
- `examples/merry-app/` -- the executable verification anchor; each documented signature is exercised by a phase marker.
