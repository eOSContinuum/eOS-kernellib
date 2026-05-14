<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# LPC Essentials

This document covers the minimum LPC a builder new to the language needs to read and write code in an eOS-kernellib application. The architecture document (`doc/ARCHITECTURE.md`) covers the substrate's tier model and daemon inventory; the application-authoring document (`doc/APPLICATION-AUTHORING.md`) covers tier-E application patterns. This document covers the language itself: how objects are identified, how inheritance composes them, how lifecycle hooks fire, and how atomicity and deferred work behave at the function-call level.

LPC is a C-flavored object-oriented language with three properties that distinguish it from most other languages a builder is likely to have written before: every object lives in a single persistent in-memory database, every kfun call commits or rolls back as a unit, and source files are recompiled into the live runtime without restarting the host. The remainder of this document covers the constructs that follow from those properties.

For language reference depth (full kfun signatures, complete grammar, edge cases) see the upstream DGD documentation at <https://github.com/dworkin/dgd>.

## Path-naming encodes object type

Every LPC source file's path tells the host driver what kind of object the file produces. The driver enforces these conventions at compile time; a misplaced file fails to compile.

| Subdirectory | Object type | Notes |
|---|---|---|
| `/lib/` | Inheritable library, never instantiated | `inherit` targets must live here |
| `/obj/` | Cloneable; clones share the master's program | `clone_object()` instantiates |
| `/data/` | Lightweight object (LWO); stored in another object's dataspace | No identity outside its container |
| `/sys/` | Singleton; one instance, fixed name | Daemons live here |

The convention is structural rather than stylistic. A file at `/usr/MyApp/lib/Helper.c` cannot be cloned; a file at `/usr/MyApp/obj/widget.c` cannot be inherited. The compile-time check catches the mistake before the object reaches the runtime.

## Master and clone

A cloneable program at `/usr/MyApp/obj/widget` has at most one master object: the program itself, with object name equal to its source path. Clones are separate objects with names like `/usr/MyApp/obj/widget#37`. The master and the clones share program code but each clone has its own dataspace.

```
status("/usr/MyApp/obj/widget")     -> master object's status
clone_object("/usr/MyApp/obj/widget")  -> returns a new clone
object_name(clone)                  -> "/usr/MyApp/obj/widget#37"
```

The master can store class-wide data; clones store per-instance data. The master is also a useful place for class-wide registries or factory functions when those need to live with the program rather than in a separate daemon.

## Inheritance

LPC supports multiple inheritance via the `inherit` directive at the top of a source file. There are two forms.

**Anonymous inherit** makes inherited functions callable directly (and chained up to via `::name()`):

```c
inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    /* subclass-specific initialization */
}
```

**Named inherit** prefixes the inherited interface so callers and chain-ups must qualify with the name:

```c
inherit access API_ACCESS;

static void create()
{
    access::create();
    /* subclass-specific initialization */
}
```

