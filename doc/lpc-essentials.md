<!-- SPDX-License-Identifier: BSD-2-Clause-Patent -->

# LPC Essentials

LPC is the programming language used by the [DGD] driver and the layers built on it, including eOS-kernellib. This document is an orientation: enough to read LPC code without surprise, enough mental model to know what to look up next. The authoritative reference is [LPC.md] in [dworkin/lpc-doc] (DGD pins commit `403cd0b` as a submodule). LPC.md is a formal specification, dense and reference-shaped; this document is the bridge that makes the spec readable.

For depth on a specific construct, follow the section citations into LPC.md. For the kfun catalog, see [dworkin/lpc-doc/kfun/]. For how LPC fits into eOS-kernellib's substrate (tier model, daemons, the auto-inheritance chain that hands every object its identity), see `doc/architecture.md`. For the patterns an application author writes on top of the substrate, see `doc/application-authoring.md`.

**Audience**: a programmer new to LPC; familiar with at least one C-family language (C, C++, Java, Go, or similar); reading this to gain enough LPC literacy to read substrate code and write application code on top. The formal language reference at [dworkin/lpc-doc] is authoritative; this document is the bridge to it.

## What is familiar

A reader from C, C++, Java, or Go will recognize most of LPC's surface. Curly braces, semicolons, `if` / `while` / `for` / `switch`, `int` / `float` / `string` declarations, function definitions with parameter lists, the C preprocessor (`#include`, `#define`, `#ifdef`). Source files compile to objects; functions take and return values; expressions and operators look the way you expect. LPC.md §3.1 covers lexical elements, §3.2 expressions, §3.5 statements, §3.7 preprocessing.

## What is surprising

Five properties make LPC behave differently from most other languages a builder is likely to have written before. None are subtle once seen; all are subtle if missed.

**Objects live in a persistent in-memory database.** Every "object" in LPC is a value in the host driver's persistent store. When the substrate restarts, the object graph is reconstituted from a snapshot file and resumes where it left off. There is no separate "save to disk" step in normal operation; persistence is the default. This is called *orthogonal persistence* in the systems literature.

**Every function call is its own atomic context by default.** If the function body errors, every dataspace mutation it performed is rolled back as if the call had never happened. A function declared with the `atomic` modifier extends that atomic context across nested calls within the same call stack. The application author writes no `BEGIN TRANSACTION` / `ROLLBACK`; the host runtime enforces it.

**Source files are recompiled into the live runtime.** Calling `compile_object("/usr/MyApp/obj/widget", new_source)` replaces the master object's program with the new source. Existing references to the old master continue to function for the in-flight call; subsequent calls dispatch to the new version. There is no separate "deploy" step.

**No manual memory management.** No `malloc` / `free`, no `delete`. Objects are reference-counted by the host driver and garbage-collected when references drop to zero. Programs do not own memory; the host owns it.

**Cross-object calls and inherited calls have their own operators.** `obj->func(arg)` calls `func` on a different object (the call traverses the kfun layer and goes through access checks). `::func()` calls the inherited version of `func` from this object's parent class. `name::func()` calls the inherited version when the inherit was given a name. A bare `func()` call is local to the current object.

## Types and values

LPC.md §3.4.2 lists eight types:

| Type | Holds | Notes |
|---|---|---|
| `nil` | The "no value" value | Default for uninitialized variables |
| `int` | 32-bit signed integer | -2147483648 through 2147483647 |
| `float` | Double-precision floating point | IEEE 754 |
| `string` | Sequence of characters | Immutable; first-class value |
| `object` | Reference to another object | Master or clone |
| `mapping` | Key-value associative map | Literal: `([ key : value ])` |
| `mixed` | Any of the above | Used for dynamic typing |
| `void` | No value | Function return type only |

Two composite shapes are not in the type list but are first-class:

- **Arrays** — written `({ a, b, c })`. Heterogeneous (an array can hold mixed types). `arr[0]` indexes; `arr[1..3]` slices.
- **Lightweight objects (LWOs)** — value-shaped objects that travel inside another object's dataspace rather than living independently. Cloned with `new` (different from `clone_object()`); the lifetime is tied to the containing object's reference.

`int` and `float` arithmetic does not auto-coerce; `1 + 1.0` is a type error. Use `(float) 1` or `(int) 1.0` to convert explicitly.

## Type modifiers

LPC.md §3.4.3 defines four modifiers that prefix a function or variable declaration:

