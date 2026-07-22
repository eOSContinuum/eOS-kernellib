# Your first application

A hands-on tutorial. In [first-hour.md](first-hour.md) you drove the console and watched the platform's state survive a restart, but the objects you made were one-line files. Here you author a real application end to end: a small key-value service with its own methods, backed by a daemon you write. You will drive its verbs from the console, trigger an atomic rollback and watch it leave nothing behind, apply a hot-fix without a restart, then stop the process and find your data still there.

**Audience**: a reader who has completed [first-hour.md](first-hour.md) (platform booted, `admin` console claimed, comfortable with `compile` and `code`) and is ready to write more than a one-line file. No LPC beyond what is shown. [lpc-essentials.md](lpc-essentials.md) is the reference when a construct is new. Every command is shown with its expected output.

**What you'll have at the end**: a domain you created, an `initd` and a daemon you wrote (about thirty lines of LPC), verbs you drove against live state, an atomic failure that rolled back cleanly, a method you added to the running service without stopping it, and the store intact after the process was killed and restarted.

The service is the in-memory key-value store sketched in [application-authoring.md](application-authoring.md) (Worked example sketch). This tutorial builds it for real.

## 1. Create the domain

Boot the platform and claim the `admin` console exactly as in [first-hour.md](first-hour.md) (sections 1 and 2), then create an owner for your application:

```text
# grant KV access
# access KV
KV has no special access.
```

`grant KV access` created the owner `KV` and its directory tree `/usr/KV`. Everything compiled under `/usr/KV` runs as `KV`, bounded by `KV`'s quotas, and reaches nothing outside its own tree until granted. The "no special access" line is that isolation stated back to you.

## 2. Write the initd and the daemon

Every tier-E domain has an `initd.c` at its root. The System initd compiles it at cold boot, and its `create()` compiles the domain's own objects. Create the directories on the host (in the repository you booted from), then the two files.

```sh
mkdir -p src/usr/KV/sys
```

`src/usr/KV/initd.c`, the domain's bootstrap:

```c
# include <kernel/kernel.h>

inherit "/usr/System/lib/auto";

static void create()
{
    ::create();
    compile_object("sys/kv_daemon");
}
```

The initd inherits the System auto library (the tier-E convention, so it can reach the System-tier compile helpers), chains to the inherited `create()`, then compiles the daemon. `"sys/kv_daemon"` resolves relative to the initd's own directory, `/usr/KV`.

`src/usr/KV/sys/kv_daemon.c`, the service itself:

```c
private mapping store;      /* the key-value store */
private int counter;        /* a running count, for the rollback demo */

static void create()
{
    store = ([ ]);
}

void put(string key, mixed value)
{
    store[key] = value;
}

mixed get(string key)
{
    return store[key];
}

void remove(string key)
{
    store[key] = nil;
}

int query_counter()
{
    return counter;
}

atomic void increment_and_fail()
{
    counter++;
    error("deliberate failure after mutating counter");
}
```

That is the whole service. `store` is an ordinary mapping held in a global variable, and `put` / `get` / `remove` operate on it. There is no save call and no schema. Section 6 shows why none is needed. The daemon inherits nothing explicitly: every object implicitly inherits the kernel auto (`auto_object` in the driver config), which is all a plain data daemon needs. `increment_and_fail` is deliberately broken. Section 4 uses it, and its `atomic` modifier is the point of that section.

## 3. Compile and drive it

Back at the console, compile both files. At cold boot the System initd compiles your `initd.c` **and touches it**, which runs its `create()` and so compiles the daemon for you. The console `compile` verb does only the first half: it compiles the program and defers `create()` until the object's first use -- so nothing has compiled the daemon yet, and you compile it yourself:

```text
# compile /usr/KV/initd.c
$0 = </usr/KV/initd>
# compile /usr/KV/sys/kv_daemon.c
$1 = </usr/KV/sys/kv_daemon>
```

From the next cold boot onward the initd does this automatically; the two-step form is a console-session fact, not the platform's boot story. The daemon is live now (its `create()` runs at its first use, initializing the empty store). Prove it by driving its verbs. `code` evaluates an LPC expression, and `->` calls a method on the object named by its path. (If you have run anything extra at the console, your `$N` indices will be higher: history numbers every value-producing command in sequence, so any experiment shifts all later slots. Compare the values to the right of the `=`, not the slot numbers.)

```text
# code "/usr/KV/sys/kv_daemon"->put("greeting", "hello")
$2 = nil
# code "/usr/KV/sys/kv_daemon"->get("greeting")
$3 = "hello"
# code "/usr/KV/sys/kv_daemon"->put("lang", "LPC")
$4 = nil
# code "/usr/KV/sys/kv_daemon"->get("lang")
$5 = "LPC"
# code "/usr/KV/sys/kv_daemon"->remove("greeting")
$6 = nil
# code "/usr/KV/sys/kv_daemon"->get("greeting")
$7 = nil
```

`put` and `remove` are declared `void`, so they return no value and the console shows `nil`. `get` returns what you stored. Your service is running: two keys went in, one came back out, one was removed. The store holds `"lang"` now.

## 4. Atomicity: a failure that leaves nothing behind

`increment_and_fail` increments the counter and then errors. Read the counter, call it, and read the counter again:

