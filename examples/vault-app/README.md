# Reference Vault application

A minimal Vault application that runs on top of eOS-kernellib. Demonstrates the platform contracts a property-bearing persistent object honors: schema-driven typed-property marshaling, on-disk XML round-trip, and Index-mediated name resolution. The full walkthrough lives in `docs/vault-applications.md`.

## Operations

- A `MyApp:Thing` clonable carries an `lpc_str` property (`label`), an `lpc_int` property (`count`), and an `lpc_obj` property (`peer`, a cross-object reference).
- At boot, the test driver clones a thing, sets properties, names it `MyApp:demo:thing1`, calls `Vault->store`, destructs the clone, then `Vault->spawn_one_by_name` reloads it from the XML on disk and asserts the property values round-trip.
- The reloaded object is also looked up via `Index->query_object(name)` to verify Vault's restoration registers the object's logical name.
- A second assertion set exercises the singleton storage shape: `sys/config.c` (a one-of-a-kind daemon) is stored as `<object program="...">` rather than `<clone .../>`. Three paths are asserted: store + re-import through the public Vault API against the loaded singleton (mutated live state loses to stored state); the cross-domain boundary (with the program unloaded, the Vault daemon cannot compile `/usr/MyApp/sys/config` -- kernel `compile_object` grants non-lib compiles only with write access to the path -- so the respawn is a no-op and the boot log carries an expected `[caught]` access trace); and the supported owning-domain respawn (the test driver, itself a vault node, calls its inherited `spawn_create_one` / `spawn_configure_one`, which compile the program in MyApp's own context and re-import the stored state).
- A final assertion pair exercises cross-object `lpc_obj` references: a thing's `peer` attribute pointing at another named thing stores as the literal `OBJ(<logical-name>)` and re-resolves through Index on import when the peer is loaded; with the peer unloaded, the import fails inside the Vault's configure step (caught internally, two expected `[caught]` traces), so the respawned object exists but carries a nil peer -- a dangling reference does not throw to the spawn caller.

## Deployment

Copy the directory into the kernel layer's `src/usr/` (the MyApp destination is the example's choice; pick any `/usr/<Name>/` that doesn't conflict):

```sh
cp -R examples/vault-app src/usr/MyApp
```

System/initd's `/usr/[A-Z]*/initd.c` iteration picks up the new domain automatically. The cross-domain inherit `~Vault/lib/vault_node` and the cross-domain clone of `~Schema/obj/schema_node` require both Vault and Schema to be globally readable -- the platform grants these in `src/usr/System/initd.c`.

## Verify

```sh
DGD_BIN=/path/to/dgd/bin/dgd scripts/run-example.sh vault-app
```

The runner does the clean-slate deploy, drives the boot cycle, and
asserts the sentinel count; boot output lands under `state/`. The
manual sequence it automates:

```sh
# Clean slate: remove any prior deploy and state, then redeploy.
rm -rf src/usr/MyApp
rm -f state/snapshot state/snapshot.old
cp -R examples/vault-app src/usr/MyApp

/path/to/dgd/bin/dgd example.dgd &
sleep 5
kill %1
cat src/usr/MyApp/data/test-result.log
```

Expected result-log contents:

```
MyApp:test: starting round-trip
MyApp:test: ROUND-TRIP OK
MyApp:test: SINGLETON OK
MyApp:test: XDOMAIN-RESPAWN-REJECT OK
MyApp:test: NODE-RESPAWN OK
MyApp:test: XREF OK
MyApp:test: XREF-DANGLING OK
```

The boot log additionally carries three expected `[caught]` traces: one `Access denied` from the cross-domain boundary assertion, and a `no object` pair (the raw error plus its XML-layer wrapper) from the dangling-reference assertion.

The on-disk artifacts land under the Vault daemon's storage root, `/usr/Vault/data/vault/` (`src/usr/Vault/data/vault/` on the host filesystem): the round-trip thing at `MyApp/demo/thing1.xml`, the singleton at `MyApp/config/main.xml`, and the cross-reference things under `MyApp/xref/`.

## Files

- `initd.c` -- domain initd; compiles `obj/thing` + `sys/test` at boot. Deliberately does not compile `sys/config`: the singleton respawn assertion is only meaningful when the program is not loaded.
- `lib/app.c` -- thin wrapper inheriting `~Vault/lib/vault_node`; daemons inherit this to participate in the Vault.
- `obj/thing.c` -- property-bearing clonable; carries `label` (string) + `count` (int) + `peer` (object reference).
- `sys/config.c` -- one-of-a-kind configuration daemon; carries `greeting` (string) + `limit` (int); exercises the singleton `<object>` storage shape.
- `sys/test.c` -- boot-time test driver; registers the `MyApp:Thing` + `MyApp:Config` schemas and runs the round-trip, singleton, and cross-reference assertions via `call_out("run_tests", 0)`.

## Notes

- The example uses a per-application schema (`MyApp:Thing`) registered at boot from `sys/test.c::create()` rather than relying on the core `Hierarchy` or `Entry` primitives. The core primitives describe structural relationships (ancestry, flat key-value), not typed application properties; per-application schemas are the natural surface for a typed property tree.
- The test driver writes assertion results to a sentinel file (`data/test-result.log`) rather than the boot log: `DRIVER->message()` requires kernel- or System-tier `previous_program`, which an application-tier daemon is not, and the platform's `sysLog` is still a no-op stub. When a real log facility lands, the driver can rely on `sysLog` alone.
