# Writing Merry applications

A Merry application on eOS-kernellib runs sandboxed scripts against an object hierarchy. Scripts are looked up by signal name through an ancestry walk -- a property bound on an ur-parent is visible to every descendant via `query_ur_object()`. The sections below show what the surface looks like, how to invoke a script from external code, what the sandbox forbids, and what the bundled reference application actually exercises.

**Audience**: an application author who wants to add scripted, sandboxed behavior to a persistent object surface on eOS-kernellib; comfortable with LPC (or read `docs/lpc-essentials.md` first); has the platform running locally per `docs/getting-started.md`.

`docs/architecture.md` covers the capability tiers and where the Merry subsystem sits. `docs/runtime-primitives.md` covers sandboxed code-load and the ur-hierarchy as platform properties. `docs/vault-applications.md` covers persistent-state participation; this document covers invocation.

## The script-bearing-object contract

A Merry-dispatching object only needs three things:

- a property store that distinguishes raw (case-preserving) and downcased keys -- inherit `/lib/util/properties` to get `set_property` / `query_raw_property` / `query_prefixed_properties`;
- an ur-parent / ur-child relationship that `find_merry` can walk -- inherit `/lib/util/ur` to get `set_ur_object` / `query_ur_object`;
- a logical name registration so the script's `Get(obj, "...")` and `Set(obj, "...", ...)` calls have a stable identity to reach -- inherit `/lib/util/named` and call `set_object_name(name)` at create.

Both `/lib/util/properties` and `/lib/util/ur` define `static void create()`; cloud-server's inherit resolution requires labels to disambiguate them in any inheritor that combines both. The reference application uses:

```c
inherit "/lib/util/named";
inherit properties "/lib/util/properties";
inherit ur "/lib/util/ur";

static void create()
{
    properties::create();
    ur::create();
}
```

The pattern surfaced on the LM-3.5 throwaway probe object when the same two libs collided; labels are the canonical fix.

## Script storage convention

A Merry script binds to its target via a property key of the form `merry:<mode>:<signal>`:

| Key | Lookup behavior |
|-----|-----------------|
| `merry:<mode>:<signal>` | Script object found at this exact ancestor wins the dispatch. |
| `merry:inherit:<mode>:<signal>` | Pointer to another object whose `merry:<mode>:<signal>` should be consulted (delegation chain). |

`find_merry` walks `query_ur_object()` from the target upward, looking for an exact match at each level. The first hit wins. If no level has the property, `find_merry` returns nil and `run_merry` returns `TRUE` (the conservative no-op).

`mode` is an application-defined namespace -- `lib` is the SkotOS convention for callable libraries; an application can introduce its own (`on`, `pre`, `validate`, ...) without touching the runtime. The dispatcher work in the upcoming `DD-*` phase will settle a property-change-observer convention layered on top of this same lookup mechanism.

## Invocation surface

Merry's invocation API is a static surface in `/usr/Merry/lib/merryapi`:

| Function | Purpose |
|----------|---------|
| `find_merry(ob, signal, mode)` | Returns the script object at the first ancestor with `merry:<mode>:<signal>`, or nil. |
| `find_merry_location(ob, signal, mode)` | Same walk, but returns the ancestor object that carries the script, not the script itself. |
| `run_merry(ob, signal, mode, args, [label])` | Walks via `find_merry`, calls `script->evaluate(...)`, returns whatever the script returned. Returns `TRUE` if no script found. |
| `find_merries(ob, signal, mode)` | Returns a mapping of every script bound under `<dprop>` or `<dprop>%`-prefix or delegated via `merry:inherit:*` keys across the full ancestry. |
| `run_merries(ob, signal, mode, args, [label])` | Calls each script in the `find_merries` mapping, anding their results. |

The methods are `static`. Daemons that want to dispatch a Merry script inherit `merryapi` rather than calling `SYS_MERRY->run_merry(...)`. The static qualifier preserves the inheritance-based invocation convention SkotOS authored against (every production callsite of `run_merry` in SkotOS goes through an inheriting daemon, not through the daemon object via `->`).

If a daemon-level dispatch surface is needed in the future (for cross-domain or stateless callers), a public delegator added to `sys/merry.c` is the natural extension point; the static merryapi stays as the inheritance-target.

## Compiling a script

