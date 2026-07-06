# LPC essentials

LPC is the programming language used by the [DGD] driver and the layers built on it, including eOS-kernellib. This document is an orientation: enough to read LPC code without surprise, enough mental model to know what to look up next. The authoritative reference is [LPC.md] in [dworkin/lpc-doc] (DGD pins commit `403cd0b` as a submodule). LPC.md is a formal specification, dense and reference-shaped; this document is the bridge that makes the spec readable.

For depth on a specific construct, follow the section citations into LPC.md. For the kfun catalog, see [dworkin/lpc-doc/kfun/]. For how LPC fits into eOS-kernellib's runtime platform (tier model, daemons, the auto-inheritance chain that hands every object its identity), see `docs/architecture.md`. For the patterns an application author writes on top of the platform, see `docs/application-authoring.md`.

**Audience**: a programmer new to LPC; familiar with at least one C-family language (C, C++, Java, Go, or similar) — or with a dynamic language (Python, TypeScript, Ruby), in which case read the "If you come from dynamic languages" section first. Reading this to gain enough LPC literacy to read platform code and write application code on top. The formal language reference at [dworkin/lpc-doc] is authoritative; this document is the bridge to it.

## What is familiar

A reader from C, C++, Java, or Go will recognize most of LPC's surface. Curly braces, semicolons, `if` / `while` / `for` / `switch`, `int` / `float` / `string` declarations, function definitions with parameter lists, the C preprocessor (`#include`, `#define`, `#ifdef`). Source files compile to objects; functions take and return values; expressions and operators look the way you expect. LPC.md §3.1 covers lexical elements, §3.2 expressions, §3.5 statements, §3.7 preprocessing.

## What is surprising

Five properties make LPC behave differently from most other languages a builder is likely to have written before. None are subtle once seen; all are subtle if missed.

**Objects live in a persistent in-memory database.** Every "object" in LPC is a value in the host driver's persistent store. When the platform restarts, the object graph is reconstituted from a snapshot file and resumes where it left off. There is no separate "save to disk" step in normal operation; persistence is the default. This is called *orthogonal persistence* in the systems literature.

**The `atomic` modifier binds a function body into a transactional unit.** A function declared `atomic` either runs to completion (its dataspace mutations commit) or errors (its mutations are rolled back as if the call had never happened). Nested calls run inside the caller's atomic context. The application author writes no `BEGIN TRANSACTION` / `ROLLBACK`; the host runtime enforces it. Functions declared without the modifier do not get function-body rollback — a caught error anywhere up the stack absorbs the mutations the failing call performed. The `examples/atomic-demo/` reference application demonstrates the rollback empirically; the runtime's `[atomic]` annotation in the error trace marks the rollback firing.

**Source files are recompiled into the live runtime.** Calling `compile_object("/usr/MyApp/obj/widget", new_source)` replaces the master object's program with the new source. Existing references to the old master continue to function for the in-flight call; subsequent calls dispatch to the new version. There is no separate "deploy" step.

**No manual memory management.** No `malloc` / `free`, no `delete`. Masters and clones persist until explicitly removed with `destruct_object()`; only lightweight objects (LWOs, see below) are reference-counted and deallocated when the last reference drops. Programs do not own memory; the host owns it.

**Cross-object calls and inherited calls have their own operators.** `obj->func(arg)` calls `func` on a different object (the call traverses the kfun layer and goes through access checks). `::func()` calls the inherited version of `func` from this object's parent class. `name::func()` calls the inherited version when the inherit was given a name. A bare `func()` call is local to the current object.

## If you come from dynamic languages

The sections above assume C-family instincts. Arriving from Python, JavaScript/TypeScript, Ruby, or Lua, four adjustments matter most:

