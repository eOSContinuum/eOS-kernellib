# Code Lifecycle

LPC code in the platform moves through these states: source becomes a master, a master spawns clones, `create()` runs, recompilation replaces a running master in place, `call_touch` schedules lazy upgrades, and `destruct_object` removes objects from the runtime. The object-manager event surface lets the platform observe each transition. The sections below cover each in turn.

For the LPC language constructs these transitions invoke (`inherit`, `create()`, `static`, `nomask`), see `doc/lpc-essentials.md`. For the per-primitive runtime guarantees that bound these transitions (atomicity, hot reload, capability separation), see `doc/runtime-primitives.md`. For the operator surface that drives lifecycle transitions interactively, see `doc/admin-console.md`.

**Audience**: an LPC author or operator reasoning about how source becomes a running object, how recompilation propagates to existing instances, how `call_touch` schedules lazy upgrades, and how the platform observes lifecycle transitions; assumes `doc/lpc-essentials.md` for language constructs and `doc/architecture.md` for the structural model.

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

Compilation runs in atomic context. If the source has a syntax error or fails type-check, the compile errors and the platform's prior state (including any prior master at `path`) is unchanged. The atomicity guarantee (`doc/runtime-primitives.md` §1) covers the compile transition itself: either the new program installs cleanly or the runtime rolls back as if the call never happened.

After successful compile, the master sits idle. Its `create()` does not run until the first function call against the master. The object manager (see below) receives a `compile` event with the master object and the inherited paths; this is the platform's hook for tracking the dependency graph.

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

- The master must be a clonable. By platform convention this means a path under `/usr/.../obj/` rather than `/usr/.../lib/`. The kernel's `clone_object` kfun enforces the `/obj/` rule.
- Clones cannot be cloned. `clone_object(clone)` errors.
- A clone's owner is the **clone-time caller's owner**, not the master's owner. Two clones of the same master can have different owners if their callers do.

The object manager receives a `clone` event with the clone object before its `create()` runs.

## LWO instantiation: new_object

`new_object(master)` instantiates an LWO from a `/data/` master. LWOs differ from clones:

- **Identity**: a clone has its own object reference; an LWO is a value, equality-compared by structure.
- **Storage**: a clone lives in the object table independently; an LWO lives in the dataspace of whichever object holds the reference.
- **Lifetime**: a clone exists until `destruct_object()`; an LWO is freed when no in-image reference holds it.