A Merry script is a clone of `/usr/Merry/data/merry` constructed with the source string:

```c
script = new_object("/usr/Merry/data/merry",
                    "return \"hello, world\";");
```

The clonable's `create(string lpc)` invokes the parser (`SYS_MERRY->parse_merry`), generates a wrapped LPC function (`mixed merry(string mode, string signal, string label) { switch(label) { case "virgin": { <source> } } }`), MD5-hashes the wrapper source, and compiles it to `/usr/Merry/merry/<md5>` via `compile_object(name, source)`. Identical sources share the same compiled object (the `4*NODE_COUNT` LRU registered through `SYS_MERRY->new_merry_node` evicts stale entries when the cache fills).

Bind the script to the target's property store with the convention key:

```c
target->set_property("merry:lib:greet", script);
```

`set_property` downcases the key (`set_downcased_property` is the underlying mutator). Inheritors that want the case-preserving variant can call `set_downcased_property` directly.

## What the sandbox forbids

`merrynode.c` defines a 51-function deny set via the `SANDBOX(f)` macro. Each entry shadows the underlying kfun with an error-throwing local: any Merry source that calls one fails with `function '<name>' not allowed in merry code`.

The list covers escape-shape and external-effect-shape kfuns: object lifecycle (`clone_object`, `destruct_object`, `destruct_program`, `compile_object`, `restore_object`, `dump_state`, `dump_file`, `dump_interval`, `swapout`, `remove_program`), filesystem (`read_file`, `write_file`, `make_dir`, `remove_dir`, `remove_file`, `rename_file`, `get_dir`, `file_info`, `editor`, `query_editor`), networking (`open_port`, `connect`, `ports`, `send_datagram`, `send_message`, `send_close`, `connect_datagram`, `datagram_*` 6 entries, `telnet_connect`, `telnet_port`), input/output (`block_input`, `subscribe_event`, `unsubscribe_event`, `add_event`, `event`, `event_except`, `remove_event`, `this_user`, `users`), call-out manipulation (`remove_call_out`), object identity (`set_object_name`, `set_originator`, `query_originator`), execution (`execute_program`, `function_object`, `call_touch`), and the shutdown kfun. `call_other` and `new_object` are wrapped: object-typed callees error; non-object callees pass through.

`call_out` is allowed -- Merry scripts may schedule timers via `In(signal, seconds)` and `Every(signal, seconds)`.

`->` (call_other syntax) and `rlimits` are forbidden at the grammar level rather than at the SANDBOX layer; a script containing either will fail to parse.

## Reference application

`examples/merry-app/` carries a working reference: a property + ur-bearing clonable, a marker lib, and a boot-time test driver that exercises the ancestry walk and the sandbox firing. The code in that directory is the canonical example -- accurate, compiling, and runnable. To deploy:

```sh
cp -R examples/merry-app src/usr/MerryApp
```

Then sync the runtime (`scripts/setup-runtime.sh`) and boot DGD against `mva.dgd`. The verify command in the example's README cats `src/usr/MerryApp/data/test-result.log` and expects three lines, in order:

```
MerryApp:test: starting
MerryApp:test: ANCESTRY OK
MerryApp:test: SANDBOX OK
```

The sections below explain what the reference application does and why. Read the code in `examples/merry-app/sys/test.c` alongside this document.

## Application layout

A minimal Merry application is four files plus an initd:

```text
src/usr/MerryApp/
  initd.c           - domain initd; compiles obj/thing + sys/test at boot
  lib/
    app.c           - marker; daemons under sys/ inherit it
  obj/
    thing.c         - property + ur-bearing clonable
  sys/
    test.c          - boot-time test driver (inherits lib/app + merryapi)
```

`lib/app.c` is a placeholder -- there is no shared daemon-side state yet. It mirrors the shape of `examples/vault-app/lib/app.c` so application authors see a parallel pattern across the bundled examples; daemons under `sys/` can later factor common state into the lib when needed.

## Boot-order constraint

System loads domains alphabetically. Applications whose name sorts before `Merry` see the daemon as not-yet-loaded during their own initd. The reference application's `sys/test.c::create` defers everything to `call_out("setup_and_run", 0)` so the work fires after every domain's initd has returned and the Merry daemon (including the lazy parser scratch directory `/usr/Merry/tmp/`) is up.

