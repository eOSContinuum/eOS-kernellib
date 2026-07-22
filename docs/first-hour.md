# Your first hour

A hands-on tutorial. In the next hour you will boot the platform, create a living object from the operator console, give it state, watch the platform react to a state change, then kill the process and watch everything come back. No theory beyond one sentence per step. The reading-path docs carry the depth. This is the part where you see it.

**Audience**: a newcomer who has completed [getting-started.md](getting-started.md) (DGD built, `example.dgd` pointing at this repository's `src/`, and the `state/` directory shipping with the checkout) and has not yet written any LPC. Every command is shown with its expected output.

**What you'll have at the end**: a domain you created, a singleton and a clone you compiled, properties you set, an observer that fired the instant a property changed, and (the point of the whole platform) all of it alive after the process was stopped and restarted.

## 1. Boot

```sh
/path/to/dgd/bin/dgd example.dgd
```

The boot log prints, each line stamped with the host time (and the banner says 1.7.9 even on a newer `master` build -- `docs/getting-started.md` Install DGD):

```text
Jul 21 18:59:39 ** DGD 1.7.9
Jul 21 18:59:39 ** Initializing...
Jul 21 18:59:39 ** Initialization complete.
```

followed by a short burst of `NOTICE` lines as the platform domains finish deferred startup work. On this default build -- no crypto module loaded -- the burst begins with the identity stack standing down cleanly, and every one of these lines is normal and expected: `identity: registry up; crypto module absent (minting unavailable)`, `webauthn: ceremony daemon up`, the matching `session:` and `agentauth:` stand-downs, then `auth: transport authentication facade up`. The daemons boot, report, and refuse their crypto-dependent operations until the module is loaded (`docs/operations.md` Loading host-driver extensions). Then come the `DTD:: Registered ...` registrations, one `https: TLS stack not compiled ... standing down` line (normal for the same reason -- the TLS stack needs host-driver support this build lacks), and one `Schema:Daemon: cross-checked ...` line per bundled core-schema file. A `Warning:: Schema node ... not found!` or `import_state FAILED` line here is NOT normal. It means a schema file names an element the registry cannot resolve, or its import errored. The driver compiled the kernel and platform domains and is now listening. Leave it running. Open a second terminal for everything below.

## 2. Connect and claim the console

```sh
telnet localhost 8023    # or: nc localhost 8023
```

A welcome banner prints, then the login prompt. On the very first cold boot the platform has no administrator yet, so logging in as `admin` makes you one:

```text
login: admin
Pick a new password:
Retype new password:
Password changed.
# 
```

The `# ` prompt is the operator console. Your password hash just became part of the platform's state. You will prove that in step 8.

## 3. Create a domain

```text
# grant Pet access
```

Silent success (the prompt returns). This created the owner `Pet` and its directory `/usr/Pet`. Owners are the unit of capability and resource accounting: everything compiled under `/usr/Pet` runs as `Pet`, bounded by `Pet`'s quotas.

```text
# access Pet
Pet has no special access.
```

"No special access" is the capability model in one line: a fresh owner holds its own tree and reaches nothing else until granted.

## 4. Write two small files

The grant created `/usr/Pet` itself but not its conventional subdirectories, so create those first (on the host, in the repository you booted from):

```sh
mkdir -p src/usr/Pet/obj src/usr/Pet/sys
```

Then, in your editor, create two files.

`src/usr/Pet/obj/pet.c`, a clonable:

```c
inherit "/lib/util/properties";
```

`src/usr/Pet/sys/keeper.c`, a singleton:

```c
inherit "/lib/util/properties";
```

That is genuinely the whole file, both times. The property library is the platform's state surface. Inheriting it is all an object needs to carry persistent, observable state.

## 5. Compile and clone: code becomes live without a restart

Back at the console:

```text
# compile /usr/Pet/sys/keeper.c
$0 = </usr/Pet/sys/keeper>
# compile /usr/Pet/obj/pet.c
$1 = </usr/Pet/obj/pet>
# clone /usr/Pet/obj/pet
$2 = </usr/Pet/obj/pet#212>
```

The platform was running the whole time. There was no build step, no deploy step, no restart: `compile` installed your programs into the live image, and `clone` instantiated one. The `$N` names are console history references. `$2` is your clone. (Your clone number will differ from `#212`. Clone indices are platform-global.)

## 6. Give the clone state

```text
# code $2->set_property("pet:name", "Fred")
$3 = "Fred"
# code $2->query_property("pet:name")
$4 = "Fred"
```

`code` evaluates an LPC expression (`set_property` returns the value it set). The property write is the platform's canonical state mutation: no schema, no save call. Now wire the clone into the singleton, so you can find it again later by path:

```text
# code "/usr/Pet/sys/keeper"->set_property("pet:companion", $2)
$5 = </usr/Pet/obj/pet#212>
```

The keeper now holds a live reference to your clone. Object references are first-class state.

## 7. Watch the platform react

Register an observer on the keeper (a sandboxed script that runs the instant a property changes, inside the same atomic operation as the write itself):

```text
# register-observer /usr/Pet/sys/keeper pet:mood main Set($this, "pet:mood-noted", 1);
register-observer: registered on /usr/Pet/sys/keeper pet:mood:main
# observers /usr/Pet/sys/keeper pet:mood
/usr/Pet/sys/keeper pet:mood:
  pre:
    (none)
  main:
    [0] /usr/Merry/data/merry#-1 {Set($this, "pet:mood-noted", 1);}
  post:
    (none)
```

(The `[0]` is the slot index. The `{...}` tail echoes the script source, re-expanded from its parse tree, so its spacing may differ slightly from what you typed.)

Now trip it:

```text
# code "/usr/Pet/sys/keeper"->set_property("pet:mood", "sunny")
$6 = "sunny"
# code "/usr/Pet/sys/keeper"->query_property("pet:mood-noted")
$7 = 1
```

The `pet:mood-noted` marker was written by the observer, not by you. No queue, no poller, no worker process: the reaction completed before your `set_property` returned. (The observer source is Merry, a sandboxed scripting layer. [merry-language.md](merry-language.md) when you want it.)

## 8. The persistence win

Stop the platform, snapshot and exit in one verb:

```text
# reboot
```

The telnet session drops and the `dgd` process in your first terminal exits. The platform is now **not running**. Your programs, your clone, Fred's name, the keeper's reference, and the registered observer exist only in `state/snapshot`. (Two things were also written to host files the moment they changed: the admin password and the `Pet` access grant live under `src/kernel/data/`: credentials and access bits are deliberately file-backed, so they survive even without a snapshot.)

Start it again, restoring from the snapshot:

```sh
/path/to/dgd/bin/dgd example.dgd state/snapshot
```

The boot log says `** State restored.` No initialization, no recompiles: the image is back. Reconnect:

```sh
telnet localhost 8023    # or: nc localhost 8023
```

```text
login: admin
Password:
# 
```

It asked for your password instead of offering to set one. The credential survived (via its host file, as noted above). Now the snapshot-carried state:

```text
# code "/usr/Pet/sys/keeper"->query_property("pet:companion")
$0 = </usr/Pet/obj/pet#212>
# code "/usr/Pet/sys/keeper"->query_property("pet:companion")->query_property("pet:name")
$1 = "Fred"
```

Read that closely. The process died. Nothing was saved by you: no database, no serialization, no save call anywhere in this hour. The clone is back **as the same object** (same `#212`), its state intact, still referenced by the keeper. The console history reset (`$0` again) because your *connection* is new: connections are the one thing snapshots don't carry.

And the observer is not just remembered: it still fires. Clear the marker, then write the property again:

```text
# code "/usr/Pet/sys/keeper"->set_property("pet:mood-noted", 0)
$2 = 0
# code "/usr/Pet/sys/keeper"->set_property("pet:mood", "rainy")
$3 = "rainy"
# code "/usr/Pet/sys/keeper"->query_property("pet:mood-noted")
$4 = 1
```

The marker went 0 → 1 on this side of the restart: the compiled observer script survived the snapshot and re-fired against the restored object.

## What you just used

| Step | Primitive | Depth |
|---|---|---|
| 3 | Capability separation: owners and access | [architecture.md](architecture.md) |
| 5 | Hot code load: compile into the running image | [code-lifecycle.md](code-lifecycle.md) |
| 6 | Persistent state: properties, object references | [persistence.md](persistence.md) |
| 7 | Sandboxed reaction, atomic with the write | [dispatcher.md](dispatcher.md), [signal-applications.md](signal-applications.md) |
| 8 | Orthogonal persistence: the image survives the process | [persistence.md](persistence.md) |

## Where to next

- **[first-application.md](first-application.md)** is the next tutorial: author your own application (a key-value service) from an empty domain to running verbs, an atomic failure that rolls back cleanly, a hot-fix without a restart, and its data surviving a restart.
- **[persistence.md](persistence.md)** explains why step 8 works, what exactly survives, and the boundaries (connections, external resources, time).
- **[coming-from-contemporary-infrastructure.md](coming-from-contemporary-infrastructure.md)** covers what this replaces in a service-stack mental model.
- **[lpc-essentials.md](lpc-essentials.md)** covers the language, now that you've compiled some.
- **[`examples/signal-app/`](../examples/signal-app/)** and **[signal-applications.md](signal-applications.md)** show step 7 as a proper application.
- **[admin-console.md](admin-console.md)** documents the console you just drove, in full.
