# Writing signal applications

The smallest thing the property-change dispatcher can do for an application: bind a reaction to a property, write the property, and have the reaction already done when the write returns. `examples/signal-app/` carries the working demonstration -- one property host, one observer, one write, one assertion -- and this page walks it and explains why the pattern is a runtime primitive here rather than infrastructure you assemble.

**Audience**: an application author wanting the smallest working reactive pattern before the full dispatcher reference; has the platform running per [getting-started.md](getting-started.md).

## The example

```text
src/usr/SignalApp/
  initd.c           - domain initd; compiles obj/thing + sys/test at boot
  lib/
    app.c           - thin marker lib (mirrors the other bundled examples)
  obj/
    thing.c         - the property host: a single /lib/util/properties inherit
  sys/
    test.c          - boot-time test driver
```

The property host is one inherit. That is the entire participation requirement: the property store carries both the observed state and the observer bindings (under `merry:on:<path>:<timing>` keys), so any object that stores properties can carry reactions.

The driver does four things:

```c
thing = clone_object("/usr/SignalApp/obj/thing");

MERRY->register_observer(thing, "signal:watched", "main",
    "Set($this, \"signal:fired\", 1); return TRUE;");

thing->set_property("signal:watched", 42);

/* by here, the reaction has already run */
if (thing->query_raw_property("signal:fired") == 1) { /* SIGNAL OK */ }
```

The observer source is Merry -- the platform's sandboxed scripting language -- compiled at registration time and stored on the host's own property table. The write routes through the dispatcher, which fires the observer synchronously inside the write: when `set_property` returns, the marker is set. There is no queue to drain, no poll interval, and no callback infrastructure beyond the one registration.

## Why this is a runtime primitive, not glue

If you are coming from a conventional stack, "react when state changes" decomposes into infrastructure: a message broker or queue between the writer and the reactor, a poller or trigger layer to notice the change, a worker to run the reaction, serialization at every hop, and a persistence story for each piece -- plus the failure modes of their seams (lost events, double delivery, reactions running against stale state).

Here the decomposition never happens, because the runtime already provides each half:

- **The state is durably live.** Objects persist orthogonally -- the property you wrote, the observer binding, and the compiled reaction all survive snapshot and restore without serialization code. A registration is not a row in a side table; it is part of the same persistent image as the data it watches.
- **The reaction is atomic with the change.** Observers fire inside the write. If the write is part of an atomic batch that rolls back, the reaction's effects roll back with it; there is no window where the state changed but the reaction has not happened, and no window where the reaction happened against a state that then unhappened.
- **The reaction is contained.** Observer source compiles into the Merry sandbox: a deny-list of system functions, no raw object access outside the provided surface. Application operators can accept reaction code at runtime without extending trust to it.
- **The wiring is data.** Bindings live in property storage, so everything that works on properties works on bindings: they replicate with the object, appear in state introspection, and can be audited and mutated at runtime from the admin console (see `admin-console.md`, "Dispatcher operator surface").

The honest boundaries: synchronous-in-write means a heavy reaction extends the write that triggered it; cascades are depth-bounded and cycle-detected rather than unbounded; and the sandbox deliberately refuses I/O and lifecycle functions inside reactions. Those are design positions, documented in `dispatcher.md`, not accidents.

## Where to next

This example is deliberately the floor. Each capability it omits is demonstrated by a sibling:

- **Timings and ordering** (pre/main/post, veto, multi-observer fan-out, cascade bounds, batching): `dispatcher.md` and `examples/merry-app/`.
- **Observer inheritance** (one registration on an ancestor covering a cohort of descendants): `merry-applications.md` ancestry walk; demonstrated in `examples/merry-app/` and `examples/chat-app/`.
- **A full application around the dispatcher** (multi-user chat with capability gates, persistence across boots, sandboxed reactions, cross-user notification): `chat-applications.md`.
- **Persistence to on-disk XML** (schema-backed export/import of property state): `vault-applications.md`.