| Modifier | Effect |
|---|---|
| `private` | Only callable from within the same object |
| `static` | On a function: callable only from this object and its inheriting children. On a variable: not saved to the snapshot |
| `nomask` | On a function: cannot be redefined by a subclass |
| `atomic` | On a function: extends the per-call atomic context across nested calls; the function body either commits wholly or rolls back wholly |

`varargs` is also a keyword (LPC.md §3.1.4) prefixing a function whose parameter list is variable-length.

Two consequences of `static` on variable declarations are worth flagging because they connect LPC to the substrate's persistence semantics:

1. A `static` global variable is **excluded from `save_object()`** — the per-object save mechanism. LPC.md §3.4.3 names this directly: "For variables that are declared as static, the variable will be defined globally for that object, and it will not be saved when save_object is called." Full statedumps (the substrate's `dump_state` mechanism) capture the entire in-memory image, so a `static` variable's current value is preserved across statedump-restore boots regardless of the modifier — `static` affects the per-object save format, not the substrate-wide snapshot. See [the 2003 DGD-list discussion of the keyword][croes-static-2003] for the runtime semantics in detail.
2. A non-`static` global variable is captured in `save_object()` output and is therefore portable across the per-object save/restore cycle. It is also captured in statedumps. Use non-`static` for state you want both in the substrate-wide snapshot and in an ad-hoc per-object save file; use `static` for state you want only in the in-memory image.

[croes-static-2003]: https://mail.dworkin.nl/pipermail/dgd/2003-April/003390.html

The `atomic` modifier carries two trade-offs the spec calls out (LPC.md §3.4.3):

- **File operations forbidden inside `atomic`.** Renaming, writing, or reading files inside an atomic function is rejected by the host runtime. The atomicity guarantee covers in-memory dataspace, not external state.
- **Double-tick cost.** Atomic functions consume tick budget at twice the rate of non-atomic functions, because every dataspace mutation is buffered until commit.

## Functions and how they are called

A function defined inside an object is called four ways depending on where the call comes from:

```c
foo();              /* local: this_object()'s own foo */
obj->foo();         /* cross-object: call foo on another object via kfun */
::foo();            /* inherited: parent class's foo */
parent::foo();      /* inherited (named): foo from inherit named "parent" */
```

Cross-object calls (`->`) go through the host runtime's kfun layer and are subject to per-tier access checks. The inherited call operators (`::` and `name::`) are language constructs and dispatch directly into the inherited program.

Some functions are not defined in any LPC source — they are *kfuns*, provided by the host driver. From the calling site, kfuns look identical to local functions: `compile_object("/usr/MyApp/obj/widget")` is a kfun call, but the syntax does not say so. The catalog lives in [dworkin/lpc-doc/kfun/]. Note that kfuns split into two surfaces: the small minimalist core the host driver ships, and the optional extension modules a deployment may load (covered in `doc/architecture.md` "Host-driver extensions" and `doc/operations.md` "Loading host-driver extensions").

## Inheritance

LPC.md §3.6 specifies the `inherit` directive in two forms:

```c
inherit "FILENAME";              /* anonymous */
inherit Identifier "FILENAME";   /* named */
```

The anonymous form makes inherited functions callable as if they were local: `foo()` calls the inherited `foo` and `::foo()` chains into the inherited version explicitly. The named form prefixes the inherited interface so calls qualify with the name: `Identifier::foo()`.

Multiple inheritance is supported. The kernel auto at `/kernel/lib/auto.c` is inherited automatically by every compiled object regardless of any explicit `inherit` directives; the host driver's `auto_object` configuration setting points at it. The System auto at `/usr/System/lib/auto.c` extends the kernel auto and is the canonical anonymous-inherit target for application code (the application initd in `examples/http-app/initd.c` shows the pattern).

The `private` keyword may prefix `inherit` to restrict the inherited members to access by this object only.

## Lifecycle: create()

LPC.md does not have a dedicated lifecycle section; `create()` is shown as an example function. The host driver invokes a configured "create" hook on every object's first function call — the hook name comes from the `.dgd` config's `create` field. eOS-kernellib's setting is:

```text
create = "_F_create";
```

`_F_create` is implemented in the kernel auto. It sets up owner and creator identity, registers clones with the driver's clone manager, and then invokes the object's own `create()` function. An application author writes `create()`; the kernel auto wires the dispatch.

```c
inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    /* subclass-specific initialization */
}
```

Calling `::create()` chains into the parent's create. The `static` keyword keeps `create` callable only from this object and its children — a convention that prevents external callers from re-running the constructor.

For deeper substrate dispatch (how the kernel auto handles master vs clone, how owner identity is set, how the clone registry is maintained), see `doc/architecture.md` "Auto-inheritance pattern."

## Atomicity

Every function call is its own atomic context by default. The function either runs to completion (its dataspace mutations survive) or errors (its dataspace mutations are rolled back as if the call had never happened). Christopher Allen's [2000 description of DGD][allen-dgd-2000] frames the property historically: "atomic function calls allow full system-state rollback in the event of a run-time error."

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html

The `atomic` modifier extends this across nested calls:

```c
atomic void transfer(object src, object dst, int amount)
{
    src->withdraw(amount);   /* errors here roll back the next call too */
    dst->deposit(amount);
}
```

Without the modifier each `->` call commits independently; with the modifier the whole `transfer` body is one transactional unit. The substrate's eOS-kernellib LPC source uses the implicit per-call atomicity throughout and rarely declares functions `atomic` explicitly because the default already covers the common case.

For the substrate-level guarantee and its open empirical questions under host-driver extensions, see `doc/substrate-primitives.md` §1.

## Deferred work: call_out

LPC has no threads. Concurrency is single-threaded with cooperative scheduling. Long-running work or work that must occur after the current call returns is scheduled via the `call_out` kfun:

```c
int call_out(string func, mixed delay, mixed... args);
```

Returns a handle. Schedules `func` to be called on `this_object()` after `delay` seconds (zero is permitted; the call fires on the next event-loop pass after the current call returns). Each fired call_out runs in its own atomic context.

Common shapes:

- `call_out("retry", 5, params)` — retry a failing operation after a delay
- `call_out("flush", 0)` — defer work to after the current call returns; the caller commits a consistent state before the deferred work runs
- `call_out("tick", 1)` — schedule recurring work; the called function re-arms the call_out before returning

A call_out fired from inside an `atomic` function does not extend the caller's atomic context — the deferred call is its own transaction, runs after the caller commits. This is the substrate's only mechanism for async work and the only way to escape a single atomic context's tick budget.

## Error handling

A function errors by calling `error("message")` (an LPC.md-listed kfun) or by triggering a runtime fault (type mismatch, divide-by-zero, missing object). The atomic-context rollback fires on either path. Callers can capture an error with `catch`:

```c
mixed result;
result = catch(some_call());
if (result) {
    /* result is the error string */
}
```

`catch` lets the caller observe the failure without propagating it. The inner call's dataspace mutations still roll back. The substrate's own LPC code rarely uses `catch` (errors propagate to the caller of the kfun, which is what callers usually want); use it sparingly in application code, since silently absorbing an error in a deeply atomic context defeats the rollback's protective purpose.

## A short worked example

A counter object with a deliberate-failure increment that demonstrates atomic rollback:

```c
inherit "/usr/System/lib/auto";

private int counter;

static void create()
{
    ::create();
    counter = 0;
}

int query() { return counter; }

void increment_with_failure()
{
    counter += 1;
    error("deliberate failure");
}
```

Calling `query()` returns the current counter. Calling `increment_with_failure()` enters the atomic context, mutates `counter` to the next value, then errors. The host runtime rolls back the mutation; the next `query()` returns the same value as before. No application code participates in the rollback; the substrate's per-call atomicity does the work.

## Where to next

- **[LPC.md]** — the formal language spec, pinned by DGD at commit `403cd0b`. §3.1 lexical elements, §3.2 expressions, §3.4 declarations and types, §3.5 statements, §3.6 inheritance, §3.7 preprocessing.
- **[dworkin/lpc-doc/kfun/]** — the kfun catalog. Every host-provided function the language calls.
- **`doc/architecture.md`** — the substrate's tier model, daemons, the auto-inheritance chain, the host-driver extension surface.
- **`doc/application-authoring.md`** — what writing a tier-E application on top of this substrate looks like: domain layout, initd, owner / access, the object-manager lifecycle, `call_touch` upgrade.
- **`doc/kernel-libraries.md`** — the inheritable libraries shipped under `src/lib/` (String, StringBuffer, KVstore, Iterator, Continuation variants, Time).
- **`doc/substrate-primitives.md`** — per-primitive foundation-and-status statement (atomicity, persistent state, hot reload, capability separation, etc.).

[DGD]: https://github.com/dworkin/dgd
[dworkin/lpc-doc]: https://github.com/dworkin/lpc-doc
[LPC.md]: https://github.com/dworkin/lpc-doc/blob/master/LPC.md
[dworkin/lpc-doc/kfun/]: https://github.com/dworkin/lpc-doc/tree/master/kfun