## The two assertions

### Phase 1 -- ancestry walk

```c
parent = clone_object(THING_PROG);
parent->set_object_name("MerryApp:demo:parent");

child = clone_object(THING_PROG);
child->set_object_name("MerryApp:demo:child");
child->set_ur_object(parent);

script = new_object(MERRY_DATA,
                    "return \"MerryApp:test: ANCESTRY OK\";");
parent->set_property("merry:lib:greet", script);

result = run_merry(child, "greet", "lib", ([ ]));
```

`run_merry(child, ...)` calls `find_merry(child, "greet", "lib")` which:

1. queries `child` for `merry:lib:greet` -- nil (script was bound on parent);
2. queries `child` for `merry:inherit:lib:greet` -- nil;
3. walks to `child->query_ur_object()` -> parent;
4. queries `parent` for `merry:lib:greet` -- finds the script object;
5. returns the script.

`run_merry` then calls `script->evaluate(child, "greet", "lib", ([ ]), nil, "MerryApp:demo:child/lib:greet")`. The clonable's evaluate forwards to the compiled `/merry/<md5>` program's evaluate, which sets up `this = child`, `args = ([])`, then calls `merry("greet", "lib", "virgin")`. The wrapped switch on `label == "virgin"` fires the body's `return "MerryApp:test: ANCESTRY OK"`. The string propagates back up the call stack.

### Phase 2 -- sandbox firing

```c
bad_script = new_object(MERRY_DATA,
                        "clone_object(\"/foo/bar\");");
parent->set_property("merry:lib:badcall", bad_script);

catch {
    run_merry(child, "badcall", "lib", ([ ]));
    /* unreachable on success */
} : {
    log_line("MerryApp:test: SANDBOX OK");
}
```

The Merry source `clone_object("/foo/bar");` parses cleanly -- the parser accepts the identifier-call syntax. At evaluation, the wrapped LPC calls `clone_object("/foo/bar")` against the compiled program's namespace; `merrynode.c`'s `SANDBOX(clone_object)` macro defines a local error-thrower that takes precedence over the kfun. The call raises `function 'clone_object' not allowed in merry code`, which the test driver's `catch` block captures and logs as the success sentinel.

## What this example does NOT exercise

Three Merry surfaces are present in the lift but not yet validated in production:

- **Duplicate / Spawn**. Both call `clone_object` from inside `merrynode.c`. The SANDBOX shadow captures the call from within Spawn / Duplicate too, so a Merry source that invokes `Duplicate(obj)` or `Spawn(ur)` will fail with the sandbox error. The fix is an `::clone_object` escape inside Spawn / Duplicate (or factoring them into a separately-inherited library). The L8 closure pairs with a richer Merry test in a later iteration.
- **$delay()** -- the `do_delay` -> `mcontext.c::create(4-arg)` continuation path. The wrapper is in place; the cloud-server `new_object(path, args...)` -> `_F_init` -> `create(args...)` 4-arg dispatch is unvalidated until a Merry source invokes `$delay(seconds, retval, label);` and surfaces the path under boot-time test.
- **LabelCall / LabelRef** -- the script-space invocation syntax (`space::method(...)`). The runtime support (`register_script_space` / `query_script_space` / merryfun `LabelCall` / `LabelRef`) is lifted. A separate test driver registering a script-space handler would cover it; the dispatcher work in the `DD-*` phase will surface a natural caller.

These three surfaces are not blocking for LM-4's "ancestry walk + sandbox" gates and stay on the deferred list for follow-on work.

## Storage and round-trip

The reference application binds scripts to in-memory clones; statedump survival of the binding requires Schema participation that `examples/merry-app/obj/thing.c` deliberately omits to keep the example focused on the runtime path. A Schema-registered version of the same thing would have:

- a per-app `MerryApp:Thing` schema_node with `merry:*` attributes typed as `lpc_obj` (so the marshaler can record the bound script clonable's program path on dump and re-resolve on restore);
- Vault registration via inheriting `~Vault/lib/vault_node` (the `examples/vault-app/` pattern), so the property tree survives `dump_state`.

The composite "scripted persistent object" is the natural next example -- composing `merry-app` and `vault-app` into a single demonstrator -- and lands as a follow-on once the LM phase closes.