```text
# code "/usr/KV/sys/kv_daemon"->query_counter()
$8 = 0
# code "/usr/KV/sys/kv_daemon"->increment_and_fail()
Error: deliberate failure after mutating counter.
# code "/usr/KV/sys/kv_daemon"->query_counter()
$9 = 0
```

The counter is still `0`. The `counter++` ran, then unhappened. That is the `atomic` modifier: a function declared `atomic` commits all of its state changes or none of them, and an error inside it rolls everything back. You wrote no rollback code. The runtime enforced it. Notice that the failed call consumed no `$N` slot. It produced no value, only an error.

This is the platform's transactional guarantee at the smallest scale. A real multi-step write (moving a value from one key to another, say) wrapped in one `atomic` function either lands wholly or not at all, even if it fails partway. See [runtime-primitives.md](runtime-primitives.md) (Atomicity) for the foundation, and `examples/atomic-demo/` for the same guarantee exercised over HTTP. Note from [execution-model.md](execution-model.md) (The tick budget, mechanically) that an `atomic` function runs on half the tick budget.

## 5. A hot-fix without a restart

Your service is live and holding data. Suppose you now need it to report how many keys it holds. Add a method to the daemon on the host, above `query_counter` in `src/usr/KV/sys/kv_daemon.c`:

```c
int size()
{
    return map_sizeof(store);
}
```

Recompile just the daemon into the running image:

```text
# compile /usr/KV/sys/kv_daemon.c
$10 = </usr/KV/sys/kv_daemon>
# code "/usr/KV/sys/kv_daemon"->size()
$11 = 1
# code "/usr/KV/sys/kv_daemon"->get("lang")
$12 = "LPC"
```

The new method answered immediately, with no restart and no redeploy. `size()` returned `1`, not `0`: the store you filled in section 3 survived the recompile intact, `"lang"` still in it. `compile` replaced the daemon's program while keeping the object's data. The same move fixes a bug in a live service: edit, recompile, done, with the service's state carried across untouched. [code-lifecycle.md](code-lifecycle.md) covers the mechanism, and [application-authoring.md](application-authoring.md) (Live code upgrade through call_touch) covers the harder case, migrating data when a recompile changes the variable layout, which adding a method does not.

## 6. The persistence win

You added `store = ([ ])` in `create()` and never wrote a save call. Stop the platform anyway. `reboot` snapshots and exits in one verb:

```text
# reboot
```

The telnet session drops and the `dgd` process exits. Your daemon, its program, and every key in the store now exist only in `state/snapshot`. Start the platform again, restoring from it:

```sh
/path/to/dgd/bin/dgd example.dgd state/snapshot
```

The boot log says `** State restored.` Reconnect (`telnet localhost 8023`, log in), and read a key you stored before the restart:

```text
# code "/usr/KV/sys/kv_daemon"->get("lang")
$0 = "LPC"
```

The process died and came back, and the store is intact. No database, no serialization, no save call anywhere in this tutorial. DGD snapshots the whole image, your daemon's mapping included. The restored boot skips initialization entirely (your `initd` does not re-run) because there is nothing to rebuild. This is orthogonal persistence applied to an application you wrote. [persistence.md](persistence.md) states what survives, what does not (connections, external resources, wall-clock time), and the backup and restore mechanics.

## What you just used

| Section | Primitive | Depth |
|---|---|---|
| 1 | Capability separation: owners and their trees | [architecture.md](architecture.md) |
| 2 | Domain layout and the initd's compile role | [application-authoring.md](application-authoring.md) |
| 3 | Hot code load: compile into the running image | [code-lifecycle.md](code-lifecycle.md) |
| 4 | Atomicity: all-or-nothing state, rollback on error | [runtime-primitives.md](runtime-primitives.md) |
| 5 | Single-object hot reload with dataspace survival | [code-lifecycle.md](code-lifecycle.md) |
| 6 | Orthogonal persistence: the image survives the process | [persistence.md](persistence.md) |

## Where to next

- **[first-http-endpoint.md](first-http-endpoint.md)** continues this build directly: an HTTP face on the service you just wrote, driven with `curl`, ending at the reference and assembly docs.
- **[application-authoring.md](application-authoring.md)** covers the patterns behind this tutorial at reference depth: domain layout, owner and access, tick-aware code, object tracking, and when the four-file example shape stops fitting.
- **[kernel-libraries.md](kernel-libraries.md)** documents the inheritable libraries you call from application code, including a `KVstore` library. This tutorial hand-rolled a mapping to keep the moving parts visible.
- **[persistence.md](persistence.md)** is section 6 in full: what the snapshot carries, the persistence boundaries, and the backup and restore mechanics.
- **[dispatcher.md](dispatcher.md)** and **[signal-applications.md](signal-applications.md)** give your service reactions: run a script the instant a property changes, atomic with the write, the way [first-hour.md](first-hour.md) section 7 did.
- **[where-code-belongs.md](where-code-belongs.md)** covers when a piece of behavior belongs in plain LPC like this daemon versus a sandboxed Merry script, and its Where-state-belongs fork -- your KV store is one plain mapping, but a real domain chooses property table versus plain member per datum. **[application-authoring.md](application-authoring.md)** Modeling domain data carries the modeling patterns behind that: entity shapes, lookup by field, secondary indexes, enumeration.
