# Writing Vault applications

A Vault application on eOS-kernellib supplies a participating domain whose clones (or singletons) carry typed properties and round-trip to on-disk XML through the Vault daemon. The sections below show what the domain looks like, what a property-bearing clonable must implement, how the Schema layer types its properties, and how the test driver validates that the persistence cycle round-trips.

**Audience**: an application author building a persistent-state service on eOS-kernellib; comfortable with LPC syntax (or read `docs/lpc-essentials.md` first); has the platform running locally per `docs/getting-started.md`.

`docs/architecture.md` covers the capability tiers and the Schema / Marshal / Vault / Index subsystems that make typed persistence possible. `docs/persistence.md` covers the durable-state primitives a Vault-participating object inherits (atomic mutation, dump_state survival, hot reload). `docs/runtime-primitives.md` covers the platform properties.

## The participating-domain contract

The Vault daemon (`/usr/Vault/sys/vault`) stores any object that:

- carries a logical name via `/lib/util/named::set_object_name(name)` (Vault uses the logical name as both the on-disk path and the inverse-lookup key through `~Index/sys/index_daemon`);
- defines `query_state_root()` returning a Schema namespace:tag pair that names a registered `~Schema/obj/schema_node` (the marshaler walks that schema_node's attributes to extract the value tree);
- implements the per-attribute `query_<name>` getters and `set_<name>` setters that the schema declares.

Singletons (one-of-a-kind daemons) come from a master via `findOrLoad(program)`; clones come from `clone_object(program)`. The Vault stores both, distinguished on disk by `<object program="..."/>` vs `<clone program="..."/>` root elements.

## Reference application

`examples/vault-app/` carries a working reference implementation: a domain initd, a property-bearing clonable, a vault_node-inheriting lib, and a boot-time test driver that round-trips a thing through `Vault->store` + `Vault->spawn_one_by_name` and asserts the property tree matches. The code in that directory is the canonical example -- accurate, compiling, and runnable. To deploy it:

```sh
cp -R examples/vault-app src/usr/MyApp
```

Then sync the runtime tree (`scripts/setup-runtime.sh`) and boot DGD against `mva.dgd`. The verify command in the example's README cats `src/usr/MyApp/data/test-result.log` and expects six OK sentinels: `ROUND-TRIP OK`, the three singleton assertions (`SINGLETON OK`, `XDOMAIN-RESPAWN-REJECT OK`, `NODE-RESPAWN OK`), and the cross-reference pair (`XREF OK`, `XREF-DANGLING OK`).

The sections below explain what the reference application is doing and why. Read the code in `examples/vault-app/sys/test.c` alongside this document.

## Application layout

A minimal Vault application is four files plus an initd:

```text
src/usr/MyApp/
  initd.c           - domain initd; compiles obj/thing + sys/test at boot
  lib/
    app.c           - inherits ~Vault/lib/vault_node; participates in the Vault
  obj/
    thing.c         - property-bearing clonable
  sys/
    config.c        - one-of-a-kind configuration daemon (singleton storage)
    test.c          - boot-time test driver (inherits lib/app)
```

`lib/app.c` is the thin layer over `~Vault/lib/vault_node`. Any application daemon that needs to register a filesystem root with the Vault inherits this lib and calls `::create("/usr/MyApp/data/<store>")` from its own create. The participating domain's per-daemon state lives under that root as `<store>/<name-with-colons-as-slashes>.xml`.

## Boot-order constraint

The system loads domains alphabetically (after the TLS / HTTP / LPC prefix in `/usr/System/initd::create`). Applications whose name sorts before `Vault` or `Schema` see those daemons as not-yet-loaded during their own initd. The reference application's `sys/test.c::create` defers everything to `call_out("setup_and_run", 0)` so registration and the test run fire AFTER every domain's initd has returned and both Vault and Schema daemons are up:

```c
static void create()
{
    call_out("setup_and_run", 0);
}

static void setup_and_run()
{
    ::create("/usr/MyApp/data/things");
    set_object_name("MyApp:TestDriver");
    register_thing_schema();
    run_tests();
}
```

A real application with a stable boot-order position (alphabetically after Vault) can do the registration inline in `create()`; the call_out pattern is the boot-order-agnostic form.

## Property-bearing clonable

`obj/thing.c` carries one string-typed `label` and one int-typed `count`. The schema layer drives marshaling through the per-attribute `query_<name>` getters; import drives `set_<name>` setters:

```c
inherit "/lib/util/named";

private string _label;
private int _count;

static void create() { }   /* clones only -- master is a template */

string query_state_root() { return "MyApp:Thing"; }

string query_label()  { return _label; }
int    query_count()  { return _count; }

void set_label(string val) { _label = val; }
void set_count(int val)    { _count = val; }
```

`query_state_root()` returns the schema name -- the `(namespace, tag)` pair that the Vault daemon looks up via `~Schema/sys/schema_daemon::get_node()` to discover the marshaling shape.

## Schema registration

The `MyApp:Thing` schema is registered at boot from `sys/test.c::register_thing_schema()`:

```c
private void register_thing_schema()
{
    object node;

    node = clone_object("/usr/Schema/obj/schema_node");
    node->set_name("MyApp", "Thing");
    node->add_attribute("label", "lpc_str", "query_label");
    node->add_attribute("count", "lpc_int", "query_count");
    node->add_callback("set_label", "label");
    node->add_callback("set_count", "count");
}
```

Each `add_attribute(attr, type, query_method)` declares one XML attribute on the `<MyApp:Thing/>` element; the marshaler invokes `query_method` on the object to extract the value. Each `add_callback(setter, ...attr-names...)` declares one setter the import path calls with the value(s) of the named attributes.

The five types the lifted dtd_daemon supports are `lpc_str`, `lpc_int`, `lpc_flt`, `lpc_obj`, and `lpc_mixed`. Of these, `lpc_mixed` is currently degraded (round-trip is not exact for arrays / mappings); the typed primitives round-trip cleanly.

## On-disk shape

The store path emits XML at `<root>/<colon-name-as-slashes>.xml`. For the reference application, storing `MyApp:demo:thing1` lands at `<vault-data>/MyApp/demo/thing1.xml` with content:

```xml
<clone program="/usr/MyApp/obj/thing">
  <MyApp:Thing label="hello" count="42"/>
</clone>
```

The root element is `<clone>` for clones (the discriminator the Vault uses to choose `clone_object` vs `findOrLoad` on the spawn path). Singletons emit `<object>` instead.

## The round-trip

The boot-time test driver exercises the full cycle and asserts at each step:

```c
static void run_tests()
{
    object thing, reloaded, indexed;

    thing = clone_object("/usr/MyApp/obj/thing");
    thing->set_object_name("MyApp:demo:thing1");
    thing->set_label("hello");
    thing->set_count(42);
    Vault->store(thing);
    destruct_object(thing);

    Vault->spawn_one_by_name("MyApp:demo:thing1");
    reloaded = find_named("MyApp:demo:thing1");
    if (reloaded->query_label() != "hello") return /* FAIL */;
    if (reloaded->query_count() != 42)      return /* FAIL */;

    indexed = INDEX->query_object("MyApp:demo:thing1");
    if (indexed != reloaded) return /* FAIL */;
    /* PASS */
}
```

`Vault->store(thing)` exports the typed-property tree through `~Marshal/XmlBinding/lib/stateimpex`, wraps it in the appropriate root element, and writes the XML to disk. `Vault->spawn_one_by_name(name)` reads the file, clones a fresh thing (because the root is `<clone>`), registers the logical name via `set_object_name`, then imports the property tree.

`find_named(name)` is the convenience wrapper from `/lib/util/named` over `~Index/sys/index_daemon::query_object`. The Index lookup is the inverse direction of `set_object_name`: any object that called `set_object_name(name)` is resolvable by that name through Index.

## The singleton path

One-of-a-kind daemons store as `<object program="..."/>`. The test driver exercises this shape with `sys/config.c` across three assertions:

- **Store + re-import against a loaded singleton.** The driver compiles the config daemon, names it, stores it, then mutates the live state and calls `Vault->spawn_one_by_name`. Because the program is loaded and named, the spawn re-imports the stored property tree onto the existing object: the stored values win over the mutation.
- **The cross-domain compile boundary.** With the program destructed, the Vault daemon's `findOrLoad` cannot bring it back: kernel `compile_object` grants non-lib compiles only to callers with write access to the path, and the Vault has none over `/usr/MyApp`. The error is caught inside the Vault's spawn internals (an `Access denied [caught]` trace in the boot log), so the spawn returns normally and the program simply stays unloaded. Application code must not rely on `Vault->spawn_one_by_name` to load another domain's singleton from scratch.
- **The owning-domain respawn.** The supported way to respawn an unloaded singleton is from inside its own domain: any vault node (the test driver is one) inherits `spawn_create_one` / `spawn_configure_one` from `~Vault/lib/vault_node`, and calling them with the stored XML compiles the program in the owning domain's own context, re-binds the logical name, and re-imports the state.

## Cross-object references

The MyApp:Thing schema types its `peer` attribute as `lpc_obj`. On export, an object-valued attribute serializes as the literal `OBJ(<name>)`, where the name is the target's logical name when it has one (its `query_object_name`), or its LPC object path otherwise. On import, the literal resolves path-first, then through Index for logical names -- so a reference to another named object survives the disk round-trip as long as the target is loaded (or resolvable by name) at import time.

Two boundaries to design around:

- **Dangling references do not throw to the spawn caller.** If the target resolves to nothing at import time, the type conversion errors inside the Vault's configure step, which is caught internally. The referencing object still spawns (the create step already succeeded) and carries whatever attributes imported cleanly, with the dangling reference left nil. Applications that need referential integrity must order their respawns so targets load before referrers, or re-check references after a bulk respawn.
- **Vault-respawned clones are owned by the Vault daemon.** `clone_object` runs in the Vault's context during a respawn, so the kernel's owner-gated `destruct_object` refuses the application domain's attempt to destruct an object the Vault respawned -- even though the object logically belongs to the application. An application that needs to retire Vault-respawned objects must route the destruct through code the owner can call (for example, a self-destruct method on the object itself).

## Cross-domain access

Inheriting `~Vault/lib/vault_node` from a domain other than Vault requires Vault to be globally readable. The platform's `src/usr/System/initd.c` grants `set_global_access("Vault", TRUE)` alongside the Schema / XML / Marshal / Index grants. Cloning `/usr/Schema/obj/schema_node` from a non-Schema domain works without a separate grant (clone_object has no access check against the path's domain), but the `set_global_access("Schema", TRUE)` grant is required for the schema_daemon constants to inherit cleanly when the application's own inherit chain pulls them in.

## What this example does not exercise

- **Statedump survival**. The DGD `dump_state` cycle (write snapshot, restart against the snapshot, verify property state survives) is part of the Vault's value proposition but is awkward to exercise in a single boot. A multi-boot test harness is a natural follow-on once the platform grows a CI shape.
- **Hot reload**. Recompiling thing.c via `compile_object` updates the clonable in place; existing clones survive the recompile and dispatch to the new code on next method call. Demonstrating this requires admin_console interaction; out of scope for a boot-time assertion driver.

## Notes

- The example uses `DRIVER->message()`-style logging via a sentinel file at `/usr/MyApp/data/test-result.log` because the platform's `sysLog` is currently a no-op stub. When a real log facility lands, the sentinel file approach can be dropped.
- The test driver's `setup_and_run` uses `make_dir("/usr/MyApp/data")` before the first `write_file` because the directory may not exist on first boot. Vault's own `paveWay` helper handles this for its own data path; application code does the same explicitly.
