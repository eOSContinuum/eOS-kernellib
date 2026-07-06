# Code lifecycle

LPC code in the platform moves through these states: source becomes a master, a master spawns clones, `create()` runs, recompilation replaces a running master in place, `call_touch` schedules lazy upgrades, and `destruct_object` removes objects from the runtime. The object-manager event surface lets the platform observe each transition. The sections below cover each in turn.

For the LPC language constructs these transitions invoke (`inherit`, `create()`, `static`, `nomask`), see `docs/lpc-essentials.md`. For the per-primitive runtime guarantees that bound these transitions (atomicity, hot reload, capability separation), see `docs/runtime-primitives.md`. For the operator surface that drives lifecycle transitions interactively, see `docs/admin-console.md`.

**Audience**: an LPC author or operator reasoning about how source becomes a running object, how recompilation propagates to existing instances, how `call_touch` schedules lazy upgrades, and how the platform observes lifecycle transitions; assumes `docs/lpc-essentials.md` for language constructs and `docs/architecture.md` for the structural model.

## Transitions

A piece of LPC source moves through these states in the platform:

```text
   (source on disk)
        |
        | compile_object(path)
        v
     master  -----------------------+
        |                           | (recompile)
        | clone_object(master)      | compile_object(path)  — same path
        v                           v
      clone                      master'  (same path, new program)
        |
        | destruct_object(clone)
        v
   (clone destroyed)
```

A `master` is the compiled program for a given path. There is at most one master per path in the runtime at any time. A `clone` is an instance of a master with its own dataspace. Multiple clones share a master's program but each carries its own variables. A clone's name is the master path plus `#N` where `N` is a unique clone index.

Light-Weight Objects (LWOs) follow a parallel pattern via `new_object()`: structured value types instantiated from a `/data/` master, lived inside a containing object's dataspace rather than independently.

## Compile: creating a master

`compile_object(path)` compiles the LPC source at `path` (with implicit `.c` suffix) and installs the resulting program as the path's master. If a master already exists at `path`, this is a **recompile** — see Hot reload below.

The two-argument form, `compile_object(path, source)`, compiles from an in-memory string rather than from disk; useful for platforms that synthesize LPC at runtime.

Compilation runs in atomic context. If the source has a syntax error or fails type-check, the compile errors and the platform's prior state (including any prior master at `path`) is unchanged. The atomicity guarantee (`docs/runtime-primitives.md` §1) covers the compile transition itself: either the new program installs cleanly or the runtime rolls back as if the call never happened.

After successful compile, the master sits idle. Its `create()` does not run until the first function call against the master. The object manager (see below) receives a `compile` event with the owner, the master's path, the source mapping, and the inherited paths; this is the platform's hook for tracking the dependency graph.

## _F_create: the create-hook dispatch

The host driver invokes a configured "create" hook on every object's **first function call**. The `.dgd` configuration's `create` line names the hook; eOS-kernellib uses `_F_create`. The kernel auto at `src/kernel/lib/auto.c` implements it:

```c
nomask void _F_create()
{
    if (!creator) {
        /* set creator and owner from object_name */
        /* if this is a clone, register with the driver's clone manager */
        /* call System-tier creator if defined, otherwise call create() */
        create();
    }
}
```

Three points the wrapper carries:

1. **`if (!creator)` guard** — `_F_create` may be called more than once in the host's dispatch path; the guard ensures the user-visible `create()` runs at most once per object.
2. **Owner and creator identity** are set from the object's path before `create()` runs. Application `create()` code can rely on `query_owner()` returning the correct identity.
3. **`create()` is the application hook**; `_F_create` is the platform wiring. Application authors write `create()`; the kernel auto handles the dispatch.

