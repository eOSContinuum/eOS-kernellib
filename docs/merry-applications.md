# Writing Merry applications

A Merry application on eOS-kernellib runs sandboxed scripts against an object hierarchy. Scripts are looked up by signal name through an ancestry walk -- a property bound on an ur-parent is visible to every descendant via `query_parent()`. The sections below show what the surface looks like, how to invoke a script from external code, what the sandbox forbids, and what the bundled reference application actually exercises.

**Audience**: an application author who wants to add scripted, sandboxed behavior to a persistent object surface on eOS-kernellib; comfortable with LPC (or read `docs/lpc-essentials.md` first); has the platform running locally per `docs/getting-started.md`.

`docs/architecture.md` covers the capability tiers and where the Merry subsystem sits. `docs/runtime-primitives.md` covers sandboxed code-load and the ur-hierarchy as platform properties. `docs/vault-applications.md` covers persistent-state participation; this document covers invocation. For writing Merry source itself (dialect restrictions, the four extensions over LPC, the compile pipeline, AST nodes, the full sandbox surface, the merryfun catalog), see `docs/merry-language.md`.

## The script-bearing-object contract

A Merry-dispatching object needs four things:

- a property store that distinguishes raw (case-preserving) and downcased keys -- inherit `/lib/util/properties` to get `set_property` / `query_raw_property` / `query_prefixed_properties`;
- an ur-parent / ur-child relationship that `find_merry` can walk -- inherit `/lib/util/ur` to get `set_ur_object` / `query_parent`;
- a logical name registration so the script's `Get(obj, "...")` and `Set(obj, "...", ...)` calls have a stable identity to reach -- inherit `/lib/util/named` and call `set_object_name(name)` at create;
- the `delayed_call` / `perform_delayed_call` pair a bound script's `$delay()` needs to schedule a continuation against the host -- inherit `/lib/util/delayed` (below). Every script-bearing object needs this, since any script bound to it may call `$delay()`.

Both `/lib/util/properties` and `/lib/util/ur` define `static void create()`; cloud-server's inherit resolution requires labels to disambiguate them in any inheritor that combines both. The reference application uses:

```c
inherit "/lib/util/named";
inherit properties "/lib/util/properties";
inherit ur "/lib/util/ur";
inherit "/lib/util/delayed";

static void create()
{
    properties::create();
    ur::create();
}
```

The pattern surfaces whenever the same two libs collide; labels are the canonical fix.

Every script-bearing object (a class passed as `this` at `run_merry` / `run_merries`) must expose `delayed_call(object ob, string fun, mixed delay, mixed args...)` and a `static void perform_delayed_call(object ob, string fun, mixed *args)` companion. Both come from `/lib/util/delayed`; a script-bearing object inherits that lib to gain the pair (`examples/merry-app/obj/thing.c` and `examples/chat-app/obj/room.c` both do).

A script's `$delay(seconds, retval)` call expands at the grammar level to `{ do_delay(mode, signal, seconds, "<label>"); return retval; case "<label>": ; }`. `do_delay` calls `::call_other(this, "delayed_call", new_object("/usr/Merry/data/mcontext", signal, mode, label, args, this_object()), "merry_continuation", seconds, this)`. The binding host's `delayed_call` (from `/lib/util/delayed`) wraps that in a `call_out("perform_delayed_call", seconds, mcontext, "merry_continuation", ({ this }))`; when it fires, `perform_delayed_call` calls `merry_continuation` on the mcontext, which calls `run_merries(this, signal, mode, args, label)` to resume execution at the case label -- a second pass through the same compiled script, continuing past the `$delay()` call rather than restarting it.

## Script storage convention

A Merry script binds to its target via a property key of the form `merry:<mode>:<signal>`:

| Key | Lookup behavior |
|-----|-----------------|
| `merry:<mode>:<signal>` | Script object found at this exact ancestor wins the dispatch. |
| `merry:inherit:<mode>:<signal>` | Pointer to another object whose `merry:<mode>:<signal>` should be consulted (delegation chain). |

`find_merry` walks `query_parent()` from the target upward, looking for an exact match at each level. The first hit wins. If no level has the property, `find_merry` returns nil and `run_merry` returns `TRUE` (the conservative no-op).

`mode` is an application-defined namespace -- `lib` is the convention for callable libraries; an application can introduce its own (`pre`, `validate`, ...) without touching the runtime. The property-change dispatcher uses `on` as its mode and the composite `<path>:<timing>` as its signal; the storage convention, ancestry walk, batching surface, and observer-source contract are documented in `docs/dispatcher.md`.

## Invocation surface

Merry's invocation API is a static surface in `/usr/Merry/lib/merryapi`:

| Function | Purpose |
|----------|---------|
| `find_merry(ob, signal, mode)` | Returns the script object at the first ancestor with `merry:<mode>:<signal>`, or nil. |
| `find_merry_location(ob, signal, mode)` | Same walk, but returns the ancestor object that carries the script, not the script itself. |
| `run_merry(ob, signal, mode, args, [label])` | Walks via `find_merry`, calls `script->evaluate(...)`, returns whatever the script returned. Returns `TRUE` if no script found. |
| `find_merries(ob, signal, mode)` | Returns a mapping of every script bound under `<dprop>` or `<dprop>%`-prefix or delegated via `merry:inherit:*` keys across the full ancestry. |
| `run_merries(ob, signal, mode, args, [label])` | Calls each script in the `find_merries` mapping, anding their results. |