LWOs are useful for structured values that need behavior (methods on the LWO's class) but don't warrant the bookkeeping cost of a first-class object — coordinates, time ranges, structured records.

## Destruct: removal

`destruct_object(obj)` removes `obj` from the runtime. The object's dataspace is freed; subsequent references resolve to nil. In-flight calls running against the object at destruct time finish on the destructed dataspace and then the storage is released.

The object manager receives a `destruct` event before the destruct fires. The platform uses this event to invalidate any cached references to the object.

Destructing the master of a class implicitly destructs every clone of that master. Destructing one clone leaves the master and other clones intact.

## Hot reload: recompile in place

Calling `compile_object(path)` against a path that already has a master replaces the master's program in place. The runtime primitive at work is **hot reload** (`doc/runtime-primitives.md` §4); Allen's [2000 description][allen-dgd-2000] names the property: code can be updated in the running runtime without restart.

Key guarantees and limits:

| Property | Behavior |
|---|---|
| In-flight calls on the **old** master | Finish on the old program. The dispatch was bound at call-start; the recompile does not interrupt them. |
| **Next** call after recompile | Dispatches to the new program. |
| Clones of the recompiled master | Their **code** is now the new master's. Their **dataspace** is unchanged (the clone-side variables retain their values). |
| Inheriting children | Their code is NOT automatically recompiled. See Library upgrade below. |
| Atomic context of the recompile | If the recompile errors (syntax, type-check), the old master remains; the platform rolls back as for any failed atomic operation. |

The object manager receives a `compile` event (or `compile_lib` for libraries) with the inherited path list, allowing it to track the dependency graph for downstream cascade decisions.

Hot reload is the operator-facing form (`compile` verb in `admin_console`); the platform-internal form is the same kfun called from any LPC code with sufficient access.

## Touch: lazy upgrade via call_touch

When a library is recompiled, its inheriting children do NOT automatically pick up the new parent. Their code continues to dispatch through the cached compiled program, which still references the prior parent's slot layout. Two patterns address this:

1. **Eager destruct-and-recompile**: `destruct_object(child); compile_object(child_path)`. Recompiles each child against the new parent. Loses any clone state.
2. **Lazy upgrade via `call_touch`**: schedule each child for an upgrade at next access, preserving state across the upgrade.

`call_touch(obj)` (a host kfun, wrapped at `src/kernel/lib/auto.c::call_touch`) marks `obj` as needing an upgrade. The next call against `obj` is intercepted by the host driver, which:

1. Dispatches a `touch(obj, function)` event to the registered object manager.
2. The object manager calls `obj->_F_touch()` on the marked object.
3. After `_F_touch()` returns, the originally-intended call proceeds.

The application-author hook is `_F_touch()`. Applications that need per-instance migration logic implement this method. The platform guarantees:

- `_F_touch()` runs at most once per `call_touch`.
- `_F_touch()` runs **before** the next call against the object.
- `_F_touch()` runs inside the calling context's atomic envelope — failures roll back.

Why not eager upgrade on library recompile? Two reasons rooted in cost:

1. **Volume.** A widely-inherited library may have thousands of dependent instances. Touching every instance at recompile time exceeds the runtime's tick budget for a single atomic envelope; the recompile fails partway.
2. **Locality.** Lazy upgrade distributes the migration cost across the rate at which instances actually matter. Long-idle objects pay the cost only when accessed.

The trade-off: long-idle objects can accumulate multiple pending upgrades. If the migration logic depends on the most-recent prior version's data shape, a long-idle object may have an upgrade chain that no longer matches. Mitigations:

- **Periodic global touch.** A scheduled `call_out` walks every owner's objects at low frequency (daily, weekly) and ensures each is touched at least once per cycle.
- **Snapshot rotation with explicit touch.** A planned snapshot-and-restore cycle pairs with a touch pass during the restore phase.

**Terminology note**: SkotOS-derived deployments often call the application hook `patch()` rather than `_F_touch()`. The platform dispatch is the same; the name is convention. Operators arriving from a SkotOS background should expect the eOS-kernellib codebase to use `_F_touch()`.

## Library upgrade: recompile cascade through dependents

The platform ships no automatic recompile-with-dependents mechanism. When a library at `/usr/MyApp/lib/util.c` recompiles, its dependents in `/usr/MyApp/obj/*` continue to run against the old parent's slot layout until each is either destructed-and-recompiled or marked via `call_touch`.

SkotOS and some kernellib deployments ship a `progdb` daemon (a port candidate listed in `doc/runtime-primitives.md` §4 Extensions) that:

- Tracks the inheritance graph as it builds via `compile` and `compile_lib` events.
- Receives a recompile signal for a library.
- Cascades the recompile to every direct and transitive dependent.
- Optionally call_touches dependents instead of destruct-recompile, preserving state.

eOS-kernellib's current shipped surface stops at the events: `compile_lib` is fired on the object manager with the inheritance graph, and `call_touch` is available for lazy upgrade. The cascade coordination is an application-layer concern unless a `progdb`-style daemon is added.

The SkotOS Wiztool's `upgrade` verb is the operator-facing form of this cascade in deployments that ship `progdb`. eOS-kernellib's `admin_console` does not ship `upgrade`; operators perform cascade manually via `code "/path/to/progdb"->upgrade(...)` if their deployment has a `progdb`-equivalent, or via a sequence of `destruct` + `compile` if it does not.

## Object-manager events: the lifecycle surface

The platform dispatches lifecycle events to a registered object manager (`driver->set_object_manager(<obj>)`). The shipped manager at `src/usr/System/sys/objectd.c` consumes these events to maintain a registry of live objects, their owners, their inherits, and their includes.

The event surface (a thin restatement of the kernellib-doc convention):

| Event | Signature | When | Used for |
|---|---|---|---|
| `compiling` | `void compiling(string path)` | Just before compile | Pre-compile interception (security, validation) |
| `compile` | `void compile(string owner, object obj, string *source, string inherited...)` | After successful compile of a clonable | Inheritance graph maintenance; create-call preparation |
| `compile_lib` | `void compile_lib(string owner, string path, string *source, string inherited...)` | After successful compile of a library | Library inheritance graph; cascade target tracking |
| `compile_failed` | `void compile_failed(string owner, string path)` | On compile error | Cleanup of any pre-compile registration |
| `clone` | `void clone(string owner, object obj)` | Just before clone's `create(1)` | Clone tracking; per-master clone-list maintenance |
| `destruct` | `void destruct(string owner, object obj)` | Just before destruct | Invalidate cached references |
| `destruct_lib` | `void destruct_lib(string owner, string path)` | Just before library destruct | Inheritance graph cleanup |
| `remove_program` | `void remove_program(string owner, string path, int timestamp, int index)` | Last reference released | Garbage collection of inherit graph entries |
| `include_file` | `mixed include_file(string compiled, string from, string path)` | During compile, per `#include` | Include translation; security; path rewriting |
| `touch` | `int touch(object obj, string function)` | call_touch dispatch interception | Routes to `_F_touch()`; preserves "untouched" status if return value is nonzero |
| `forbid_call` | `int forbid_call(string path)` | Per `call_other` | Block cross-object calls to specific paths |
| `forbid_inherit` | `int forbid_inherit(string from, string path, int priv)` | Per inherit | Block specific inheritance patterns |

An application that needs additional behavior typically registers a daemon that **calls into** objectd or wraps its event stream, rather than replacing the shipped manager. Replacement is possible (`driver->set_object_manager(<replacement>)`) but rare; objectd handles the platform's common cases, and replacements risk losing those.

## What this doc does not cover

- The host-driver implementation of the dispatch (the C code inside DGD that fires these events). See the upstream DGD source at <https://github.com/dworkin/dgd>.
- The per-owner resource consumption of each transition (compile costs ticks; destruct frees them; clone consumes object-quota). See `doc/operations.md` Resource limits and the resource_daemon source.
- Per-tier access control on each transition (who can compile what; who can destruct what). See `doc/architecture.md` Capability tiers and the access_daemon source.

## Where to next

- **`doc/lpc-essentials.md`** — the language constructs (`create()`, `inherit`, `static`, `nomask`) the transitions above invoke.
- **`doc/runtime-primitives.md`** §1 (atomicity), §4 (hot reload), §5 (sandboxed code load) — the per-primitive guarantees that bound the lifecycle.
- **`doc/admin-console.md`** Hot-fixing code in production — operator-facing workflow for compile / clone / destruct in a running platform.
- **`doc/application-authoring.md`** — how an application's code consumes the lifecycle (initd, call_touch upgrade, object tracking patterns).
- **`src/kernel/lib/auto.c`** — the authoritative source for `_F_create`, `_F_touch`, and the inheritance-discipline enforcement.
- **`src/usr/System/sys/objectd.c`** — the shipped object manager's implementation of the event surface above.

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html