A clone's `create()` runs in its own dataspace at clone time. The master's `create()` runs once when the first call against the master fires after compile (which may or may not be at compile time depending on the application's pattern; an initd that does `compile_object("/usr/MyApp/obj/foo")` followed by a method call against the master triggers `create()` then). The kernel auto handles the master/clone distinction via its registration logic, so most application authors write a single `create()` body that runs on both master and clones.

`create()` is declared `static` by convention — callable only from the object itself and its children. The host runtime invokes `_F_create` directly; `static create()` is the LPC-visible end of the dispatch.

## Clone: instantiation

`clone_object(master_path)` creates a clone of the named master. The clone receives a fresh dataspace; its `create()` runs with the host's automatic dispatch.

Constraints:

- The master must be a clonable. By platform convention this means a path under `/usr/.../obj/` rather than `/usr/.../lib/`. The kernel's `clone_object` kfun does not check for `/obj/`; it rejects a `/lib/` path (and, for non-kernel callers, any `/kernel/` path). The `/obj/` convention is enforced one layer up, by the System-tier auto (`src/usr/System/lib/auto.c`), not by the kernel.
- Clones cannot be cloned. `clone_object(clone)` errors.
- A clone's owner is the **clone-time caller's owner**, not the master's owner. Two clones of the same master can have different owners if their callers do.

The object manager receives a `clone` event with the clone object before its `create()` runs.

## LWO instantiation: new_object

`new_object(master)` instantiates a Light-Weight Object from a `/data/` master. This is the consolidated LWO reference; `docs/glossary.md`, `docs/lpc-essentials.md`, and `docs/architecture.md` carry one-line summaries that point here.

**Creation and identity.** The new LWO's creator function runs immediately at instantiation (unlike a master, whose `create()` defers to the first call). An LWO's name is the master path plus `#-1` — every LWO of a master shares that name; there is no per-instance index and no entry in the object table. Calling `new_object()` on an existing LWO returns a copy of it.

**Lifetime.** LWOs cannot be destructed — `destruct_object()` does not apply. An LWO is deallocated when the last reference to it is dropped. Statedump captures LWOs as part of the dataspace that holds them, like every other in-image value.

**Sharing semantics — alias within a dataspace, value across dataspaces.** An LWO lives in the dataspace of an object that references it, with the same locality rules the driver applies to arrays and mappings:

- Within one object's dataspace, references alias: a mutation through one reference is visible through every other, for as long as execution continues.
- When a reference is exported to another object's dataspace (stored in its variables, passed and kept), each dataspace ends up with its own copy. The driver performs the export processing between executions — never in the middle of one — so within a single task the shared reference still aliases; by the next task each holder has an independent copy.

The practical rule: treat a cross-object LWO handoff as pass-by-value. Mutating an LWO after handing it to another object does not propagate; an LWO is not a substitute for a clone where shared mutable identity is the point (the chat example's admin token works precisely because a capability *should* be a value attached to its holder, not a shared mutable object).

**Upgrade.** Recompiling a `/data/` master upgrades existing LWOs of that master; their variable layout is remapped to the new program like clones under hot reload.

**Differences from clones, in brief**:

| | Clone | LWO |
|---|---|---|
| Created by | `clone_object()` from an `/obj/` master | `new_object()` from a `/data/` master |
| Name | `<path>#N`, unique index | `<path>#-1`, shared |
| Object table | First-class entry | None — lives in a holder's dataspace |
| Cross-object reference | Shared identity | Copied at export (value semantics) |
| End of life | `destruct_object()` | Last reference dropped |

LWOs are useful for structured values that need behavior (methods on the LWO's class) but don't warrant the bookkeeping cost of a first-class object — capability tokens, coordinates, time ranges, structured records.

## Destruct: removal

`destruct_object(obj)` removes `obj` from the runtime. The object's dataspace is freed; subsequent references resolve to nil. In-flight calls running against the object at destruct time finish on the destructed dataspace and then the storage is released.

The object manager receives a `destruct` event before the destruct fires. The platform uses this event to invalidate any cached references to the object.

Destructing the master of a class implicitly destructs every clone of that master. Destructing one clone leaves the master and other clones intact.

## Hot reload: recompile in place

Calling `compile_object(path)` against a path that already has a master replaces the master's program in place. The runtime primitive at work is **hot reload** (`docs/runtime-primitives.md` §4); Allen's [2000 description][allen-dgd-2000] names the property: code can be updated in the running runtime without restart.

Key guarantees and limits:

| Property | Behavior |
|---|---|
| In-flight calls on the **old** master | Finish on the old program. The dispatch was bound at call-start; the recompile does not interrupt them. |
| **Next** call after recompile | Dispatches to the new program. |
| Clones of the recompiled master | Their **code** is now the new master's. Their **dataspace** is unchanged (the clone-side variables retain their values). |
| Inheriting children | Their code is NOT automatically recompiled. See Library upgrade below. |
| Atomic context of the recompile | If the recompile errors (syntax, type-check), the old master remains; the platform rolls back as for any failed atomic operation. |

The object manager receives a `compile` event with the inherited path list, allowing it to track the dependency graph for downstream cascade decisions.

Hot reload is the operator-facing form (`compile` verb in `admin_console`); the platform-internal form is the same kfun called from any LPC code with sufficient access. The `examples/hot-reload-demo/` reference application exposes the platform-internal form through an HTTP route: `POST /compile` with new LPC source as the request body calls `compile_object(target_path, body)`, and the next `GET /greet` returns the recompiled program's output.

## Touch: lazy upgrade via call_touch

When a library is recompiled, its inheriting children do NOT automatically pick up the new parent. Their code continues to dispatch through the cached compiled program, which still references the prior parent's slot layout. Two patterns address this:

1. **Eager destruct-and-recompile**: `destruct_object(child); compile_object(child_path)`. Recompiles each child against the new parent. Loses any clone state.
2. **Lazy upgrade via `call_touch`**: schedule each child for an upgrade at next access, preserving state across the upgrade.

`call_touch(obj)` (a host kfun, wrapped at `src/kernel/lib/auto.c::call_touch`) marks `obj` as needing an upgrade. The next call against `obj` is intercepted by the host driver, which:

1. Dispatches a `touch(obj, function)` event to the registered object manager.
2. The object manager calls `obj->_F_touch()` on the marked object.
3. After `_F_touch()` returns, the originally-intended call proceeds.

The application-author hook is `patch()`. Applications that need per-instance migration logic implement this method. `_F_touch()` itself is `nomask` (`src/usr/System/lib/auto.c`) — applications cannot implement or override it; it is the platform's dispatch gate, and its body calls `this_object()->patch()`. The platform guarantees:

- `patch()` runs at most once per `call_touch`.
- `patch()` runs **before** the next call against the object.
- `patch()` runs inside the calling context's atomic envelope — failures roll back.

Why not eager upgrade on library recompile? Two reasons rooted in cost:

1. **Volume.** A widely-inherited library may have thousands of dependent instances. Touching every instance at recompile time exceeds the runtime's tick budget for a single atomic envelope; the recompile fails partway.
2. **Locality.** Lazy upgrade distributes the migration cost across the rate at which instances actually matter. Long-idle objects pay the cost only when accessed.

The trade-off: long-idle objects can accumulate multiple pending upgrades. If the migration logic depends on the most-recent prior version's data shape, a long-idle object may have an upgrade chain that no longer matches. Mitigations:

- **Periodic global touch.** A scheduled `call_out` walks every owner's objects at low frequency (daily, weekly) and ensures each is touched at least once per cycle.
- **Snapshot rotation with explicit touch.** A planned snapshot-and-restore cycle pairs with a touch pass during the restore phase.

**Terminology note**: the application hook here is `patch()`, the same name SkotOS-derived deployments use. `_F_touch()` is not an application-level name to reach for — it is the `nomask` platform gate that dispatches to `patch()`. Operators arriving from a SkotOS background should feel at home with `patch()`.

## Library upgrade: recompile cascade through dependents

When a library at `/usr/MyApp/lib/util.c` recompiles, its dependents in `/usr/MyApp/obj/*` continue to run against the old parent's slot layout until each is either destructed-and-recompiled or marked via `call_touch`. The platform ships the cascade coordination for this:

- The object manager (`/usr/System/sys/objectd.c`) tracks the inheritance and include graph as it builds, via the `compile` event.
- The upgrade daemon (`/usr/System/sys/upgraded.c`) takes one or more source files, walks the graph for every direct and transitive dependent, destructs stale library issues, and recompiles dependents — optionally as one all-or-nothing atomic operation. When a patch tool is supplied, the daemon additionally queues `call_touch` patching so live clones migrate state on next reference instead of being destructed.
- The operator `upgrade [-a|-p] <file> [<file> ...]` verb on the System operator login (`/usr/System/obj/user.c`) drives the flow interactively: `-a` selects the atomic recompile, `-p` supplies the patch tool and runs the `call_touch` patch flow.

One piece the cascade lacks is a stored per-master clone list: clone patching sweeps the object table rather than enumerating a registry (`docs/runtime-primitives.md` §4 and §8 name the gap and the `objregd` port candidate).

## Object-manager events: the lifecycle surface

The platform dispatches lifecycle events to a registered object manager (`driver->set_object_manager(<obj>)`). The shipped manager at `src/usr/System/sys/objectd.c` consumes these events to maintain a registry of live objects, their owners, their inherits, and their includes.

The event surface this kernel actually dispatches:

| Event | Signature | When | Used for |
|---|---|---|---|
| `compile` | `void compile(string owner, string path, mapping source, string inherits...)` | After a successful compile (clonable or library alike) | Registers the object's owner, path, includes, and inherits for the dependency graph |
| `compile_failed` | `void compile_failed(string owner, string path)` | On compile error | Records the failed path |
| `clone` | `void clone(string owner, object obj)` | After a clone is created | Dispatched by the driver, but the shipped `objectd.c` defines no `clone()` handler — the call lands on an undefined function and has no effect |
| `destruct` | `void destruct(string owner, string path)` | Just before destruct | Unregisters the object from the dependency graph |
| `remove_program` | `void remove_program(string owner, string path, int timestamp, int index)` | Last reference to a library's program released | Unregisters library entries from the dependency graph |
| `call_object` | `string call_object(string path)` | Resolving `call_other`'s first (string) argument | Blocks direct `call_other` into a clone master's path or a generated leaf object |
| `inherit_program` | `string inherit_program(string from, string path, int priv)` | Per `inherit` of a `/lib/` path | Rejects inheriting a library path that itself looks like `/obj/`, `/@@@/`, or `/sys/` |
| `include_file` | `mixed include_file(string compiled, string from, string path)` | During compile, per `#include` | Injects the System-tier auto inherit for non-System creators |
| `touch` | `int touch(object obj, string func)` | `call_touch` dispatch interception | Routes to `obj->_F_touch()`, which forwards to the application's `patch()`; preserves "untouched" status if the return value is nonzero |

An application that needs additional behavior typically registers a daemon that **calls into** objectd or wraps its event stream, rather than replacing the shipped manager. Replacement is possible (`driver->set_object_manager(<replacement>)`) but rare; objectd handles the platform's common cases, and replacements risk losing those.

## What this doc does not cover

- The host-driver implementation of the dispatch (the C code inside DGD that fires these events). See the upstream DGD source at <https://github.com/dworkin/dgd>.
- The per-owner resource consumption of each transition (compile costs ticks; destruct frees them; clone consumes object-quota). See `docs/operations.md` Resource limits and the resource_daemon source.
- Per-tier access control on each transition (who can compile what; who can destruct what). See `docs/architecture.md` Capability tiers and the access_daemon source.

## Where to next

- **`docs/lpc-essentials.md`** — the language constructs (`create()`, `inherit`, `static`, `nomask`) the transitions above invoke.
- **`docs/runtime-primitives.md`** §1 (atomicity), §4 (hot reload), §5 (sandboxed code load) — the per-primitive guarantees that bound the lifecycle.
- **`docs/admin-console.md`** Hot-fixing code in production — operator-facing workflow for compile / clone / destruct in a running platform.
- **`docs/application-authoring.md`** — how an application's code consumes the lifecycle (initd, call_touch upgrade, object tracking patterns).
- **`src/kernel/lib/auto.c`** — the authoritative source for `_F_create` and the inheritance-discipline enforcement. `_F_touch` is defined in `src/usr/System/lib/auto.c`.
- **`src/usr/System/sys/objectd.c`** — the shipped object manager's implementation of the event surface above.

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html