The methods are `static`. Daemons that want to dispatch a Merry script inherit `merryapi` rather than calling `MERRY->run_merry(...)`. The static qualifier preserves the inheritance-based invocation convention the API was authored against: call sites go through an inheriting daemon, not through the daemon object via `->`.

If a daemon-level dispatch surface is needed in the future (for cross-domain or stateless callers), a public delegator added to `sys/merry.c` is the natural extension point; the static merryapi stays as the inheritance-target.

A script-space handler offers a second invocation route alongside `find_merry` / `run_merry`: `MERRY->register_script_space(name, handler)` registers an object exposing `int query_method(string name)` and `mixed call_method(string name, mapping args)`. Merry source calls into it via `<space>::<method>($arg: value, ...)`, which the grammar maps to `LabelCall(space, method, ({ arg, value, ... }))`. `merrynode.c::LabelCall` queries `MERRY->query_script_space(space)`, then calls `Call(handler, method, local)`; `Call` walks the handler's `query_method` -- a non-zero return routes through `handler->call_method(method, args)`, where `args` is the merry-source `args` mapping with the inline locals overlaid.

Script-space handlers are independent of the ancestry-walk lookup. `LabelCall` is the entry point for runtime-extensible namespaces -- a daemon, a per-domain command surface, an MCP-style external bridge. Each registration adds one name to the namespace; unregister cleans up. The registry survives statedump (it lives on the `MERRY` daemon's persistent state).

## Compiling a script

A Merry script is a light-weight instance of `/usr/Merry/data/merry` (via `new_object`) constructed with the source string:

```c
script = new_object("/usr/Merry/data/merry",
                    "return \"hello, world\";");
```

The clonable's `create(string lpc)` invokes the parser (`MERRY->parse_merry`), generates a wrapped LPC function (`mixed merry(string mode, string signal, string label) { switch(label) { case "virgin": { <source> } } }`), MD5-hashes the wrapper source, and compiles it to `/usr/Merry/merry/<md5>` via `compile_object(name, source)`. Identical sources share the same compiled object (the `4*NODE_COUNT` LRU registered through `MERRY->new_merry_node` evicts stale entries when the cache fills).

Bind the script to the target's property store with the convention key:

```c
target->set_property("merry:lib:greet", script);
```

`set_property` downcases the key (`set_downcased_property` is the underlying mutator). Inheritors that want the case-preserving variant can call `set_downcased_property` directly.

Merry source has no LPC-style variable declarations -- `object o = Spawn(...)` is a parse error. Compose merryfun calls inline instead.

## What the sandbox forbids

`merrynode.c` defines a 51-function deny set via the `SANDBOX(f)` macro. Each entry shadows the underlying kfun with an error-throwing local: any Merry source that calls one fails with `function '<name>' not allowed in merry code`.

The list covers escape-shape and external-effect-shape kfuns: object lifecycle (`clone_object`, `destruct_object`, `destruct_program`, `compile_object`, `restore_object`, `dump_state`, `dump_file`, `dump_interval`, `swapout`, `remove_program`), filesystem (`read_file`, `write_file`, `make_dir`, `remove_dir`, `remove_file`, `rename_file`, `get_dir`, `file_info`, `editor`, `query_editor`), networking (`open_port`, `connect`, `ports`, `send_datagram`, `send_message`, `send_close`, `connect_datagram`, `datagram_*` 6 entries, `telnet_connect`, `telnet_port`), input/output (`block_input`, `subscribe_event`, `unsubscribe_event`, `add_event`, `event`, `event_except`, `remove_event`, `this_user`, `users`), call-out manipulation (`remove_call_out`), object identity (`set_object_name`, `set_originator`, `query_originator`), execution (`execute_program`, `function_object`, `call_touch`), and the shutdown kfun. `call_other` and `new_object` are wrapped: `call_other` passes through only `nil`, `int`, `float`, array, and mapping callees, and errors on both object- and string-typed callees (so the plain object-name-string invocation form errors too); `new_object` errors unconditionally, with no callee ever allowed through.

`call_out` is allowed -- Merry scripts may schedule timers via `In(signal, seconds)` and `Every(signal, seconds)`.

`->` (call_other syntax) and `rlimits` are forbidden at the grammar level rather than at the SANDBOX layer; a script containing either will fail to parse.

`Spawn(object ur)` and `Duplicate(object ur)` are the two merryfuns that clone the binding host's clonable from inside sandboxed Merry source. Both resolve via `merrynode`, extract the binding host's clonable from its object_name, and call `::clone_object(clonable)` -- the `::` escape past the local `SANDBOX(clone_object)` shadow, the same idiom `merrynode` applies for `::call_other`, `::destruct_object`, and `::new_object`. `Duplicate` additionally walks the source clone's registered state model (`export_state` resolves it via `SID->get_root_node(ob)`; `import_state` replays it on the fresh clone), so the returned clone carries the same schema-declared state as the source.

## Reference application

`examples/merry-app/` carries a working reference: a property + ur-bearing clonable, a marker lib, and a boot-time test driver that exercises the ancestry walk and the sandbox firing. The code in that directory is the canonical example -- accurate, compiling, and runnable. To deploy:

```sh
cp -R examples/merry-app src/usr/MerryApp
```

Then boot DGD against the configuration from `docs/getting-started.md` (`example.dgd`). The verify command in the example's README is `scripts/run-example.sh merry-app`, which drives a two-boot cycle: the cold boot runs phases 1 through 16, dumps a snapshot once `PERSIST SETUP OK` lands, and exits on its own; the second boot restarts against that snapshot, and the call_outs still pending from the first boot fire in turn, ending in phase 17's `PERSIST VERIFY OK`. The runner asserts 29 lines total in `src/usr/MerryApp/data/test-result.log`, starting with:

```
MerryApp:test: starting
MerryApp:test: ANCESTRY OK
MerryApp:test: SANDBOX OK
```

The full sentinel sequence, including the pending call_outs that resolve after the restart, is listed in `examples/merry-app/README.md`.

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

## Verification -- the merry-app sentinels

The example's boot driver (`examples/merry-app/sys/test.c`) proves the invocation surface live against a running kernel: an ancestry walk, the sandbox's deny list, the `Spawn` merryfun, `$delay()`'s continuation path, and the LabelCall script-space dispatch, each ending in a `MerryApp:test: <SENTINEL> OK` line. Run it with `scripts/run-example.sh merry-app`; for the per-phase walkthrough -- what each phase's code does and why -- see `examples/merry-app/README.md` "Phase notes".

| Claim | Phase | Sentinel |
|---|---|---|
| Ancestry walk -- `find_merry` resolves a script bound on an ur-parent | 1 | `ANCESTRY OK` |
| Sandbox firing -- a denied kfun in Merry source raises `not allowed in merry code` | 2 | `SANDBOX OK` |
| `Spawn` merryfun clones the binding host's clonable via the `::clone_object` escape | 3 | `SPAWN OK` |
| `$delay()` and the 5-arg mcontext dispatch -- a deferred continuation resumes and completes the write | 4 | `DELAY OK` |
| `LabelCall` and the script-space handler protocol -- a registered handler dispatches a named method | 5 | `LABELCALL OK` |

## What this example does NOT exercise

The earlier deferred list (Spawn/Duplicate, $delay, LabelCall/LabelRef) is now exercised -- Duplicate's full runtime path included, since phase 3b gives the clonable a registered state model -- and snapshot+restore survival of property-bound script clones is exercised by the dispatcher's own example (`examples/merry-app/sys/test.c` phases 16 and 17, walked in `docs/dispatcher.md` Persistence). For the dispatcher's smallest standalone introduction, see `signal-applications.md` + `examples/signal-app/`.

## Storage and round-trip

The reference application binds scripts to in-memory clones, and statedump survival of that binding needs no Schema or Vault participation: phases 16 and 17 of `examples/merry-app/sys/test.c` register an observer script on a `Thing` clone, `dump_state`, restart against the snapshot, and confirm the observer still fires -- no Schema registration, no on-disk Vault round-trip (`docs/dispatcher.md` Persistence walks the mechanism). `examples/merry-app/obj/thing.c` does register a `MerryApp:Thing` schema node, but that participation backs the `Duplicate` merryfun's state export/import, not the property-bound script's own statedump survival.

On-disk durability across a full data loss is a separate concern from statedump survival, and does need Schema/Vault participation. A Schema-registered version of the same thing would additionally have:

- `merry:*` attributes on the `MerryApp:Thing` schema_node typed as `lpc_obj` (so the marshaler can record the bound script clonable's program path on dump and re-resolve on restore);
- Vault registration via inheriting `~Vault/lib/vault_node` (the `examples/vault-app/` pattern), so the property tree survives an on-disk Vault round-trip.

The composite "scripted persistent object" is the natural next example -- composing `merry-app` and `vault-app` into a single demonstrator -- and is a natural follow-on.

## Where to next

- **[merry-language.md](merry-language.md)** -- the dialect itself: the four extensions over LPC, the compile pipeline, AST nodes, the merryfun catalog, and the full sandbox surface this document only summarizes.
- **[dispatcher.md](dispatcher.md)** -- the property-change dispatcher, which reuses this document's `merry:<mode>:<signal>` storage convention and ancestry walk for `merry:on:<path>:<timing>` observers.
- **[vault-applications.md](vault-applications.md)** -- persistent-state participation for a property-bearing object, including the on-disk Vault round-trip the Storage and round-trip section above describes.
- **[runtime-primitives.md](runtime-primitives.md)** -- sandboxed code-load and the ur-hierarchy as platform properties.
- **[`examples/merry-app/`](../examples/merry-app/)** -- the runnable reference application walked through above.