The kernel auto at `/kernel/lib/auto.c` is inherited automatically by every compiled object regardless of tier (the host driver's `auto_object` setting points at it). It provides per-object owner and creator identity, kfun wrappers, and access checks. The System auto at `/usr/System/lib/auto.c` extends the kernel auto with substrate-level facilities and is the canonical anonymous-inherit target for application code (the application initd in `examples/http-app/initd.c` shows the pattern).

A subclass's constructor is responsible for chaining up to whichever inherited constructors it depends on. The language does not impose a chain-up order; the subclass picks the order that matches its initialization needs.

## Lifecycle: create()

Every object's first function call is `create()`. The host driver dispatches into the object via the `_F_create` hook configured in the `.dgd` file:

```
create = "_F_create";
```

`_F_create` is implemented in the kernel auto. It sets up owner and creator identity, registers clones with the driver's clone manager, and then invokes the object's `create()` function. An object author writes `create()`; the driver wires the dispatch.

A clone's `create()` runs in the clone's own dataspace. The master's `create()` runs once when the program is first compiled. A pattern common across substrates is to have `create()` examine `object_name(this_object())` and behave differently for the master versus clones, but in this substrate the kernel auto handles the master/clone distinction via its registration logic and most application authors write a single `create()` body that runs on both.

For higher-level application lifecycle (initd's role at boot, domain initialization order, deferred startup via `call_out(0)`) see `doc/APPLICATION-AUTHORING.md`.

## Atomicity

Every LPC function call is its own atomic context by default. The function either runs to completion (the dataspace mutations it performed survive) or errors (the dataspace mutations it performed are rolled back as if the call had never happened). The substrate provides this transparently; an application author does not write begin/commit/rollback.

The closest familiar analogy is a database transaction: the function body is the transaction, errors trigger rollback, successful completion commits. SUBSTRATE-PRIMITIVES.md §1 covers the guarantee at the substrate level.

The `atomic` function modifier deepens this. A function declared `atomic` extends its atomic context across nested function calls within the same call stack:

```c
atomic void transfer(object src, object dst, int amount)
{
    src->withdraw(amount);   /* errors here roll back the next call too */
    dst->deposit(amount);
}
```

Without the modifier, each `->` call commits independently; with the modifier, the whole `transfer` body is one transactional unit. The substrate's eOS-kernellib LPC source uses the implicit per-call atomicity throughout and rarely declares functions `atomic` explicitly because the default already covers the common case.

Atomic contexts have a runtime cost: dataspace mutations are buffered until commit, doubling effective tick consumption inside an atomic context (the "double-tick" cost). For reference depth on the buffering machinery see the upstream DGD documentation.

## Deferred work: call_out

LPC has no threads; concurrency is single-threaded with cooperative scheduling. Long-running work or work that must occur after the current call returns is scheduled via `call_out`:

```c
int call_out(string func, mixed delay, mixed... args);
```

The kfun returns a handle (an integer) and schedules `func` to be called on `this_object()` after `delay` seconds (zero is permitted; the call fires on the next event loop pass after the current call returns). Each fired call_out runs in its own atomic context.

Common uses:

- `call_out("retry", 5, params)` -- retry a failing operation after a delay
- `call_out("flush", 0)` -- defer work to after the current call returns; lets the caller see a consistent state before the deferred work runs
- `call_out("tick", 1)` -- schedule recurring work; the called function re-arms the call_out before returning

A call_out fired from inside an atomic function does not extend the caller's atomic context: the deferred call is its own transaction and runs after the caller commits. This is the substrate's only mechanism for async work and the only way to escape a single atomic context's tick budget.

## Common library types

eOS-kernellib ships a small set of inheritable libraries in `/lib/`. The headers in `src/include/` define the canonical names; an application includes the relevant header and inherits or instantiates as needed.

| Library | Header | Role |
|---|---|---|
| `String`, `StringBuffer` | `<String.h>` | Efficient string accumulation; `StringBuffer` is the right choice for piecewise output construction |
| `KVstore`, `KVnode` | `<KVstore.h>` | Persistent key-value store with structural sharing |
| `Iterable`, `Iterator`, `IntIterator` | `<Iterator.h>` | Iterator protocol for traversing collections |
| `Time`, `GMTime` | `<Time.h>` | Time arithmetic and formatting |
| `Continuation`, `IterativeContinuation`, `ChainedContinuation`, `DelayedContinuation`, `DistContinuation` | `<Continuation.h>` | Composable async-control primitives over `call_out` |

LPC also provides built-in aggregate types that need no library: `mapping` (associative map, `([ key : value ])` literal), arrays (`({ a, b, c })` literal), and lightweight objects (LWOs) for values that should travel inside another object's dataspace. Strings are first-class values.

## Kfuns: built-in and extension-loadable

A kfun is a function provided by the host driver rather than written in LPC. The driver ships a small minimalist core (capped at 256 kfuns by the 1-byte kfun numbering) covering object lifecycle, compilation, atomicity, connections, and basic math / strings / arrays. Additional kfuns can be loaded as host-driver extensions via the `.dgd` configuration's `modules =` mapping; an LPC file calling some kfun cannot tell from the call shape whether the kfun is a host built-in or a dlopen-loaded extension.

Do not assume all kfuns are equally available across deployments. A program that depends on an extension-provided kfun will fail to run on a deployment that does not load that extension. For the architectural pattern see `doc/ARCHITECTURE.md` "Host-driver extensions"; for deployment-time guidance see `doc/OPERATIONS.md`.

## Error handling

A function errors by calling `error("message")` or by triggering a runtime fault (type mismatch, divide-by-zero, missing object). The atomic-context rollback fires on either path. Callers can capture an error with `catch`:

```c
mixed result;
result = catch(some_call());
if (result) {
    /* result is the error string */
}
```

`catch` lets the caller observe the failure without propagating it; the inner call's dataspace mutations still roll back. The substrate's own LPC code rarely uses `catch` (errors propagate to the caller of the kfun, which is what callers usually want); use it sparingly in application code, since silently absorbing an error in a deeply atomic context defeats the rollback's protective purpose.

## Where to next

- **`doc/ARCHITECTURE.md`** -- substrate tier model, daemons, boot sequence, where applications plug in.
- **`doc/APPLICATION-AUTHORING.md`** -- writing a tier-E application: domain layout, initd, owner / access conventions, the object-manager lifecycle, `call_touch` upgrade.
- **`doc/HTTP-APPLICATIONS.md`** -- the canonical HTTP/1 application pattern with a runnable reference at `examples/http-app/`.
- **`doc/SUBSTRATE-PRIMITIVES.md`** -- the substrate's eight runtime primitives (atomicity, capability separation, persistent state, hot reload, sandboxed code load, async events, multi-agent coherence, state introspection) covered as substrate guarantees rather than language constructs.
- **DGD upstream reference** at <https://github.com/dworkin/dgd> -- full grammar and kfun reference.