- **Types are declared and checked.** Variables, parameters, and returns carry declared types, and the platform compiles with strict typechecking (the `typechecking = 2` line in the `.dgd` configuration). `mixed` is the escape hatch when a value is genuinely dynamic — it is the exception in platform code, not the default. There is no implicit numeric coercion: `1 + 1.0` is a compile error, not `2.0`.
- **Identity semantics differ by kind.** Objects (masters, clones) are true references with stable identity — pass one around and every holder sees the same object. Strings are immutable values. Arrays and mappings behave like Python lists/dicts *within one object*, but they do not make a shared-mutable channel *between* objects: a structure handed to another object becomes that object's own copy once the runtime processes the export (never mid-execution, so within a single call chain the alias still holds). Cross-object shared mutable state therefore lives in an object, reached through its functions — not in a list two objects both captured. The same rule covers LWOs ([code-lifecycle.md](code-lifecycle.md) LWO instantiation).
- **Functions are not first-class values.** There are no closures or lambdas to store in variables; dispatch is by function name — `obj->func(...)` and the kfun reflection surface (`function_object`). Where a dynamic-language design would store callbacks, platform designs store object references (call a known function on it), schedule by name (`call_out("fn", ...)`), or bind scripts to properties (the Merry layer). Errors follow the same shape: `error("message")` raises a string, `catch` observes it — there is no exception class hierarchy.
- **The scripting layer is the *more* restricted layer, not the looser one.** Merry looks like the scripting surface — no local variable declarations, inline composition, TCL-ish feel — and arriving from a dynamic language it is tempting to treat it as "the Python of the platform." Invert that: Merry is a sandboxed *subset*. No `->` calls (use `Call()`), no file or network kfuns, no code loading, a 51-entry deny list ([merry-language.md](merry-language.md)). Dynamic-language comfort helps with Merry's idioms; the *capabilities* run the other way — full LPC is the expressive layer, Merry is the contained one.

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
- **Lightweight objects (LWOs)** — value-shaped objects that travel inside another object's dataspace rather than living independently. Created with `new_object()` (distinct from `clone_object()`); deallocated when the last reference is dropped. Consolidated treatment in `docs/code-lifecycle.md` LWO instantiation.

`int` and `float` arithmetic does not auto-coerce; `1 + 1.0` is a type error. Use `(float) 1` or `(int) 1.0` to convert explicitly.

## Type modifiers

LPC.md §3.4.3 defines four modifiers that prefix a function or variable declaration:

| Modifier | Effect |
|---|---|
| `private` | Only callable from within the same object |
| `static` | On a function: callable only from this object and its inheriting children. On a variable: not saved to the snapshot |
| `nomask` | On a function: cannot be redefined by a subclass |
| `atomic` | On a function: the body either commits wholly or rolls back wholly. Required for function-body rollback; nested calls run inside the caller's atomic context |

`varargs` is also a keyword (LPC.md §3.1.4) prefixing a function whose parameter list is variable-length.

Two consequences of `static` on variable declarations are worth flagging because they connect LPC to the platform's persistence semantics:

1. A `static` global variable is **excluded from `save_object()`** — the per-object save mechanism. LPC.md §3.4.3 names this directly: "For variables that are declared as static, the variable will be defined globally for that object, and it will not be saved when save_object is called." Full statedumps (the platform's `dump_state` mechanism) capture the entire in-memory image, so a `static` variable's current value is preserved across statedump-restore boots regardless of the modifier — `static` affects the per-object save format, not the platform-wide snapshot. See [the 2003 DGD-list discussion of the keyword][croes-static-2003] for the runtime semantics in detail.
2. A non-`static` global variable is captured in `save_object()` output and is therefore portable across the per-object save/restore cycle. It is also captured in statedumps. Use non-`static` for state you want both in the platform-wide snapshot and in an ad-hoc per-object save file; use `static` for state you want only in the in-memory image.

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

Some functions are not defined in any LPC source — they are *kfuns*, provided by the host driver. From the calling site, kfuns look identical to local functions: `compile_object("/usr/MyApp/obj/widget")` is a kfun call, but the syntax does not say so. The catalog lives in [dworkin/lpc-doc/kfun/]. Note that kfuns split into two surfaces: the small minimalist core the host driver ships, and the optional extension modules a deployment may load (covered in `docs/architecture.md` "Host-driver extensions" and `docs/operations.md` "Loading host-driver extensions").

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

For deeper platform dispatch (how the kernel auto handles master vs clone, how owner identity is set, how the clone registry is maintained), see `docs/architecture.md` "Auto-inheritance pattern."

## Atomicity

A function declared `atomic` runs as one transactional unit: the body either runs to completion (its dataspace mutations commit) or errors (its mutations are rolled back as if the call had never happened). Nested calls execute inside the caller's atomic context, so the rollback covers them too. Christopher Allen's [2000 description of DGD][allen-dgd-2000] frames the property historically: "atomic function calls allow full system-state rollback in the event of a run-time error."

[allen-dgd-2000]: https://mail.dworkin.nl/pipermail/mud-dev-archive/2000-April/013083.html

A typical pattern:

```c
atomic void transfer(object src, object dst, int amount)
{
    src->withdraw(amount);   /* errors here roll back the next call too */
    dst->deposit(amount);
}
```

The `atomic` modifier makes the whole `transfer` body one transactional unit. If `dst->deposit` errors, `src->withdraw`'s mutation is rolled back as part of the same envelope.

Functions declared without `atomic` do not get function-body rollback. If such a function mutates dataspace and then errors, and a caller anywhere up the stack catches the error, the mutation persists. The rollback fires only when the call stack hits an `atomic` ancestor — without one, the mutation is committed at the point of mutation, not the point of return.

The host runtime annotates the boot log with `[atomic]` on the error trace when the rollback fires. The absence of the annotation on an error trace tells the operator the rollback did not engage. The `examples/atomic-demo/` reference application demonstrates both the rollback annotation and the empirical evidence: a counter declared with `atomic void increment_with_failure()` whose mutation rolls back across a deliberate-failure POST, observable through a three-step HTTP probe.

For the platform-level guarantee and its open empirical questions under host-driver extensions, see `docs/runtime-primitives.md` §1.

## Deferred work: call_out

LPC has no threads. Concurrency is single-threaded with cooperative scheduling. Long-running work or work that must occur after the current call returns is scheduled via the `call_out` kfun:

```c
int call_out(string func, mixed delay, mixed... args);
```

Returns a handle. Schedules `func` to be called on `this_object()` after `delay` seconds (zero is permitted; the call fires on the next event-loop pass after the current call returns). Each fired call_out runs as an ordinary, non-atomic call; it gets function-body rollback only if `func` itself is declared `atomic`.

Common shapes:

- `call_out("retry", 5, params)` — retry a failing operation after a delay
- `call_out("flush", 0)` — defer work to after the current call returns; the caller commits a consistent state before the deferred work runs
- `call_out("tick", 1)` — schedule recurring work; the called function re-arms the call_out before returning

A call_out fired from inside an `atomic` function does not extend the caller's atomic context — the deferred call runs after the caller commits, as an ordinary, non-atomic call unless the called function is itself declared `atomic`. This is the platform's only mechanism for async work and the only way to escape a single atomic context's tick budget.

## Error handling

A function errors by calling `error("message")` (an LPC.md-listed kfun) or by triggering a runtime fault (type mismatch, divide-by-zero, missing object). The atomic-context rollback fires on either path *when an `atomic` ancestor is on the call stack*. Callers can capture an error with `catch`:

```c
mixed result;
result = catch(some_call());
if (result) {
    /* result is the error string */
}
```

`catch` lets the caller observe the failure without propagating it. If `some_call` was declared `atomic` (or runs inside an atomic ancestor that has not yet returned), its dataspace mutations roll back before the `catch` block runs. If `some_call` was not atomic and no atomic ancestor is on the stack, `catch` absorbs the error and the mutations persist — the call left dataspace in whatever intermediate state it reached before erroring.

The platform's own LPC code rarely uses `catch` (errors propagate to the caller of the kfun, which is what callers usually want); use it sparingly in application code, since silently absorbing an error in a deeply atomic context defeats the rollback's protective purpose.

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

atomic void increment_with_failure()
{
    counter += 1;
    error("deliberate failure");
}
```

The `atomic` modifier on `increment_with_failure` is what makes the rollback work. Calling `query()` returns the current counter. Calling `increment_with_failure()` enters the atomic envelope, mutates `counter` to the next value, then errors. The host runtime rolls back the mutation; the next `query()` returns the same value as before. No application code participates in the rollback.

Drop the modifier and the same probe shows the mutation persisting past the error: the function runs without an atomic envelope, the mutation lands on dataspace, the error fires, and any catch up the stack absorbs the error without triggering a rollback.

The `examples/atomic-demo/` reference application wraps this counter in an HTTP/1 application server with a smoke script that exercises the three-step probe (`GET /counter` → `POST /increment-with-failure` → `GET /counter`) and asserts the rollback end-to-end.

## Where to next

- **[LPC.md]** — the formal language spec, pinned by DGD at commit `403cd0b`. §3.1 lexical elements, §3.2 expressions, §3.4 declarations and types, §3.5 statements, §3.6 inheritance, §3.7 preprocessing.
- **[dworkin/lpc-doc/kfun/]** — the kfun catalog. Every host-provided function the language calls.
- **[`docs/architecture.md`](architecture.md)** — the platform's tier model, daemons, the auto-inheritance chain, the host-driver extension surface.
- **[`docs/application-authoring.md`](application-authoring.md)** — what writing a tier-E application on top of this platform looks like: domain layout, initd, owner / access, the object-manager lifecycle, `call_touch` upgrade.
- **[`docs/kernel-libraries.md`](kernel-libraries.md)** — the inheritable libraries shipped under [`src/lib/`](../src/lib/) (String, StringBuffer, KVstore, Iterator, Continuation variants, Time).
- **[`docs/runtime-primitives.md`](runtime-primitives.md)** — per-primitive foundation-and-status statement (atomicity, persistent state, hot reload, capability separation, etc.).

[DGD]: https://github.com/dworkin/dgd
[dworkin/lpc-doc]: https://github.com/dworkin/lpc-doc
[LPC.md]: https://github.com/dworkin/lpc-doc/blob/master/LPC.md
[dworkin/lpc-doc/kfun/]: https://github.com/dworkin/lpc-doc/tree/master/kfun
